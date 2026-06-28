// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

// Hub that registers all interface-specific IPC handler sets and installs
// the IpcDispatch dispatch layer atop IPCBus. Each IpcHandlers_*::Register()
// function registers pre/post pairs through IpcDispatch::Register(), which
// resolves the func hash from IpcLoader metadata and pushes the handler
// into IPCBus's existing dispatch table.

#include "hooks/client/IpcDispatch.h"
#include "runtime/Logger.h"

// Forward declarations for interface handler registration functions.
// Each one lives in its own translation unit under hooks/client/.
namespace IpcHandlers_ISteamUser { void Register(); }
namespace IpcHandlers_ISteamUtils { void Register(); }

namespace IpcHooks {

    void Install() {
        // Load IPC method metadata TOML first
        // (called earlier from Bootstrap but idempotent)

        // Register all interface-specific handler sets
        IpcHandlers_ISteamUser::Register();
        IpcHandlers_ISteamUtils::Register();

        // Push all registered handlers into IPCBus's dispatch table
        IpcDispatch::Install();

        LOG_IPC_INFO("IpcHooks: all IPC handlers installed");
    }

    void Uninstall() {
        IpcDispatch::Uninstall();
    }

}
