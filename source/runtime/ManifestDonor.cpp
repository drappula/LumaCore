// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "ManifestDonor.h"

#include "Logger.h"
#include "RuntimeHttp.h"
#include "config/Settings.h"
#include "hooks/client/ManifestBind.h"

#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Steam/Types.h"

#include "steam_messages.pb.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ManifestDonor {

namespace {

    struct SentReq {
        uint32_t depot = 0;
        uint64_t gid = 0;
    };

    std::mutex g_lock;
    std::map<uint64_t, SentReq> g_sent;
    constexpr size_t kMaxSentTracked = 4096;

    // Same baked-in base as ManifestCache: infrastructure, not a user
    // setting. SetArchiveBase overrides it (trailing slashes trimmed).
    constexpr const char* kDefaultArchiveBase = "https://manifest.luastools.xyz";
    std::string g_archiveBase;

    std::string BaseUrl() {
        std::string base;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            base = g_archiveBase.empty() ? kDefaultArchiveBase : g_archiveBase;
        }
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base;
    }

    void SubmitOne(uint32_t depot, uint64_t gid, uint64_t code) {
        const std::string url = BaseUrl() + "/manifestcode/submit";
        const std::string body =
            std::to_string(depot) + ':' + std::to_string(gid) + ':' +
            std::to_string(code) + '\n';
        RuntimeHttp::Response r =
            RuntimeHttp::Post(url, body, {"Content-Type: text/plain"});
        // The code itself is never logged; depot/gid are enough to audit.
        if (!r.networkError && r.status == 200)
            LOG_MANIFESTCH_INFO("ManifestDonor: submitted depot={} gid={}", depot, gid);
        else
            LOG_MANIFESTCH_WARN("ManifestDonor: submit failed depot={} gid={} status={} err='{}'",
                                depot, gid, r.status, r.diagnostic);
    }

    // ── Active probe state ─────────────────────────────────────────
    //
    // Mirrors BST Hooks_NetPacket_ManifestProbe read-only: the websocket
    // object + send trampoline are borrowed from the TX hook, one real
    // outbound header is kept whole as a template (steamid/session come
    // along automatically), and originated jobids come from a high range
    // Steam's own counter can never reach.

    // BST convention, used verbatim: well clear of Steam's own jobids,
    // which count up from small values.
    constexpr uint64_t kProbeJobIdBase = 0x7E51'0000'0000'0000ull;

    // A disconnect between send and reply must never leave a caller
    // blocked forever: the loop waits with this budget (not an unbounded
    // get), and stale entries are swept on every new request.
    constexpr auto kProbeTimeout = std::chrono::seconds(30);

    void*       g_ws   = nullptr;
    SendFrameFn g_send = nullptr;
    std::vector<uint8_t> g_hdrTemplate;
    std::mutex  g_probeLock;
    uint64_t    g_nextJobId = kProbeJobIdBase;

    struct Pending {
        uint32_t appId = 0;
        uint32_t depot = 0;
        uint64_t gid   = 0;
        std::promise<uint64_t> result;
        std::chrono::steady_clock::time_point sentAt{};
    };
    std::map<uint64_t, Pending> g_pending;

    // Caller holds g_probeLock.
    void ExpireStale() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_pending.begin(); it != g_pending.end(); ) {
            if (now - it->second.sentAt > kProbeTimeout) {
                LOG_MANIFESTCH_WARN("ManifestProbe: no reply for depot={} (jobid={}) after {}s",
                                    it->second.depot, it->first,
                                    static_cast<long long>(kProbeTimeout.count()));
                it->second.result.set_value(0);
                it = g_pending.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── Wanted-list loop state ─────────────────────────────────────

    std::atomic<bool> g_running{false};
    uint32_t g_mintedThisSession = 0;   // worker thread only

    // Wanted list is large (~5 MB): pulled on its own slow cadence and
    // reused across the faster mint cycles. Worker thread only, except
    // where noted; still mutex-guarded for a clean shutdown story.
    struct Wanted {
        uint32_t appId = 0;   // may legitimately be 0
        uint32_t depot = 0;
        uint64_t gid   = 0;
    };
    std::vector<Wanted> g_wantedCache;
    std::chrono::steady_clock::time_point g_wantedFetchedAt{};
    std::mutex g_wantedLock;

    // The wanted list is served whole and edge-cached: raise the body cap
    // far above the 8 MiB default so a large list is never silently
    // truncated (BST uses the same 32 MiB bound).
    constexpr uint32_t kMaxWantedBytes = 32u * 1024u * 1024u;

    // Parses one "app:depot:gid" line. False on anything unexpected so a
    // malformed row can never become a request for depot 0.
    bool ParseWantedLine(std::string_view line, Wanted& out) {
        auto field = [](std::string_view s, uint64_t& v) {
            if (s.empty()) return false;
            const char* beg = s.data();
            const char* end = s.data() + s.size();
            const auto res = std::from_chars(beg, end, v);
            // Full-consumption check (stricter than BST, which accepts a
            // numeric prefix): "480:200:1:extra" must not become gid 1.
            return res.ec == std::errc{} && res.ptr == end;
        };
        const size_t c1 = line.find(':');
        if (c1 == std::string_view::npos) return false;
        const size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string_view::npos) return false;
        uint64_t app = 0, depot = 0, gid = 0;
        if (!field(line.substr(0, c1), app))                 return false;
        if (!field(line.substr(c1 + 1, c2 - c1 - 1), depot)) return false;
        if (!field(line.substr(c2 + 1), gid))                return false;
        if (!depot || !gid) return false;   // app 0 is valid; these are not
        out.appId = static_cast<uint32_t>(app);
        out.depot = static_cast<uint32_t>(depot);
        out.gid   = gid;
        return true;
    }

    std::vector<Wanted> FetchWanted() {
        std::vector<Wanted> wanted;
        // Pull the WHOLE missing set (no ?limit): every donor sees
        // everything and mints only what Steam grants a code for.
        const std::string url = BaseUrl() + "/manifestwanted";
        RuntimeHttp::Response r =
            RuntimeHttp::GetLarge(url, L"LumaCore-RuntimeHttp/1.0",
                                  kMaxWantedBytes, 30'000);
        if (r.networkError || r.status != 200) {
            LOG_MANIFESTCH_DEBUG("ManifestDonor: wanted list unavailable (status={} err='{}')",
                                 r.status, r.diagnostic);
            return wanted;
        }
        size_t start = 0;
        while (start < r.body.size()) {
            size_t end = r.body.find('\n', start);
            if (end == std::string::npos) end = r.body.size();
            std::string_view line(r.body.data() + start, end - start);
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == ' '))
                line.remove_suffix(1);
            Wanted w;
            if (!line.empty() && ParseWantedLine(line, w))
                wanted.push_back(w);
            else if (!line.empty())
                LOG_MANIFESTCH_DEBUG("ManifestDonor: ignoring malformed wanted line");
            start = end + 1;
        }
        LOG_MANIFESTCH_DEBUG("ManifestDonor: {} wanted entr(ies)", wanted.size());
        return wanted;
    }

    // Existence probe: is this manifest already archived? True ONLY on a
    // definitive 200 — anything else falls through to false so a network
    // blip can never make us skip a real gap (better to re-mint than to
    // leave a manifest missing).
    bool AlreadyCached(uint32_t depot, uint64_t gid) {
        const std::string url =
            BaseUrl() + "/m/" + std::to_string(depot) + "/" + std::to_string(gid);
        RuntimeHttp::Response r = RuntimeHttp::Head(url);
        return !r.networkError && r.status == 200;
    }

    // Refresh the cache when stale (or never fetched). Shuffled on every
    // refresh so donors do not all march the list top-down in lockstep.
    void RefreshWantedIfStale(int wantedRefreshSecs) {
        const long long cadence =
            wantedRefreshSecs > 0 ? wantedRefreshSecs : 300;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_wantedLock);
        const bool never = g_wantedFetchedAt.time_since_epoch().count() == 0;
        const auto ageSecs =
            std::chrono::duration_cast<std::chrono::seconds>(now - g_wantedFetchedAt).count();
        if (!never && ageSecs < cadence) return;
        std::vector<Wanted> fresh = FetchWanted();
        if (!fresh.empty()) {
            static std::mt19937 rng{std::random_device{}()};
            std::shuffle(fresh.begin(), fresh.end(), rng);
            g_wantedCache = std::move(fresh);
            g_wantedFetchedAt = now;
        } else if (never) {
            // First fetch failed outright: mark the attempt so the cadence
            // is honoured before retrying instead of hammering every cycle.
            g_wantedFetchedAt = now;
        }
    }

    void SubmitBatch(const std::string& body, size_t count) {
        const std::string url = BaseUrl() + "/manifestcode/submit";
        RuntimeHttp::Response r =
            RuntimeHttp::Post(url, body, {"Content-Type: text/plain"});
        if (!r.networkError && r.status == 200)
            LOG_MANIFESTCH_INFO("ManifestDonor: submitted {} code(s)", count);
        else
            LOG_MANIFESTCH_WARN("ManifestDonor: batch submit failed for {} code(s) status={} err='{}'",
                                count, r.status, r.diagnostic);
    }

    void RunCycle() {
        // No send context (Steam not connected yet, or the TX hook never
        // installed): idle with zero traffic — no wanted fetch, no HEADs,
        // no originated frames.
        if (!ManifestDonor::ProbeReady()) {
            LOG_MANIFESTCH_DEBUG("ManifestDonor: no probe send context yet, skipping cycle");
            return;
        }
        const int maxPerCycle = Settings::donate.maxMintsPerCycle;
        const int minGapMs    = Settings::donate.minMintIntervalMs;
        const int maxSession  = Settings::donate.maxMintsPerSession;
        if (maxPerCycle <= 0) {
            LOG_MANIFESTCH_DEBUG("ManifestDonor: max_mints_per_cycle={} disables minting",
                                 maxPerCycle);
            return;
        }
        // maxMintsPerSession == 0 means unlimited (whole session).
        if (maxSession > 0 && g_mintedThisSession >= static_cast<uint32_t>(maxSession)) {
            LOG_MANIFESTCH_DEBUG("ManifestDonor: session cap reached ({}), idle until restart",
                                 maxSession);
            return;
        }

        RefreshWantedIfStale(Settings::donate.wantedRefreshSecs);

        std::vector<Wanted> wanted;
        {
            std::lock_guard<std::mutex> lock(g_wantedLock);
            wanted = g_wantedCache;
        }
        if (wanted.empty()) return;

        std::string body;
        size_t minted = 0, cached = 0, refused = 0;
        bool attempted = false;   // has any mint been attempted yet this cycle?

        for (const Wanted& w : wanted) {
            if (!g_running) break;
            if (minted >= static_cast<size_t>(maxPerCycle)) break;
            if (maxSession > 0 &&
                g_mintedThisSession >= static_cast<uint32_t>(maxSession)) break;

            // Already archived (likely supplied by another donor since the
            // list was fetched): skip the Steam call entirely. The probe is
            // a free edge hit, and the skip happens before the mint spacing
            // so it costs no time.
            if (AlreadyCached(w.depot, w.gid)) { ++cached; continue; }

            // Spacing goes before each mint rather than after it: the gap
            // between consecutive Steam calls is identical either way, but
            // nothing is left sleeping after the last one — a trailing wait
            // is pure delay on codes that are already rotating.
            if (attempted && minGapMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(minGapMs));
                if (!g_running) break;
            }
            attempted = true;

            // The server may legitimately send app_id 0; it usually
            // satisfies Steam's access check but not always (eresult=8
            // InvalidParam has been observed on app_id=0 for an owned
            // depot), so prefer a real app id whenever one is known.
            // Ownership itself is NOT pre-gated: LumaCore has no
            // license-list enumeration (unlike BST's GetOwnedDepots), so
            // Steam's reply is the ownership oracle — a refused mint
            // reports nothing, and only successful codes are uploaded.
            uint32_t appId = w.appId;
            if (!appId) {
                AppId_t knownApp = 0;
                uint64_t knownGid = 0;
                if (ManifestBind::LookupDepot(w.depot, knownApp, knownGid) && knownApp) {
                    appId = knownApp;
                    LOG_MANIFESTCH_DEBUG("ManifestDonor: depot={} app_id 0 -> {} (from BuildDepotDependency)",
                                         w.depot, appId);
                }
            }

            const auto mintedAt = std::chrono::steady_clock::now();
            auto fut = ManifestDonor::Request(appId, w.depot, w.gid);
            ++g_mintedThisSession;
            uint64_t code = 0;
            if (fut.wait_for(kProbeTimeout) == std::future_status::ready)
                code = fut.get();
            else
                LOG_MANIFESTCH_WARN("ManifestDonor: probe timed out for depot={} gid={}",
                                    w.depot, w.gid);

            if (!code) {
                ++refused;
            } else {
                const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - mintedAt).count();
                body += std::to_string(w.depot) + ':' + std::to_string(w.gid) + ':' +
                        std::to_string(code) + '\n';
                ++minted;
                LOG_MANIFESTCH_DEBUG("ManifestDonor: minted depot={} gid={} in {}ms",
                                     w.depot, w.gid, ageMs);
            }
        }

        LOG_MANIFESTCH_INFO("ManifestDonor: cycle done — {} minted, {} already archived, "
                            "{} refused by Steam ({} this session)",
                            minted, cached, refused, g_mintedThisSession);

        // One batch the moment the cycle ends: codes rotate within
        // minutes, so anything delaying this wastes the mint.
        if (minted) SubmitBatch(body, minted);
    }

    void WorkerMain() {
        LOG_MANIFESTCH_INFO("ManifestDonor: loop started");
        while (g_running) {
            // Re-read every cycle so toggling [donate] takes effect without
            // a restart. Disabled means zero traffic: no HTTP, no probes.
            if (Settings::donate.enabled)
                RunCycle();
            const int interval =
                Settings::donate.intervalSecs > 0 ? Settings::donate.intervalSecs : 30;
            // Short slices so Stop() is honoured promptly rather than
            // after a full interval.
            for (int i = 0; i < interval && g_running; ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        LOG_MANIFESTCH_INFO("ManifestDonor: loop stopped");
    }

}

void SetArchiveBase(std::string url) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_archiveBase = std::move(url);
}

void NoteSent(uint64_t jobId, uint32_t depotId, uint64_t gid) {
    if (!Settings::donate.enabled) return;
    if (!jobId || !depotId || !gid) return;
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sent.find(jobId);
    if (it == g_sent.end() && g_sent.size() >= kMaxSentTracked) return;
    g_sent[jobId] = SentReq{depotId, gid};
}

void NoteRecvOk(uint64_t jobId, uint64_t code) {
    if (!Settings::donate.enabled) return;
    if (!jobId || !code) return;
    SentReq sent{};
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_sent.find(jobId);
        if (it == g_sent.end()) return;
        sent = it->second;
        g_sent.erase(it);
    }
    if (!sent.depot || !sent.gid) return;

    // Fire-and-forget: the caller is on Steam's network thread and must
    // not block on an HTTP round trip. No existence probe first (unlike
    // the active donor loop): a captured code is free — it was already
    // minted by a real download — and the server dedups cheaply.
    LOG_MANIFESTCH_DEBUG("ManifestDonor: captured genuine code for depot={} gid={}",
                         sent.depot, sent.gid);
    std::thread([depot = sent.depot, gid = sent.gid, code]() {
        SubmitOne(depot, gid, code);
    }).detach();
}

void CaptureSendContext(void* ws, SendFrameFn send,
                        const uint8_t* hdr, uint32_t hdrLen) {
    std::lock_guard<std::mutex> lock(g_probeLock);
    if (ws)   g_ws   = ws;
    if (send) g_send = send;
    if (g_hdrTemplate.empty() && hdr && hdrLen)
        g_hdrTemplate.assign(hdr, hdr + hdrLen);
}

std::future<uint64_t> Request(uint32_t appId, uint32_t depotId, uint64_t gid) {
    // Every early exit resolves the promise with 0 rather than dropping
    // it, so a caller waiting on the future is never left hanging on a
    // request that was never sent.
    std::promise<uint64_t> promise;
    std::future<uint64_t> future = promise.get_future();
    auto failWith = [&promise, &future]() -> std::future<uint64_t> {
        promise.set_value(0);
        return std::move(future);
    };
    if (!Settings::donate.enabled) return failWith();
    if (!depotId || !gid) return failWith();

    std::vector<uint8_t> hdrTemplate;
    void*     ws    = nullptr;
    SendFrameFn send = nullptr;
    uint64_t  jobId = 0;
    {
        std::lock_guard<std::mutex> lock(g_probeLock);
        ExpireStale();
        if (!g_ws || !g_send || g_hdrTemplate.empty()) {
            LOG_MANIFESTCH_WARN("ManifestProbe: no send context captured yet "
                                "(websocket={}, sendFn={}, header={} bytes) - is Steam logged in?",
                                g_ws != nullptr, g_send != nullptr,
                                g_hdrTemplate.size());
            return failWith();
        }
        hdrTemplate = g_hdrTemplate;
        ws          = g_ws;
        send        = g_send;
        jobId       = ++g_nextJobId;
    }

    CMsgProtoBufHeader hdr;
    if (!hdr.ParseFromArray(hdrTemplate.data(),
                            static_cast<int>(hdrTemplate.size()))) {
        LOG_MANIFESTCH_WARN("ManifestProbe: header template failed to parse");
        return failWith();
    }
    hdr.set_target_job_name("ContentServerDirectory.GetManifestRequestCode#1");
    hdr.set_jobid_source(jobId);
    hdr.clear_jobid_target();

    CContentServerDirectory_GetManifestRequestCode_Request req;
    req.set_app_id(appId);
    req.set_depot_id(depotId);
    req.set_manifest_id(gid);

    // Frame layout is byte-identical in structure to BST's probe frame
    // and to LumaCore's own PacketPool<false>::Build: MsgHdr, then the
    // serialised header, then the serialised body.
    const uint32_t cbHdr  = static_cast<uint32_t>(hdr.ByteSizeLong());
    const uint32_t cbBody = static_cast<uint32_t>(req.ByteSizeLong());
    std::vector<uint8_t> frame(sizeof(MsgHdr) + cbHdr + cbBody);

    auto* mhdr = reinterpret_cast<MsgHdr*>(frame.data());
    mhdr->eMsg = static_cast<EMsg>(
        static_cast<uint32_t>(k_EMsgServiceMethodCallFromClient) | kMsgHdrProtoFlag);
    mhdr->headerLength = cbHdr;

    if (!hdr.SerializeToArray(frame.data() + sizeof(MsgHdr), static_cast<int>(cbHdr)) ||
        !req.SerializeToArray(frame.data() + sizeof(MsgHdr) + cbHdr, static_cast<int>(cbBody))) {
        LOG_MANIFESTCH_WARN("ManifestProbe: failed to serialise request frame");
        return failWith();
    }

    // Registered before sending: the reply can arrive on another thread
    // the instant the frame goes out, and it must find the entry there.
    {
        std::lock_guard<std::mutex> lock(g_probeLock);
        Pending& p    = g_pending[jobId];
        p.appId       = appId;
        p.depot       = depotId;
        p.gid         = gid;
        p.result      = std::move(promise);
        p.sentAt      = std::chrono::steady_clock::now();
    }

    LOG_MANIFESTCH_INFO("ManifestProbe send: app={} depot={} gid={} jobid={} ({} bytes)",
                        appId, depotId, gid, jobId, frame.size());

    // Direct trampoline call bypasses the TX hook, so HandleSend never
    // sees this frame: no provider fetch, no NoteSent entry. The reply
    // still passes through HandleRecv, which routes probe jobids here.
    if (!send(ws, k_eWebSocketOpCode_Binary,
              frame.data(), static_cast<uint32_t>(frame.size()))) {
        LOG_MANIFESTCH_WARN("ManifestProbe: send returned false for jobid={}", jobId);
        std::lock_guard<std::mutex> lock(g_probeLock);
        auto it = g_pending.find(jobId);
        if (it != g_pending.end()) {
            it->second.result.set_value(0);
            g_pending.erase(it);
        }
    }
    return future;
}

bool HandleProbeRecv(const uint8_t* body, uint32_t bodyLen,
                     const uint8_t* hdr, uint32_t hdrLen) {
    CMsgProtoBufHeader hdrMsg;
    if (!hdrMsg.ParseFromArray(hdr, static_cast<int>(hdrLen)) ||
        !hdrMsg.has_jobid_target())
        return false;

    const uint64_t jobId = hdrMsg.jobid_target();
    if (jobId < kProbeJobIdBase) return false;   // cheap reject before locking

    uint32_t appId = 0, depot = 0;
    uint64_t gid = 0;
    std::promise<uint64_t> result;
    {
        std::lock_guard<std::mutex> lock(g_probeLock);
        auto it = g_pending.find(jobId);
        if (it == g_pending.end()) return false;
        appId  = it->second.appId;
        depot  = it->second.depot;
        gid    = it->second.gid;
        result = std::move(it->second.result);
        g_pending.erase(it);
    }

    CContentServerDirectory_GetManifestRequestCode_Response resp;
    if (!resp.ParseFromArray(body, static_cast<int>(bodyLen))) {
        LOG_MANIFESTCH_WARN("ManifestProbe recv: failed to parse response for jobid={}", jobId);
        result.set_value(0);
        return true;
    }

    const uint64_t code =
        resp.has_manifest_request_code() ? resp.manifest_request_code() : 0;
    const int32_t res = hdrMsg.has_eresult() ? hdrMsg.eresult() : 0;

    if (code) {
        LOG_MANIFESTCH_INFO("ManifestProbe recv: app={} depot={} gid={} -> code minted (eresult={})",
                            appId, depot, gid, res);
    } else {
        // Non-OK eresult normally means the depot is not licensed to this
        // account (the ownership oracle). eresult 8 (InvalidParam) has been
        // observed on app_id=0 for an owned depot, so a failure here is not
        // proof of missing ownership — just a refused mint.
        LOG_MANIFESTCH_WARN("ManifestProbe recv: app={} depot={} gid={} -> NO CODE (eresult={})",
                            appId, depot, gid, res);
    }
    result.set_value(code);
    return true;
}

bool IsProbeJobId(uint64_t jobId) {
    return jobId >= kProbeJobIdBase;
}

bool ProbeReady() {
    std::lock_guard<std::mutex> lock(g_probeLock);
    return g_ws && g_send && !g_hdrTemplate.empty();
}

void Start() {
    if (g_running.exchange(true)) {
        LOG_MANIFESTCH_WARN("ManifestDonor: loop already running");
        return;
    }
    // The worker is started even when [donate] is currently disabled so
    // that enabling it at runtime (config reload) takes effect without a
    // Steam restart; while disabled the loop idles with zero traffic.
    std::thread(WorkerMain).detach();
}

void Stop() {
    g_running = false;
}

}
