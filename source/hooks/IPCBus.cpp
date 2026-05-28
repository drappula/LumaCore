// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "IPCBus.h"
#include "CmdUser.h"
#include "CmdUtils.h"
#include "Macros.h"
#include "entry.h"
#include "utils/Hash.h"
#include "SteamCapture.h"
#include <unordered_map>

namespace {

    using GetPipeClient_t = CSteamPipeClient*(*)(void* pEngine, HSteamPipe hSteamPipe);
    GetPipeClient_t oGetPipeClient = nullptr;

    static CSteamPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    // ▌▌ LumaCore ▌ IPC ▌ Handler registry
    // ▌▌
    using namespace IPCBus;

    static constexpr uint64 MakeHandlerKey(EIPCInterface iface, uint32 funcHash) {
        return (static_cast<uint64>(iface) << 32) | funcHash;
    }

    std::unordered_map<uint64, IpcHandlerEntry> g_Handlers;

    static const IpcHandlerEntry* FindHandler(EIPCInterface iface, uint32 funcHash) {
        auto it = g_Handlers.find(MakeHandlerKey(iface, funcHash));
        return (it != g_Handlers.end()) ? &it->second : nullptr;
    }

    // ▌▌ LumaCore ▌ IPC ▌ Main hook
    // ▌▌
    LC_HOOK_DEF(IPCProcessMessage, bool,
              void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        auto* pipe = GetPipe(pServer, hSteamPipe);

        // ▌ IPC ▌ Always log every incoming IPC, before any filter
        // Helps diagnose ticket-validation flows that may be silently
        // skipped by the pipe-handle filter below.
        if (pRead->TellPut() >= IPC_HEADER_SIZE) {
            const uint8* rawData = pRead->Base();
            const auto rawCmd = static_cast<EIPCCommand>(rawData[OFFSET_CMD]);
            const int32 rawSize = pRead->TellPut();
            std::string preview;
            const int32 dumpN = rawSize > 32 ? 32 : rawSize;
            char tmp[4];
            preview.reserve(dumpN * 3);
            for (int32 idx = 0; idx < dumpN; ++idx) {
                std::snprintf(tmp, sizeof(tmp), "%02X ", rawData[idx]);
                preview.append(tmp);
            }
            LOG_IPCCH_INFO("RAW IPC: cmd={} pipe=0x{:08X} size={} head[hex]={}",
                         EIPCCommandName(rawCmd),
                         pipe ? pipe->m_hSteamPipe : 0u,
                         rawSize, preview);
        }

        // ▌ IPC ▌ Parse header, find handler
        const IpcHandlerEntry* handlerEntry = nullptr;
        // userStatsCall is true exactly when the parsed interface is
        // IClientUserStats. Drives the SetUserStatsContext bracket
        // around oIPCProcessMessage so the GetAppIDForCurrentPipe
        // detour returns the real appid (not the 480 masquerade) for
        // the duration of the IPC. Lobby / friends / controller /
        // RemoteStorage paths leave the flag false and stay byte-
        // identical to the existing 480 behaviour.
        bool userStatsCall = false;

        if (pRead->TellPut() >= IPC_HEADER_SIZE) {
            const uint8* pktData = pRead->Base();
            const auto cmd = static_cast<EIPCCommand>(pktData[OFFSET_CMD]);

            if (cmd == EIPCCommand::Handshake) {
                if (pipe) LOG_IPCCH_INFO("[Handshake]: {}", pipe->DebugString());
            } else if (cmd == EIPCCommand::InterfaceCall) {
                // exclude InterfaceCall from steam
                if (!pipe || (pipe->m_hSteamPipe & 0xFFFF) <= 2) {
                    if (pipe) LOG_IPCCH_INFO("[InterfaceCall] from steam, pipe=0x{:08X} skip handler", pipe->m_hSteamPipe);
                    return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
                }
                const auto iface = static_cast<EIPCInterface>(pktData[OFFSET_INTERFACE_ID]);
                const uint32 funcHash = *reinterpret_cast<const uint32*>(pktData + OFFSET_FUNC_HASH);
                userStatsCall = (iface == EIPCInterface::IClientUserStats);
                handlerEntry = FindHandler(iface, funcHash);
                if (handlerEntry) {
                    LOG_IPCCH_INFO("[InterfaceCall] {} {} realAppId={},AppId={}",
                                  handlerEntry->name, pipe ? pipe->DebugString() : "pipe=null",
                                  SteamCapture::ResolveAppId(),
                                  SteamCapture::GetAppIDForCurrentPipe()
                                );
                } else {
                    LOG_IPCCH_INFO("[InterfaceCall(unhandled)]{}::0x{:08X} {} realAppId={},AppId={}",
                                  EIPCInterfaceName(iface), funcHash,
                                  pipe ? pipe->DebugString() : "pipe=null",
                                  SteamCapture::ResolveAppId(),
                                  SteamCapture::GetAppIDForCurrentPipe()
                                );
                }
            } else {
                if (pipe) LOG_IPCCH_INFO("[{}] {}", EIPCCommandName(cmd), pipe->DebugString());
            }
        }

        // ▌ IPC ▌ Run original
        // Scope is open only for IClientUserStats so the lobby /
        // friends / controller / RemoteStorage pass-through that
        // depends on the 480 masquerade stays byte-identical.
        // The pipe-scoped fine gate (g_StatsScopePipe) wraps both the
        // original call AND the post-dispatch handler so CmdUtils
        // GetAPICallResult rewrites observe the scope.
        if (userStatsCall) {
            SteamCapture::SetUserStatsContext(true);
            SteamCapture::EnterStatsScope(hSteamPipe);
        }
        const bool outcome = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);
        if (!outcome || !handlerEntry) {
            if (userStatsCall) {
                SteamCapture::LeaveStatsScope();
                SteamCapture::SetUserStatsContext(false);
            }
            return outcome;
        }

        // Only run handlers for apps with configured depots.
        AppId_t appId = SteamCapture::ResolveAppId();
        if (!LuaLoader::HasDepot(appId)) {
            LOG_IPCCH_INFO("{}: appId={} has no configured depot, skip handler {}",
                handlerEntry->name, appId, pipe ? pipe->DebugString() : "pipe=null");
            if (userStatsCall) {
                SteamCapture::LeaveStatsScope();
                SteamCapture::SetUserStatsContext(false);
            }
            return outcome;
        }

        handlerEntry->handler(pipe, pRead, pWrite);
        if (userStatsCall) {
            SteamCapture::LeaveStatsScope();
            SteamCapture::SetUserStatsContext(false);
        }
        return outcome;
    }

} // namespace


namespace IPCBus {

    void RegisterHandlers(const IpcHandlerEntry* entries, size_t count) {
        g_Handlers.reserve(g_Handlers.size() + count);
        for (size_t idx = 0; idx < count; ++idx)
            g_Handlers.emplace(MakeHandlerKey(entries[idx].interfaceID, entries[idx].funcHash), entries[idx]);
    }

    void Install() {
        LC_RESOLVE_D(GetPipeClient);

        // Interface modules register their handlers here.
        CmdUser::Register();
        CmdUtils::Register();

        LC_TX_OPEN();
        LC_ATTACH_D(IPCProcessMessage);
        LC_TX_COMMIT();

        LOG_IPCCH_INFO("IPCBus: install complete, hook at 0x{:X}",
                       reinterpret_cast<uintptr_t>(oIPCProcessMessage));
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(IPCProcessMessage);
        LC_TX_COMMIT();
        oGetPipeClient = nullptr;
        g_Handlers.clear();
    }

}
