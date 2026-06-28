// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "hooks/client/NetPacket.h"
#include "hooks/capture/SteamCapture.h"
#include "config/LuaLoader.h"
#include "core/entry.h"
#include "runtime/Logger.h"

namespace NetPacket::Handlers::OnlineFix {

bool HandleSend(const uint8_t* pBody, uint32_t cbBody) {
    CMsgClientGamesPlayed msg;
    if (!msg.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"parse-fail\"}}}}");
        return false;
    }
    LOG_PKTRT_DEBUG("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"original\":{}}}", msg.DebugString());

    AppId_t storedReal = SteamCapture::ResolveAppId();
    bool sawAny480 = false;
    bool patched = false;
    for (int i = 0; i < msg.games_played_size(); ++i) {
        auto* game = msg.mutable_games_played(i);
        AppId_t appid = static_cast<AppId_t>(game->game_id() & UINT32_MAX);

        if (appid == kOnlineFixAppId) {
            sawAny480 = true;
            AppId_t realAppId = SteamCapture::ResolveAppId();
            if (!realAppId) {
                LOG_PKTRT_WARN("{{{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"no-realid\"}}}}");
                continue;
            }
            if (!LuaLoader::HasDepot(realAppId)) {
                LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"no-depot\",\"appId\":{}}}", realAppId);
                continue;
            }
            std::string name = SteamCapture::GetGameNameByAppID(realAppId);
            if (name.empty()) {
                LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"no-name\",\"appId\":{}}}", realAppId);
                continue;
            }
            game->set_game_extra_info(name);
            patched = true;
            LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"patch\",\"was\":480,\"name\":\"{}\",\"appId\":{}}}",
                       name, realAppId);
        } else if (storedReal && appid == storedReal) {
            LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"warn\":\"leaked\",\"appId\":{}}}", appid);
        }
    }

    if (!patched) {
        if (sawAny480) {
            LOG_PKTRT_DEBUG("{{{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"info\":\"saw-480-no-patch\"}}}}");
        }
        return false;
    }

    s_tx.BodyLen = static_cast<uint32_t>(msg.ByteSizeLong());
    if (s_tx.BodyLen > kBodyCap) {
        LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"overflow\",\"size\":{}}}", s_tx.BodyLen);
        return false;
    }
    if (!msg.SerializeToArray(s_tx.Body, kBodyCap)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"encode-fail\"}}}}");
        return false;
    }

    LOG_PKTRT_DEBUG("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"modified\":{}}}", msg.DebugString());
    return true;
}

} // namespace NetPacket::Handlers::OnlineFix
