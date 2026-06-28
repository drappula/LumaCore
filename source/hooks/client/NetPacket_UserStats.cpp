// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "hooks/client/NetPacket.h"
#include "hooks/capture/SteamCapture.h"
#include "config/LuaLoader.h"
#include "core/entry.h"
#include "runtime/Logger.h"

#include <unordered_map>
#include <mutex>
#include <chrono>

namespace NetPacket::Handlers::UserStats {

using JobEntry = std::pair<AppId_t, std::chrono::steady_clock::time_point>;
std::unordered_map<uint64_t, JobEntry> g_JobIdToAppId;

std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_PendingClientStatsSpoof;
std::mutex g_PendingClientStatsSpoofMutex;

bool HandleSend_GetUserStats(const uint8_t* pBody, uint32_t cbBody,
                             const uint8_t* pHdr, uint32_t cbHdr) {
    CPlayer_GetUserStats_Request req;
    if (!req.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"err\":\"parse-fail\"}}}}");
        return false;
    }
    if (!req.has_appid()) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"err\":\"no-appid\"}}}}");
        return false;
    }

    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"original\":{}}}", req.DebugString());

    AppId_t appId = req.appid();
    AppId_t realAppId = SteamCapture::ResolveAppId();
    if (appId == kOnlineFixAppId
        && realAppId != 0
        && realAppId != kOnlineFixAppId) {
        LOG_PKTRT_INFO("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"redirect\":\"onlinefix\",\"was\":{},\"now\":{}}}",
                   appId, realAppId);
        appId = realAppId;
        req.set_appid(realAppId);
    }
    if (!LuaLoader::HasDepot(appId)) {
        LOG_PKTRT_WARN("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"err\":\"no-depot\",\"appId\":{}}}", appId);
        return false;
    }

    req.clear_sha_schema();

    CMsgProtoBufHeader hdr;
    if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_jobid_source()) {
        uint64_t jobId = hdr.jobid_source();
        auto now = std::chrono::steady_clock::now();
        std::erase_if(g_JobIdToAppId, [&now](const auto& e) {
            return now - e.second.second > std::chrono::seconds(30);
        });
        g_JobIdToAppId[jobId] = {appId, now};
        LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"job\":{},\"appId\":{}}}", jobId, appId);
    }

    uint64_t newSteamId = LuaLoader::GetStatSteamId(appId);
    req.set_steamid(newSteamId);
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"spoof\":{},\"appId\":{}}}", newSteamId, appId);

    s_tx.BodyLen = static_cast<uint32_t>(req.ByteSizeLong());
    if (s_tx.BodyLen > kBodyCap) {
        LOG_PKTRT_WARN("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"err\":\"overflow\",\"size\":{}}}", s_tx.BodyLen);
        return false;
    }
    if (!req.SerializeToArray(s_tx.Body, kBodyCap)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"err\":\"encode-fail\"}}}}");
        return false;
    }

    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"GetUserStats\",\"modified\":{}}}", req.DebugString());
    return true;
}

void HandleRecv_GetUserStatsResponse(const uint8_t* pHdr, uint32_t cbHdr,
                                     const uint8_t* pBody, uint32_t cbBody) {
    CMsgProtoBufHeader hdrMsg;
    if (!hdrMsg.ParseFromArray(pHdr, cbHdr)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"err\":\"header-parse-fail\"}}}}");
        return;
    }
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"original-header\":{}}}", hdrMsg.DebugString());

    AppId_t appId = 0;
    bool hasAppId = false;
    if (hdrMsg.has_jobid_target()) {
        uint64_t jobId = hdrMsg.jobid_target();
        auto it = g_JobIdToAppId.find(jobId);
        if (it != g_JobIdToAppId.end()) {
            appId = it->second.first;
            hasAppId = true;
            LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"job-match\":{},\"appId\":{}}}", jobId, appId);
            g_JobIdToAppId.erase(it);
        }
    }

    CPlayer_GetUserStats_Response resp;
    if (!resp.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"err\":\"body-parse-fail\"}}}}");
        return;
    }
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"original-body\":{}}}", resp.DebugString());

    if (!hasAppId || !LuaLoader::HasDepot(appId)) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"skip\":\"no-match\"}}}}");
        return;
    }

    hdrMsg.set_eresult(static_cast<int32_t>(k_EResultOK));
    s_rx.HdrLen = static_cast<uint32_t>(hdrMsg.ByteSizeLong());
    if (s_rx.HdrLen > kHdrCap || !hdrMsg.SerializeToArray(s_rx.Hdr, kHdrCap))
        return;
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"modified-header\":{}}}", hdrMsg.DebugString());

    resp.clear_stats();
    size_t newLen147 = resp.ByteSizeLong();
    if (newLen147 > kBodyCap) {
        LOG_PKTRT_WARN(
            "{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"err\":\"overflow\",\"appId\":{},\"size\":{}}}",
            appId, newLen147);
        return;
    }
    s_rx.BodyLen = static_cast<uint32_t>(newLen147);
    if (!resp.SerializeToArray(s_rx.Body, kBodyCap)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"err\":\"encode-fail\"}}}}");
        return;
    }
    s_rx.PatchHdr = true;
    s_rx.PatchBody = true;

    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"GetUserStatsResp\",\"modified-body\":{}}}", resp.DebugString());
}

bool HandleSend_ClientGetUserStats(const uint8_t* pBody, uint32_t cbBody) {
    CMsgClientGetUserStats req;
    if (!req.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"err\":\"parse-fail\"}}}}");
        return false;
    }
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"original\":{}}}", req.DebugString());

    if (!req.has_game_id()) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"err\":\"no-game-id\"}}}}");
        return false;
    }
    AppId_t appId = static_cast<AppId_t>(req.game_id());
    AppId_t realAppId = SteamCapture::ResolveAppId();
    if (appId == kOnlineFixAppId
        && realAppId != 0
        && realAppId != kOnlineFixAppId) {
        LOG_PKTRT_INFO(
            "{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"redirect\":\"onlinefix\",\"was\":{},\"now\":{}}}",
            appId, realAppId);
        appId = realAppId;
        req.set_game_id(realAppId);
    }
    if (!LuaLoader::HasDepot(appId)) {
        LOG_PKTRT_WARN("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"err\":\"no-depot\",\"appId\":{}}}", appId);
        return false;
    }
    req.clear_crc_stats();
    req.set_schema_local_version(-1);

    uint64_t newSteamId = LuaLoader::GetStatSteamId(appId);
    req.set_steam_id_for_user(newSteamId);
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"spoof\":{},\"appId\":{}}}", newSteamId, appId);

    {
        std::lock_guard<std::mutex> guard(g_PendingClientStatsSpoofMutex);
        auto now = std::chrono::steady_clock::now();
        std::erase_if(g_PendingClientStatsSpoof, [&now](const auto& e) {
            return now - e.second > std::chrono::seconds(30);
        });
        g_PendingClientStatsSpoof[appId] = now;
    }

    s_tx.BodyLen = static_cast<uint32_t>(req.ByteSizeLong());
    if (s_tx.BodyLen > kBodyCap) {
        LOG_PKTRT_WARN("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"err\":\"overflow\",\"size\":{}}}", s_tx.BodyLen);
        return false;
    }
    if (!req.SerializeToArray(s_tx.Body, kBodyCap)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"err\":\"encode-fail\"}}}}");
        return false;
    }

    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"send\",\"sub\":\"ClientGetUserStats\",\"modified\":{}}}", req.DebugString());
    return true;
}

bool HandleRecv_ClientGetUserStatsResponse(const uint8_t* pBody, uint32_t cbBody) {
    CMsgClientGetUserStatsResponse resp;
    if (!resp.ParseFromArray(pBody, cbBody))
        return false;
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"original\":{}}}", resp.DebugString());
    if (!resp.has_game_id()) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"skip\":\"no-game-id\"}}}}");
        return false;
    }
    AppId_t gameId = static_cast<AppId_t>(resp.game_id());
    AppId_t realAppId = SteamCapture::ResolveAppId();
    if (gameId == kOnlineFixAppId
        && realAppId != 0
        && realAppId != kOnlineFixAppId) {
        LOG_PKTRT_INFO(
            "{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"redirect\":\"onlinefix\",\"was\":{},\"now\":{}}}",
            gameId, realAppId);
        gameId = realAppId;
    }
    if (!LuaLoader::HasDepot(gameId)) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"skip\":\"no-depot\"}}}}");
        return false;
    }

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
        LOG_PKTRT_DEBUG(
            "{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"skip\":\"not-spoofed\",\"appId\":{}}}",
            gameId);
        return false;
    }

    resp.clear_stats();
    resp.clear_achievement_blocks();
    resp.clear_crc_stats();
    resp.set_eresult(1);
    LOG_PKTRT_DEBUG("{{{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"stripped\":1}}}}");

    size_t newLen819 = resp.ByteSizeLong();
    if (newLen819 > kBodyCap) {
        LOG_PKTRT_WARN(
            "{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"err\":\"overflow\",\"appId\":{},\"size\":{}}}",
            gameId, newLen819);
        return false;
    }
    s_rx.BodyLen = static_cast<uint32_t>(newLen819);
    if (!resp.SerializeToArray(s_rx.Body, kBodyCap))
        return false;
    LOG_PKTRT_DEBUG("{{\"evt\":\"UserStats\",\"act\":\"recv\",\"sub\":\"ClientGetUserStatsResp\",\"modified\":{}}}", resp.DebugString());
    return true;
}

} // namespace NetPacket::Handlers::UserStats
