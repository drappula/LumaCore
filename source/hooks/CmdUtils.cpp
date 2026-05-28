// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "IPCBus.h"
#include "CmdUtils.h"
#include "CmdUser.h"
#include "SteamCapture.h"
#include "RuntimeCapture.h"
#include "Steam/Callback.h"
#include "Steam/Structs.h"
#include "entry.h"
#include "utils/Logger.h"

namespace {

    // ── IClientUtils::GetAPICallResult request args ──────────────
    struct GetAPICallResultRequest {
        uint64  hSteamAPICall;     // +0
        uint32  cubCallback;       // +8
        uint32  iCallbackExpected; // +12
    };

    // File-local twin of the PackagePatch.cpp helper. Two copies live
    // file-local in the call sites (per project §13 small-surface rule)
    // rather than sharing a header. Both translation units gate the rewrite
    // on the same quad: OnlineFix session, pipe-scoped fine gate, payload
    // size, and low-24 m_nGameID equal to the real appid.
    static bool RewriteAchievementCallbackGameId(int iCallback, void* pCallbackData,
                                                 int cubCallbackData)
    {
        AppId_t real = SteamCapture::OnlineFixRealAppId();
        if (real == 0 || real == kOnlineFixAppId) return false;
        if (cubCallbackData < static_cast<int>(sizeof(uint64_t))) return false;
        if (pCallbackData == nullptr) return false;

        auto* pGameId = static_cast<uint64_t*>(pCallbackData);
        AppId_t current = static_cast<AppId_t>(*pGameId & 0xFFFFFF);
        if (current != real) return false;

        *pGameId = (*pGameId & ~static_cast<uint64_t>(0xFFFFFF))
                 | static_cast<uint64_t>(kOnlineFixAppId);
        LOG_ONLINEFIX_DEBUG("GetAPICallResult achievement cb {} m_nGameID {} -> {}",
                            iCallback, real, kOnlineFixAppId);
        return true;
    }

    // ── Helper: write the GetAPICallResult response boilerplate ───
    template<typename CallbackT, typename F>
    bool WriteCallbackResponse(CUtlBuffer* pWrite, F&& fill)
    {
        constexpr int32 total = 1 + 1 + sizeof(CallbackT) + 1;
        if (pWrite->m_Put < total) return false;

        uint8* base = pWrite->m_Memory.m_pMemory;
        base[0] = IPC_REPLY_TAG;
        base[1] = 1;
        base[2 + sizeof(CallbackT)] = 0;

        auto* cb = reinterpret_cast<CallbackT*>(base + 2);
        fill(*cb);
        return true;
    }

    // ── Handler: IClientUtils::GetAppID ──────────────────────────
    //  SpawnProcess rewrites pGameID to 480 for OnlineFix games,
    //  so steamclient returns 480.  Restore the real app_id.
    void Cmd_IClientUtils_GetAppID(
        CSteamPipeClient* pipe, CUtlBuffer*, CUtlBuffer* pWrite)
    {
        AppId_t realAppId = SteamCapture::ResolveAppId();
        if (!realAppId || pWrite->m_Put < 5) return;

        AppId_t current = *reinterpret_cast<const AppId_t*>(pWrite->Base() + 1);
        if (current == realAppId) return;

        *reinterpret_cast<AppId_t*>(pWrite->Base() + 1) = realAppId;
        LOG_IPCCH_INFO("GetAppID: spoof response {} -> {}", current, realAppId);
    }

    // ════════════════════════════════════════════════════════════════
    //  GetAPICallResult per-callback handlers
    // ════════════════════════════════════════════════════════════════

    bool HandleCallback_EncryptedAppTicketResponse(
        CUtlBuffer* pWrite, uint64 hAsyncCall, uint32 cubCallback)
    {
        AppId_t appId = CmdUser::LookupEticketAsyncCall(hAsyncCall);
        if (!appId) return false;

        LOG_IPCCH_DEBUG("GetAPICallResult: EncryptedAppTicketResponse hAsyncCall=0x{:016X} "
                  "AppId={} - injecting k_EResultOK", hAsyncCall, appId);

        if (!WriteCallbackResponse<EncryptedAppTicketResponse_t>(pWrite, [](auto& cb) {
            cb.m_eResult = k_EResultOK;
        })) return false;

        CmdUser::EraseEticketAsyncCall(hAsyncCall);
        return true;
    }

    // ── Achievement-callback m_nGameID rewrite for GetAPICallResult ────────
    //
    // The IPC response sits in pWrite as
    //   [IPC_REPLY_TAG][success_flag][callback payload][0x00]
    // so the CGameID lives at offset 2 (start of payload). We rewrite the
    // low 24 bits from the real appid back to kOnlineFixAppId only when:
    //   * the OnlineFix session is active (g_OnlineFixRealAppId != 0)
    //   * the calling pipe matches the stamped g_StatsScopePipe (fine gate)
    //   * the success flag at offset 1 is 1
    //   * the response is large enough to hold the m_nGameID prefix
    //   * the low-24 bits of m_nGameID equal the real appid
    bool HandleCallback_AchievementStatsResult(
        HSteamPipe pipe, CUtlBuffer* pWrite, int iCallback, uint32 cubCallback)
    {
        if (SteamCapture::OnlineFixRealAppId() == 0) return false;
        if (SteamCapture::StatsScopePipe() != pipe) return false;
        if (cubCallback < sizeof(uint64_t)) return false;
        const int32 minTotal = static_cast<int32>(2 + sizeof(uint64_t));
        if (pWrite->m_Put < minTotal) return false;

        uint8* base = pWrite->Base();
        if (base[0] != IPC_REPLY_TAG || base[1] != 1) return false;

        return RewriteAchievementCallbackGameId(iCallback, base + 2,
                                                static_cast<int>(cubCallback));
    }

    bool HandleCallback_UserStatsReceived(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback) {
        return HandleCallback_AchievementStatsResult(
            pipe ? pipe->m_hSteamPipe : 0, pWrite,
            UserStatsReceived_t::k_iCallback, cubCallback);
    }

    bool HandleCallback_GlobalAchievementPercentagesReady(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback) {
        return HandleCallback_AchievementStatsResult(
            pipe ? pipe->m_hSteamPipe : 0, pWrite,
            GlobalAchievementPercentagesReady_t::k_iCallback, cubCallback);
    }

    bool HandleCallback_GlobalStatsReceived(
        CSteamPipeClient* pipe, CUtlBuffer* pWrite, uint64, uint32 cubCallback) {
        return HandleCallback_AchievementStatsResult(
            pipe ? pipe->m_hSteamPipe : 0, pWrite,
            GlobalStatsReceived_t::k_iCallback, cubCallback);
    }

    // The dispatch entry signature accepts a CSteamPipeClient* so the
    // achievement-stats handlers can read m_hSteamPipe for the fine gate.
    // EncryptedAppTicketResponse ignores the pipe argument and stays a
    // pure function of hAsyncCall.
    struct GacrDispatchEntry {
        uint32  callbackId;
        bool  (*handler)(CSteamPipeClient* pipe, CUtlBuffer* pWrite,
                         uint64 hAsyncCall, uint32 cubCallback);
    };

    static bool Adapt_EncryptedAppTicketResponse(
        CSteamPipeClient*, CUtlBuffer* pWrite, uint64 hAsyncCall, uint32 cubCallback) {
        return HandleCallback_EncryptedAppTicketResponse(pWrite, hAsyncCall, cubCallback);
    }

    constexpr GacrDispatchEntry g_GacrDispatch[] = {
        { EncryptedAppTicketResponse_t::k_iCallback,         Adapt_EncryptedAppTicketResponse },
        { UserStatsReceived_t::k_iCallback,                  HandleCallback_UserStatsReceived },
        { GlobalAchievementPercentagesReady_t::k_iCallback,  HandleCallback_GlobalAchievementPercentagesReady },
        { GlobalStatsReceived_t::k_iCallback,                HandleCallback_GlobalStatsReceived },
    };

    // ── Handler: IClientUtils::GetAPICallResult ──────────────────
    void Cmd_IClientUtils_GetAPICallResult(
        CSteamPipeClient* pipe, CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        if (pRead->m_Put < IPC_ARGS_OFFSET + sizeof(GetAPICallResultRequest)) return;

        const auto* req = reinterpret_cast<const GetAPICallResultRequest*>(
            pRead->Base() + IPC_ARGS_OFFSET);

        AppId_t appId = SteamCapture::GetAppIDForCurrentPipe();
        LOG_IPCCH_DEBUG("GetAPICallResult: hAsyncCall=0x{:016X} AppId={} iCallback={} cubCallback={}",
                  req->hSteamAPICall, appId, req->iCallbackExpected, req->cubCallback);
        for (auto& entry : g_GacrDispatch) {
            if (entry.callbackId == req->iCallbackExpected) {
                entry.handler(pipe, pWrite, req->hSteamAPICall, req->cubCallback);
                return;
            }
        }
    }

    const IPCBus::IpcHandlerEntry g_Entries[] = {
        REGISTER_IPC_CMD(IClientUtils, GetAppID),
        REGISTER_IPC_CMD(IClientUtils, GetAPICallResult),
    };

} // namespace

namespace CmdUtils {
    void Register() {
        IPCBus::RegisterHandlers(g_Entries, std::size(g_Entries));
    }
}
