// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "ManifestDonor.h"

#include "Logger.h"
#include "RuntimeHttp.h"
#include "config/Settings.h"

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

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

}
