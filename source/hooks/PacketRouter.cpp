// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "PacketRouter.h"
#include "SteamCapture.h"
#include "RichPresence.h"
#include "Macros.h"
#include "entry.h"
#include "utils/Ticket.h"
#include "utils/Hash.h"
#include "utils/ManifestFetch.h"
#include <mutex>
#include <unordered_map>

#include "steam_messages.pb.h"

// ▌▌ LumaCore ▌ WIRE ▌ Shared infrastructure
// ▌▌
namespace {

    // 6.2.4 hotfix: bumped from 8092 to 262144 (256 KB) to accommodate
    // big achievement schemas. Games like Black Myth: Wukong (147 KB),
    // LEGO Batman, Schedule I etc. ship 50+ achievements with 16+
    // localized strings each — the modified body easily exceeds 8 KB
    // even after clear_stats(). With the old 8 KB ceiling the response
    // either truncated to garbage (corrupted Steam's local schema
    // cache) or fell through to pass-through (let dummy-account
    // unlocks bleed into the local cache). 256 KB covers every
    // achievement-related response we've seen on the wire.
    constexpr uint32 kMaxBodySize   = 262144;
    constexpr uint32 kMaxHdrSize    = 1024;
    constexpr uint32 kMaxPacketSize = 8 + kMaxHdrSize + kMaxBodySize;
    constexpr int    kPacketPoolSize = 8;

    static std::mutex g_RxLock;
    static std::mutex g_TxLock;

    // ── Incoming (RecvPkt) packet pool ───────────────────────
    uint8  g_RxBody[kMaxBodySize];
    uint32 g_RxBodyLen   = 0;
    uint8  g_RxHdr[kMaxHdrSize];
    uint32 g_RxHdrLen    = 0;
    bool   g_PatchRx = false;
    bool   g_PatchRxHdr  = false;
    bool   g_BodyShrunk = false;
    uint32 g_RxBodySize    = 0;
    uint8  g_RxPool[kPacketPoolSize][kMaxPacketSize];
    int    g_RxPoolIdx = 0;

    // ── Outgoing (BBuildAndAsyncSendFrame) — same pattern ───────
    uint8  g_TxBody[kMaxBodySize];
    uint32 g_TxBodyLen = 0;
    bool   g_PatchTx = false;
    uint8  g_TxPool[kPacketPoolSize][kMaxPacketSize];
    int    g_TxPoolIdx = 0;

    // ── EMsg -> name lookup  ─────────────────────────
    using PchMsgNameFromEMsg_t = char*(*)(EMsg);
    PchMsgNameFromEMsg_t oPchMsgNameFromEMsg = nullptr;

    inline const char* EmsgName(EMsg eMsg) {
        if (oPchMsgNameFromEMsg) return oPchMsgNameFromEMsg(eMsg);
        return "?";
    }


    // ── Packet layout ──────────────────────────────────────────
    inline bool DecodeFrame(const uint8* data, uint32 size,
                          EMsg& eMsg, const uint8*& pHdr, uint32& cbHdr,
                          const uint8*& pBody, uint32& cbBody)
    {
        if (!data || size < sizeof(MsgHdr)) {
        fail:
            eMsg = static_cast<EMsg>(0);
            cbHdr = 0;
            pHdr = nullptr;
            pBody = nullptr;
            cbBody = 0;
            return false;
        }
        const MsgHdr* hdr = reinterpret_cast<const MsgHdr*>(data);
        if (!(hdr->eMsg & kMsgHdrProtoFlag)) goto fail;

        eMsg  = static_cast<EMsg>(hdr->eMsg & ~kMsgHdrProtoFlag);
        cbHdr = hdr->headerLength;
        uint32 off = sizeof(MsgHdr) + cbHdr;
        if (off > size) goto fail;
        pHdr   = data + sizeof(MsgHdr);
        pBody  = data + off;
        cbBody = size - off;
        return true;
    }

    // ── Incoming: replace header and/or body (ring-buffer pool) ──
    inline void PatchRecvFrame(CNetPacket* p,
                                  const uint8* pNewHdr, uint32 cbNewHdr,
                                  const uint8* pNewBody, uint32 cbNewBody)
    {
        uint32 newSize = sizeof(MsgHdr) + cbNewHdr + cbNewBody;
        if (newSize > sizeof(g_RxPool[0])) return;

        std::lock_guard<std::mutex> lock(g_RxLock);
        uint8* buf = g_RxPool[g_RxPoolIdx];
        const MsgHdr* orig = reinterpret_cast<const MsgHdr*>(p->m_pubData);
        MsgHdr* out = reinterpret_cast<MsgHdr*>(buf);
        out->eMsg         = orig->eMsg;
        out->headerLength = cbNewHdr;
        memcpy(buf + sizeof(MsgHdr), pNewHdr, cbNewHdr);
        if (cbNewBody)
            memcpy(buf + sizeof(MsgHdr) + cbNewHdr, pNewBody, cbNewBody);
        p->m_pubData = buf;
        p->m_cubData = newSize;

        g_RxPoolIdx = (g_RxPoolIdx + 1) % kPacketPoolSize;
    }

    // ── Outgoing: assemble modified packet (ring-buffer pool) ────
    inline uint8* PatchSendFrame(const uint8* pubData,
                                    uint32 cbHdr, const uint8* pHdr,
                                    const uint8* pNewBody, uint32 cbNewBody,
                                    uint32* pNewSize)
    {
        *pNewSize = sizeof(MsgHdr) + cbHdr + cbNewBody;
        if (*pNewSize > sizeof(g_TxPool[0])) return nullptr;

        std::lock_guard<std::mutex> lock(g_TxLock);
        uint8* buf = g_TxPool[g_TxPoolIdx];
        const MsgHdr* orig = reinterpret_cast<const MsgHdr*>(pubData);
        MsgHdr* out = reinterpret_cast<MsgHdr*>(buf);
        out->eMsg         = orig->eMsg;
        out->headerLength = cbHdr;
        memcpy(buf + sizeof(MsgHdr), pHdr, cbHdr);
        memcpy(buf + sizeof(MsgHdr) + cbHdr, pNewBody, cbNewBody);
        g_TxPoolIdx = (g_TxPoolIdx + 1) % kPacketPoolSize;
        return buf;
    }

    // ── Hash constants for target_job_name dispatch ─────────────
    constexpr uint32 HASH_JOB_NotifyRunningApps      = LcFnvHash("FamilyGroupsClient.NotifyRunningApps#1");
    constexpr uint32 HASH_JOB_GetUserStats            = LcFnvHash("Player.GetUserStats#1");
    constexpr uint32 HASH_JOB_GetManifestRequestCode  = LcFnvHash("ContentServerDirectory.GetManifestRequestCode#1");
} // anonymous namespace


// ▌▌ LumaCore ▌ WIRE ▌ AccessToken
//  Outgoing: CMsgClientPICSProductInfoRequest (eMsg 8903)
// ▌▌
namespace AccessToken {

    bool HandleSend(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientPICSProductInfoRequest req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_PICS_WARN("Failed to ParseFromArray CMsgClientPICSProductInfoRequest");
            return false;
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest original body:\n{}", req.DebugString());

        bool needsPatch = false;
        for (const auto& app : req.apps()) {
            if (LuaLoader::HasDepot(app.appid()) && LuaLoader::GetAccessToken(app.appid())) {
                needsPatch = true;
                LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: found appid {} with access_token, need patching", app.appid());
                break;
            }
        }
        if (!needsPatch) {
            LOG_PICS_TRACE("CMsgClientPICSProductInfoRequest: no apps need token injection, skip");
            return false;
        }

        int injected = 0, noToken = 0, notAddAppId = 0;
        for (auto& app : *req.mutable_apps()) {
            if (LuaLoader::HasDepot(app.appid())) {
                uint64_t token = LuaLoader::GetAccessToken(app.appid());
                if (token) {
                    LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: inject appid={}: {} -> {}", app.appid(),
                               app.has_access_token() ? std::to_string(app.access_token()) : "absent",
                               token);
                    app.set_access_token(token);
                    ++injected;
                } else {
                    LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: skip appid={}: in depot, no token configured", app.appid());
                    ++noToken;
                }
            } else {
                ++notAddAppId;
            }
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: injected={} no_token={} not_in_add_appid={} total={}",
                   injected, noToken, notAddAppId, req.apps_size());

        g_TxBodyLen = static_cast<uint32>(req.ByteSizeLong());
        if (g_TxBodyLen > kMaxBodySize) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: encoded size {} exceeds buffer", g_TxBodyLen);
            return false;
        }
        if (!req.SerializeToArray(g_TxBody, kMaxBodySize)) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: Failed to encode modified request");
            return false;
        }

        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: modified body: {}", req.DebugString());
        return true;
    }

} // namespace AccessToken


// ▌▌ LumaCore ▌ WIRE ▌ UserStats
//  Outgoing: CPlayer_GetUserStats_Request  (eMsg 151 -> target: Player.GetUserStats#1)
//            CMsgClientGetUserStats        (eMsg 818)
//  Incoming: CPlayer_GetUserStats_Response (eMsg 147 <- target: Player.GetUserStats#1)
//            CMsgClientGetUserStatsResponse(eMsg 819)
// ▌▌
namespace UserStats {

    // jobid_source -> {appid, insert_time} mapping (eMsg 151 request -> eMsg 147 response)
    // Entries older than 30 s are pruned on each insert to prevent unbounded growth.
    using JobEntry = std::pair<AppId_t, std::chrono::steady_clock::time_point>;
    std::unordered_map<uint64, JobEntry> g_JobIdToAppId;

    // 6.2.4 hotfix: per-appid "we just spoofed an 818 for this game"
    // tracker. EMsg 819 has no jobid correlation, so we record the appid
    // when 818 send actually spoofs and only strip the matching 819
    // response. Without this gate the strip ran on pass-through requests
    // too (where Steam already had a cached schema) and told Steam every
    // configured game had 0 unlocks. TTL-prune keeps the map bounded
    // even if a request never gets a matching response.
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_PendingClientStatsSpoof;
    std::mutex g_PendingClientStatsSpoofMutex;

    // ── Send: CPlayer_GetUserStats_Request (eMsg 151) ──────────
    bool HandleSend_GetUserStats(const uint8* pBody, uint32 cbBody,
                                 const uint8* pHdr, uint32 cbHdr)
    {

        CPlayer_GetUserStats_Request req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to ParseFromArray");
            return false;
        }
        if (!req.has_appid()) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: missing appid");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: original body:\n{}", req.DebugString());
        
        AppId_t appId = req.appid();
        // -onlinefix masquerade: the wire reports app 480 (Spacewar)
        // because SpawnProcess rewrote pGameID. Redirect to the real
        // appid so the depot check, the spoof rewrite, and the body
        // we serialise all see the real game.
        AppId_t realAppId = SteamCapture::ResolveAppId();
        if (appId == kOnlineFixAppId
            && realAppId != 0
            && realAppId != kOnlineFixAppId) {
            LOG_ACHIEVEMENT_INFO(
                "Player::GetUserStats request: -onlinefix redirect appid {} -> {}",
                appId, realAppId);
            appId = realAppId;
            req.set_appid(realAppId);
        }
        if (!LuaLoader::HasDepot(appId)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: appid={} is not in addappid", appId);
            return false;
        }

        // Widen the spoof gate for fake-owned appids: clear sha_schema so the
        // server returns a populated schema instead of eresult=2. The
        // schema-wipe-on-disk concern is gone (the wipe was removed in a prior
        // fix), so re-clearing sha_schema is safe again.
        req.clear_sha_schema();

        // Save jobid_source -> appid for the response handler
        CMsgProtoBufHeader hdr;
        if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_jobid_source()) {
            uint64 jobId = hdr.jobid_source();
            auto now = std::chrono::steady_clock::now();
            std::erase_if(g_JobIdToAppId, [&now](const auto& e) {
                return now - e.second.second > std::chrono::seconds(30);
            });
            g_JobIdToAppId[jobId] = {appId, now};
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: stored jobid={} -> appid={}", jobId, appId);
        }

        // Single stable stat steamid per appid, no pool cycling. Cycling
        // gave Steam a different dummy account on every retry and confused
        // the local cache.
        uint64_t newSteamId = LuaLoader::GetStatSteamId(appId);
        req.set_steamid(newSteamId);
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: spoof steamid={} for appid={}", newSteamId, appId);

        g_TxBodyLen = static_cast<uint32>(req.ByteSizeLong());
        if (g_TxBodyLen > kMaxBodySize) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: encoded size {} exceeds buffer", g_TxBodyLen);
            return false;
        }
        if (!req.SerializeToArray(g_TxBody, kMaxBodySize)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to encode");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CPlayer_GetUserStats_Response (eMsg 147) ─────────
    //     Header: set eresult=OK.  Body: strip stats (field 4).
    //
    // The request handler swapped steamid with a pool dummy (in
    // HandleSend_GetUserStats), so any returned stats belong to the dummy
    // account.  Always strip them.  Genuinely-owned games never reach this
    // handler because HasDepot() returns false for them after CheckAppOwnership
    // marks them owned.
    void HandleRecv_GetUserStatsResponse(const uint8* pHdr, uint32 cbHdr,
                                    const uint8* pBody, uint32 cbBody)
    {
        // Header: set eresult=OK
        CMsgProtoBufHeader hdrMsg;
        if (!hdrMsg.ParseFromArray(pHdr, cbHdr)){
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to ParseFromArray original header");
            return;
        }
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original header:\n{}", hdrMsg.DebugString());

        // Look up appid via jobid_target -> jobid_source match
        AppId_t appId = 0;
        bool hasAppId = false;
        if (hdrMsg.has_jobid_target()) {
            uint64 jobId = hdrMsg.jobid_target();
            auto it = g_JobIdToAppId.find(jobId);
            if (it != g_JobIdToAppId.end()) {
                appId = it->second.first;
                hasAppId = true;
                LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: matched jobid={} -> appid={}", jobId, appId);
                g_JobIdToAppId.erase(it);
            }
        }

        // Parse body up front so we can decide whether to short-circuit.
        CPlayer_GetUserStats_Response resp;
        if (!resp.ParseFromArray(pBody, cbBody)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to ParseFromArray original response");
            return;
        }
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original body:\n{}", resp.DebugString());

        if (!hasAppId || !LuaLoader::HasDepot(appId)) {
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: no appid match, skip patches");
            return;
        }

        // Force header eresult=OK so the UI accepts the (now empty) reply.
        hdrMsg.set_eresult(static_cast<int32_t>(k_EResultOK));
        g_RxHdrLen = static_cast<uint32>(hdrMsg.ByteSizeLong());
        if (g_RxHdrLen > kMaxHdrSize || !hdrMsg.SerializeToArray(g_RxHdr, kMaxHdrSize))
            return;
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: modified header:\n{}", hdrMsg.DebugString());

        // Body: strip stats so the UI doesn't render dummy-account unlocks.
        resp.clear_stats();
        // 6.2.4 hotfix: bail to pass-through when the modified body
        // would overflow kMaxBodySize. Big schemas (LEGO Batman with
        // 50+ achievements * 16+ localized strings, etc) easily exceed
        // 8 KB. Silent truncation produced a malformed protobuf that
        // Steam wrote into its schema cache as zero achievements,
        // blanking the panel. Also flip g_PatchRxHdr only after the
        // body re-serialize actually succeeds, so a body failure can
        // never leave a patched header dangling.
        size_t newLen147 = resp.ByteSizeLong();
        if (newLen147 > kMaxBodySize) {
            LOG_ACHIEVEMENT_WARN(
                "Player::GetUserStats response: appid={} modified size {} > {} buffer; pass-through to keep schema cache intact",
                appId, newLen147, kMaxBodySize);
            return;
        }
        g_RxBodyLen = static_cast<uint32>(newLen147);
        if (!resp.SerializeToArray(g_RxBody, kMaxBodySize)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to SerializeToArray modified response");
            return;
        }
        g_PatchRxHdr = true;
        g_PatchRx = true;

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: modified body:\n{}", resp.DebugString());
    }

    // ── Send: CMsgClientGetUserStats (eMsg 818) ────────────────
    bool HandleSend_ClientGetUserStats(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientGetUserStats req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to ParseFromArray");
            return false;
        }
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: original body:\n{}", req.DebugString());

        if (!req.has_game_id()) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: missing game_id");
            return false;
        }
        AppId_t appId = static_cast<AppId_t>(req.game_id());
        // -onlinefix masquerade: the wire reports app 480 (Spacewar)
        // because SpawnProcess rewrote pGameID. Redirect to the real
        // appid so the depot check, the spoof rewrite, and the body
        // we serialise all see the real game.
        AppId_t realAppId = SteamCapture::ResolveAppId();
        if (appId == kOnlineFixAppId
            && realAppId != 0
            && realAppId != kOnlineFixAppId) {
            LOG_ACHIEVEMENT_INFO(
                "ClientGetUserStats request: -onlinefix redirect game_id {} -> {}",
                appId, realAppId);
            appId = realAppId;
            req.set_game_id(realAppId);
        }
        if (!LuaLoader::HasDepot(appId)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: appid={} is not in addappid", appId);
            return false;
        }
        // appid in addappid means fresh-fetch rewrite, no cache-token gate.
        // The on-disk wipe is gone, so we cannot trust the local crc to
        // match what we will spoof. Wipe crc_stats and force
        // schema_local_version=-1 every time so the server cannot
        // short-circuit with eresult=2 on a stale validation token.
        req.clear_crc_stats();
        req.set_schema_local_version(-1);

        // Single stable stat steamid per appid, no pool cycling.
        uint64_t newSteamId = LuaLoader::GetStatSteamId(appId);
        req.set_steam_id_for_user(newSteamId);
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: spoof steam_id_for_user={} for appid={}", newSteamId, appId);

        // Mark this appid as "just spoofed" so the matching 819 response
        // strips. The 819 handler keys off this set; pass-through
        // requests (where Steam already had a cached schema) get
        // pass-through responses, keeping the local cache intact.
        {
            std::lock_guard<std::mutex> guard(g_PendingClientStatsSpoofMutex);
            auto now = std::chrono::steady_clock::now();
            std::erase_if(g_PendingClientStatsSpoof, [&now](const auto& e) {
                return now - e.second > std::chrono::seconds(30);
            });
            g_PendingClientStatsSpoof[appId] = now;
        }

        g_TxBodyLen = static_cast<uint32>(req.ByteSizeLong());
        if (g_TxBodyLen > kMaxBodySize) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: encoded size {} exceeds buffer", g_TxBodyLen);
            return false;
        }
        if (!req.SerializeToArray(g_TxBody, kMaxBodySize)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to SerializeToArray");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CMsgClientGetUserStatsResponse (eMsg 819) ────────
    //     Strip stats(5) + achievement_blocks(6), patch eresult->OK
    //     ONLY when this response belongs to a request we just spoofed.
    //
    // EMsg 819 has no jobid correlation field, so we key off the
    // game_id and the per-appid pending flag set by the send handler.
    // Spoofed first-fetch responses get stripped (their stats belong
    // to the dummy account). Pass-through responses (Steam had a local
    // cache, schema_local_version != -1) get left alone so Steam keeps
    // its own cache instead of being told the user has 0 unlocks.
    bool HandleRecv_ClientGetUserStatsResponse(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientGetUserStatsResponse resp;
        if (!resp.ParseFromArray(pBody, cbBody))
            return false;
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: original body:\n{}", resp.DebugString());
        if (!resp.has_game_id()) {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: no modification needed");
            return false;
        }
        // -onlinefix masquerade: the wire reports app 480 (Spacewar).
        // Resolve the real appid before the depot gate AND before the
        // pending-spoof lookup so the matching 818 entry under the
        // real appid actually gets found.
        AppId_t gameId = static_cast<AppId_t>(resp.game_id());
        AppId_t realAppId = SteamCapture::ResolveAppId();
        if (gameId == kOnlineFixAppId
            && realAppId != 0
            && realAppId != kOnlineFixAppId) {
            LOG_ACHIEVEMENT_INFO(
                "ClientGetUserStats response: -onlinefix redirect game_id {} -> {}",
                gameId, realAppId);
            gameId = realAppId;
        }
        if (!LuaLoader::HasDepot(gameId)) {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: no modification needed");
            return false;
        }


        // Was the matching 818 spoofed? If not, this is pass-through.
        bool wasSpoofed = false;
        {
            std::lock_guard<std::mutex> guard(g_PendingClientStatsSpoofMutex);
            auto it = g_PendingClientStatsSpoof.find(gameId);
            if (it != g_PendingClientStatsSpoof.end()) {
                wasSpoofed = true;
                g_PendingClientStatsSpoof.erase(it);
            }
        }
        if (!wasSpoofed) {
            LOG_ACHIEVEMENT_DEBUG(
                "ClientGetUserStats response: appid={} pass-through (request was not spoofed), keep stats",
                gameId);
            return false;
        }

        resp.clear_stats();
        resp.clear_achievement_blocks();
        // 6.2.4 hotfix: also clear crc_stats. Steam writes whatever
        // crc the server returns into <steam>/appcache/stats/
        // UserGameStats_<accid>_<appid>.bin. With a non-zero crc and
        // zero stats inside, Steam treats the cache as "valid empty"
        // on next launch — sends 818 with that crc, server returns
        // eresult=2 (no update), pass-through, Steam shows the empty
        // cache. The achievement panel goes blank on every restart.
        // Clearing crc here makes Steam re-fetch on every launch so
        // the spoofed schema (with all achievement names / icons)
        // always comes back fresh.
        resp.clear_crc_stats();
        resp.set_eresult(1);  // k_EResultOK
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: clear stats/achievement_blocks/crc_stats, set eresult=OK");

        // 6.2.4 hotfix: bail to pass-through when the modified body
        // overflows kMaxBodySize. LEGO Batman / Schedule I / similar
        // big-schema games hit this; silent truncation corrupted
        // Steam's local schema cache and the overlay panel rendered
        // 0 / 52 unlocks until the cache file was deleted by hand.
        size_t newLen819 = resp.ByteSizeLong();
        if (newLen819 > kMaxBodySize) {
            LOG_ACHIEVEMENT_WARN(
                "ClientGetUserStats response: appid={} modified size {} > {} buffer; pass-through to keep schema cache intact",
                gameId, newLen819, kMaxBodySize);
            return false;
        }
        g_RxBodyLen = static_cast<uint32>(newLen819);
        if (!resp.SerializeToArray(g_RxBody, kMaxBodySize))
            return false;
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: modified body:\n{}", resp.DebugString());
        return true;
    }

} // namespace UserStats


// ▌▌ LumaCore ▌ WIRE ▌ ETicket
//  Incoming: CMsgClientRequestEncryptedAppTicketResponse (eMsg 5527)
// ▌▌
namespace ETicket {

    void HandleEncryptedAppTicketResponse(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientRequestEncryptedAppTicketResponse resp;
        if (!resp.ParseFromArray(pBody, cbBody)) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: failed to ParseFromArray");
            return;
        }
        LOG_NETPACKET_DEBUG("ClientRequestEncryptedAppTicketResponse: original body:\n{}", resp.DebugString());

        if (resp.eresult() == k_EResultOK) return;
        if (!LuaLoader::HasDepot(resp.app_id())) return;

        auto ticket = Ticket::GetEncryptedTicketFromRegistry(resp.app_id());
        if (ticket.empty()) return;

        if (!resp.mutable_encrypted_app_ticket()->ParseFromArray(
                ticket.data(), static_cast<int>(ticket.size()))) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: failed to ParseFromArray EncryptedAppTicket");
            return;
        }

        resp.set_eresult(k_EResultOK);

        auto encSize = resp.ByteSizeLong();
        if (encSize > sizeof(g_RxBody)) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: modified message too large");
            return;
        }
        if (!resp.SerializeToArray(g_RxBody, sizeof(g_RxBody))) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: failed to SerializeToArray modified response");
            return;
        }
        
        LOG_NETPACKET_DEBUG("ClientRequestEncryptedAppTicketResponse: modified body:\n{}", resp.DebugString());

        g_RxBodyLen = static_cast<uint32>(encSize);
        g_PatchRx = true;
    }

} // namespace ETicket


// ▌▌ LumaCore ▌ WIRE ▌ AppOwnershipTicketResponse (debug-only inspector for now)
//  Incoming: CMsgClientGetAppOwnershipTicketResponse  (eMsg 858)
//  Outgoing: CMsgClientGetAppOwnershipTicket          (eMsg 857)
//
//  Steam asks the server for an AppOwnershipTicket via eMsg 857. When
//  the user does not legitimately own the app, the server returns a
//  short body (typically 6 bytes) carrying eresult != OK and no ticket
//  payload. Steam then refuses to launch DRM-wrapped games (error 54).
//
//  This handler exists right now only to dump the raw response so we
//  can see exactly what the server is sending. No patching yet; once
//  the wire format is confirmed we will build a forged success reply
//  here using the cached registry blob.
// ▌▌
namespace AppOwnershipTicketResp {

    void HandleSend(const uint8* pBody, uint32 cbBody)
    {
        // Dump the request body (typically very small protobuf with
        // only the appid field) so we know which app Steam asked for.
        std::string hex;
        const uint32 dumpN = cbBody > 64 ? 64 : cbBody;
        hex.reserve(dumpN * 3);
        char buf[4];
        for (uint32 i = 0; i < dumpN; ++i) {
            std::snprintf(buf, sizeof(buf), "%02X ", pBody[i]);
            hex.append(buf);
        }
        LOG_NETPACKET_INFO("k_EMsgClientGetAppOwnershipTicket(857) SEND cbBody={} body[hex]={}",
                           cbBody, hex);
    }

    void HandleRecv(const uint8* pHdr, uint32 cbHdr,
                    const uint8* pBody, uint32 cbBody)
    {
        // Hex dump up to 256 bytes of body (more than enough for a small reply).
        std::string hex;
        const uint32 dumpN = cbBody > 256 ? 256 : cbBody;
        hex.reserve(dumpN * 3);
        char buf[4];
        for (uint32 i = 0; i < dumpN; ++i) {
            std::snprintf(buf, sizeof(buf), "%02X ", pBody[i]);
            hex.append(buf);
        }
        LOG_NETPACKET_INFO("k_EMsgClientGetAppOwnershipTicketResponse: cbBody={} cbHdr={} body[hex]={}",
                           cbBody, cbHdr, hex);

        // Try to parse the protobuf header so we can print the eresult.
        CMsgProtoBufHeader hdr;
        if (hdr.ParseFromArray(pHdr, cbHdr)) {
            LOG_NETPACKET_INFO("k_EMsgClientGetAppOwnershipTicketResponse: header eresult={} jobid_target={} jobid_source={}",
                               hdr.eresult(),
                               hdr.has_jobid_target() ? hdr.jobid_target() : 0,
                               hdr.has_jobid_source() ? hdr.jobid_source() : 0);
        } else {
            LOG_NETPACKET_WARN("k_EMsgClientGetAppOwnershipTicketResponse: failed to parse CMsgProtoBufHeader");
        }
    }

} // namespace AppOwnershipTicketResp


// ▌▌ LumaCore ▌ WIRE ▌ FamilySharing
// ▌▌
namespace FamilySharing {

    void ClearBody(const uint8*, uint32)
    {
        LOG_NETPACKET_DEBUG("Clearing family sharing message...");
        g_RxBodyLen = 0;
        g_PatchRx = true;
    }

} // namespace FamilySharing




// ▌▌ LumaCore ▌ WIRE ▌ OnlineFix
//  Outgoing: CMsgClientGamesPlayed (eMsg 742 / 5410)
//
//  When a game launched with -onlinefix reports appid 480, replace
//  game_extra_info with the real game's localized name so friends
//  see the correct title.
// ▌▌
namespace OnlineFix {

    bool HandleSend(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientGamesPlayed msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to parse CMsgClientGamesPlayed");
            return false;
        }
        LOG_ONLINEFIX_DEBUG("OnlineFix: original body:\n{}", msg.DebugString());

        AppId_t storedReal = SteamCapture::ResolveAppId();
        bool sawAny480 = false;
        bool patched = false;
        for (int i = 0; i < msg.games_played_size(); ++i) {
            auto* game = msg.mutable_games_played(i);
            AppId_t appid = static_cast<AppId_t>(game->game_id() & UINT32_MAX);

            // SpawnProcess rewrites pGameID to 480, so game_id is already 480.
            // Fill game_extra_info with the real game name.
            if (appid == kOnlineFixAppId) {
                sawAny480 = true;
                AppId_t realAppId = SteamCapture::ResolveAppId();
                if (!realAppId) {
                    LOG_ONLINEFIX_WARN("OnlineFix: saw 480 but realAppId=0 (SpawnProcess never set it)");
                    continue;
                }
                if (!LuaLoader::HasDepot(realAppId)) {
                    LOG_ONLINEFIX_WARN("OnlineFix: realAppId={} has no depot, skip", realAppId);
                    continue;
                }
                std::string name = SteamCapture::GetGameNameByAppID(realAppId);
                if (name.empty()) {
                    LOG_ONLINEFIX_WARN("OnlineFix: realAppId={} game name lookup empty, skip", realAppId);
                    continue;
                }
                game->set_game_extra_info(name);
                patched = true;
                LOG_ONLINEFIX_INFO("OnlineFix: 480 -> name '{}' (real appid {})",
                                   name, realAppId);
            } else if (storedReal && appid == storedReal) {
                // Real appid leaked through — SpawnProcess rewrite missed.
                LOG_ONLINEFIX_WARN("OnlineFix: games_played carries real appid {} "
                                   "(expected 480 — SpawnProcess rewrite did not run)",
                                   appid);
            }
        }

        if (!patched) {
            if (sawAny480) {
                LOG_ONLINEFIX_DEBUG("OnlineFix: saw 480 entry but nothing patched");
            }
            return false;
        }

        g_TxBodyLen = static_cast<uint32>(msg.ByteSizeLong());
        if (g_TxBodyLen > kMaxBodySize) {
            LOG_ONLINEFIX_WARN("OnlineFix: encoded size {} exceeds buffer", g_TxBodyLen);
            return false;
        }
        if (!msg.SerializeToArray(g_TxBody, kMaxBodySize)) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to SerializeToArray");
            return false;
        }

        LOG_ONLINEFIX_DEBUG("OnlineFix: modified body:\n{}", msg.DebugString());
        return true;
    }

} // namespace OnlineFix


// ▌▌ LumaCore ▌ WIRE ▌ DepotFallback
//  Outgoing: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 151)
//  Incoming: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 147)
//
//  When Steam asks the content directory for a request code on a depot
//  we faked ownership of, the server hands back eresult=2 / cbBody=0
//  and the download UI screams "NO INTERNET CONNECTION". The send
//  handler kicks off an async HTTP fetch; the recv handler waits up
//  to manifest_fetch.timeout_sec for the future to land, then rewrites
//  the header eresult to OK and stuffs the request code into the body.
//  On timeout/failure the original frame just passes through, so a
//  legitimately-owned depot or a busted mirror never makes things
//  worse than they already were.
// ▌▌
namespace DepotFallback {

    bool HandleSend(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        CContentServerDirectory_GetManifestRequestCode_Request req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_MANIFESTCH_WARN("GetManifestRequestCode send: parse failed (cbBody={})", cbBody);
            return false;
        }
        if (!req.has_depot_id() || !req.has_manifest_id()) {
            LOG_MANIFESTCH_DEBUG("GetManifestRequestCode send: depot/manifest missing, skip");
            return false;
        }
        const AppId_t depotId = req.depot_id();
        const uint64_t gid    = req.manifest_id();
        const AppId_t appId   = req.has_app_id() ? req.app_id() : 0;

        // Only intercept depots LumaCore is actively faking. Real-owned
        // depots use the same target_job_name and we want their requests
        // to fly straight through.
        if (!LuaLoader::HasDepot(depotId)) {
            LOG_MANIFESTCH_DEBUG("GetManifestRequestCode send: depot={} gid={} not in addappid, skip",
                                 depotId, gid);
            return false;
        }

        CMsgProtoBufHeader hdr;
        if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_jobid_source()) {
            LOG_MANIFESTCH_WARN("GetManifestRequestCode send: missing jobid_source, skip");
            return false;
        }
        const uint64 jobId = hdr.jobid_source();

        LOG_MANIFESTCH_INFO("GetManifestRequestCode send: depot={} gid={} app={} jobid={}",
                            depotId, gid, appId, jobId);
        ManifestFetch::Submit(jobId, gid, appId, depotId);
        // Don't rewrite the outgoing frame; the body Steam built is fine
        // as a placeholder, and we always intercept the response.
        return false;
    }

    void HandleRecv(const uint8* pHdr, uint32 cbHdr,
                    const uint8* pBody, uint32 cbBody)
    {
        CMsgProtoBufHeader hdr;
        if (!hdr.ParseFromArray(pHdr, cbHdr)) {
            LOG_MANIFESTCH_WARN("GetManifestRequestCode recv: header parse failed");
            return;
        }
        if (!hdr.has_jobid_target()) {
            LOG_MANIFESTCH_DEBUG("GetManifestRequestCode recv: no jobid_target");
            return;
        }
        const uint64 jobId = hdr.jobid_target();

        auto resolved = ManifestFetch::Resolve(jobId);
        if (!resolved) {
            // Either no Submit ever ran for this jobid (depot wasn't ours)
            // or the HTTP fetch failed/timed out. Let the original frame
            // pass through — Steam falls back to its own retry path.
            LOG_MANIFESTCH_DEBUG("GetManifestRequestCode recv: jobid={} no patch (cbBody={} hdr.eresult={})",
                                 jobId, cbBody, hdr.eresult());
            return;
        }

        // Rewrite the header so the eresult flips to OK.
        hdr.set_eresult(static_cast<int32_t>(k_EResultOK));
        const size_t hdrSize = hdr.ByteSizeLong();
        if (hdrSize > kMaxHdrSize || !hdr.SerializeToArray(g_RxHdr, kMaxHdrSize)) {
            LOG_MANIFESTCH_WARN("GetManifestRequestCode recv: header re-serialise failed (size={})", hdrSize);
            return;
        }
        g_RxHdrLen = static_cast<uint32>(hdrSize);

        // Build the body carrying the request code.
        CContentServerDirectory_GetManifestRequestCode_Response resp;
        resp.set_manifest_request_code(*resolved);
        const size_t bodySize = resp.ByteSizeLong();
        if (bodySize > kMaxBodySize || !resp.SerializeToArray(g_RxBody, kMaxBodySize)) {
            LOG_MANIFESTCH_WARN("GetManifestRequestCode recv: body re-serialise failed (size={})", bodySize);
            return;
        }
        g_RxBodyLen = static_cast<uint32>(bodySize);

        g_PatchRxHdr = true;
        g_PatchRx    = true;
        LOG_MANIFESTCH_INFO("GetManifestRequestCode recv: jobid={} injected code={} (orig cbBody={})",
                            jobId, *resolved, cbBody);
    }

} // namespace DepotFallback


// ▌▌ LumaCore ▌ WIRE ▌ Dispatch
// ▌▌
namespace {

    bool SendServiceJob(const char* targetJobName,
                        const uint8* pBody, uint32 cbBody,
                        const uint8* pHdr, uint32 cbHdr)
    {
        LOG_NETPACKET_DEBUG("Send target_job_name: {}", targetJobName);
        switch (LcFnvHash(targetJobName)) {

        case HASH_JOB_GetUserStats:
            return UserStats::HandleSend_GetUserStats(pBody, cbBody, pHdr, cbHdr);

        case HASH_JOB_GetManifestRequestCode:
            return DepotFallback::HandleSend(pBody, cbBody, pHdr, cbHdr);

        // ---- add new 151 service methods here ----
        }
        return false;
    }

    void SendJob(EMsg eMsg, const uint8* pBody, uint32 cbBody,
                 const uint8* pHdr, uint32 cbHdr)
    {
        g_PatchTx = false;

        LOG_NETPACKET_DEBUG("Send eMsg {}({}) (cbBody={}, cbHdr={})",
                        EmsgName(eMsg), static_cast<uint32>(eMsg), cbBody, cbHdr);

        switch (eMsg) {

        case k_EMsgServiceMethodCallFromClient: {   // 151
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_target_job_name()) {
                g_PatchTx = SendServiceJob(hdr.target_job_name().c_str(), pBody, cbBody, pHdr, cbHdr);
            }
            return;
        }

        case k_EMsgClientPICSProductInfoRequest:     // 8903
            g_PatchTx = AccessToken::HandleSend(pBody, cbBody);
            return;

        case k_EMsgClientGamesPlayed:                 // 742
        case k_EMsgClientGamesPlayedWithDataBlob:     // 5410
            g_PatchTx = OnlineFix::HandleSend(pBody, cbBody);
            return;

        case k_EMsgClientGetUserStats:               // 818
            g_PatchTx = UserStats::HandleSend_ClientGetUserStats(pBody, cbBody);
            return;

        case k_EMsgClientGetAppOwnershipTicket:       // 857
            // Inspector only — log which app Steam asks for tickets on.
            AppOwnershipTicketResp::HandleSend(pBody, cbBody);
            return;

        default:
            return;
        }
    }

    void RecvServiceJob(const char* targetJobName,
                        const uint8* pBody, uint32 cbBody,
                        const uint8* pHdr, uint32 cbHdr)
    {
        LOG_NETPACKET_DEBUG("Recv target_job_name: {}", targetJobName);
        g_PatchRx = false;
        g_PatchRxHdr  = false;

        switch (LcFnvHash(targetJobName)) {

        case HASH_JOB_NotifyRunningApps:
            FamilySharing::ClearBody(pBody, cbBody);
            return;

        case HASH_JOB_GetUserStats:
            UserStats::HandleRecv_GetUserStatsResponse(pHdr, cbHdr, pBody, cbBody);
            return;

        case HASH_JOB_GetManifestRequestCode:
            DepotFallback::HandleRecv(pHdr, cbHdr, pBody, cbBody);
            return;

        // ---- add new 147 service methods here ----
        }
    }

    void RecvJob(EMsg eMsg, const uint8* pBody, uint32 cbBody,
                 const uint8* pHdr, uint32 cbHdr)
    {
        g_PatchRx = false;
        g_PatchRxHdr  = false;

        if(eMsg == k_EMsgMulti) {
            LOG_NETPACKET_TRACE("Received k_EMsgMulti, skipping dispatch");
            return;
        }
        LOG_NETPACKET_DEBUG("Recv eMsg {}({}) (cbBody={}, cbHdr={})",
                        EmsgName(eMsg), static_cast<uint32>(eMsg), cbBody, cbHdr);

        switch (eMsg) {

        case k_EMsgServiceMethodResponse: {     // 147
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_target_job_name())
                RecvServiceJob(hdr.target_job_name().c_str(), pBody, cbBody, pHdr, cbHdr);
            return;
        }

        // migrated to IPC layer CmdUser::GetEncryptedAppTicketResponse
        // case k_EMsgClientRequestEncryptedAppTicketResponse:     // 5527
        //     ETicket::HandleEncryptedAppTicketResponse(pBody, cbBody);
        //     return;

        case k_EMsgClientGetUserStatsResponse:     // 819
            g_PatchRx = UserStats::HandleRecv_ClientGetUserStatsResponse(
                pBody, cbBody);
            return;

        case k_EMsgClientGetAppOwnershipTicketResponse:     // 858
            // Inspector only — logs server reply so we can see why the
            // ticket fetch is failing for Steam-DRM games like Teardown.
            AppOwnershipTicketResp::HandleRecv(pHdr, cbHdr, pBody, cbBody);
            return;

        case k_EMsgClientPersonaState:     // 766
        {
            uint32 rpSize = 0;
            if (RichPresence::HandleRecv(pBody, cbBody, g_RxBody, kMaxBodySize, &rpSize)) {
                g_RxBodyLen = rpSize;
                g_PatchRx = true;
            }
            return;
        }

        case k_EMsgClientSharedLibraryLockStatus:      // 9405
            // Steam sends this when a family-shared library entry transitions
            // between locked/unlocked. Clearing means "nothing is locked",
            // which keeps fake-owned apps playable when the actual owner is
            // online. Defensive — observed cases where 9406 alone wasn't
            // enough on the latest Steam client.
            FamilySharing::ClearBody(pBody, cbBody);
            return;

        case k_EMsgClientSharedLibraryStopPlaying:     // 9406
            FamilySharing::ClearBody(pBody, cbBody);
            return;

        default:
            return;
        }
    }

    // ▌ WIRE ▌ Hooks
    // ▌

    LC_HOOK_DEF(BBuildAndAsyncSendFrame, bool,
              void* pObject, EWebSocketOpCode eWebSocketOpCode,
              uint8* pubData, uint32 cubData)
    {
        if (eWebSocketOpCode != k_eWebSocketOpCode_Binary)
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);

        EMsg eMsg;
        const uint8 *pHdr, *pBody;
        uint32 cbHdr, cbBody;
        if (DecodeFrame(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) {
            SendJob(eMsg, pBody, cbBody, pHdr, cbHdr);

            if (g_PatchTx) {
                uint32 newSize = 0;
                uint8* buf = PatchSendFrame(pubData, cbHdr, pHdr,
                                               g_TxBody, g_TxBodyLen, &newSize);
                if (buf)
                    return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, buf, newSize);
            }
        }
        return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
    }

    LC_HOOK_DEF(RecvPkt, void*, void* pThis, CNetPacket* pPacket)
    {
        EMsg eMsg;
        const uint8 *pBody, *pHdr;
        uint32 cbBody, cbHdr;
        if (DecodeFrame(pPacket->m_pubData, pPacket->m_cubData,
                     eMsg, pHdr, cbHdr, pBody, cbBody)) {
            g_BodyShrunk = false;
            RecvJob(eMsg, pBody, cbBody, pHdr, cbHdr);

            if (g_BodyShrunk && g_PatchRxHdr) {
                // Body shrunk in-place + header changed -> full replace via pool
                PatchRecvFrame(pPacket,
                    g_RxHdr, g_RxHdrLen,
                    pBody, g_RxBodySize);
            } else if (g_BodyShrunk) {
                pPacket->m_cubData = sizeof(MsgHdr) + cbHdr + g_RxBodySize;
            } else if (g_PatchRxHdr || g_PatchRx) {
                PatchRecvFrame(pPacket,
                    g_PatchRxHdr  ? g_RxHdr  : pHdr,
                    g_PatchRxHdr  ? g_RxHdrLen : cbHdr,
                    g_PatchRx ? g_RxBody : pBody,
                    g_PatchRx ? g_RxBodyLen : cbBody);
            }
        }

        return oRecvPkt(pThis, pPacket);
    }

} // anonymous namespace


namespace PacketRouter {
    void Install() {
        LC_RESOLVE_D(PchMsgNameFromEMsg);
        LC_TX_OPEN();
        LC_ATTACH_D(BBuildAndAsyncSendFrame);
        LC_ATTACH_D(RecvPkt);
        LC_TX_COMMIT();
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(BBuildAndAsyncSendFrame);
        LC_DETACH(RecvPkt);
        LC_TX_COMMIT();
        oPchMsgNameFromEMsg = nullptr;
    }
}
