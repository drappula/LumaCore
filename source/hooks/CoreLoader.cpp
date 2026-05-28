// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "CoreLoader.h"
#include "DepotKeys.h"
#include "IPCBus.h"
#include "KeyValues.h"
#include "ManifestBind.h"
#include "PatternFetcher.h"
#include "SteamCapture.h"
#include "SteamUI.h"
#include "PacketRouter.h"
#include "PackagePatch.h"
#include "LicenseHooks.h"
#include "utils/Diagnostics.h"


namespace LumaCore {

    void Attach() {
        DepotKeys::Install();
        IPCBus::Install();
        KVHooks::Install();
        ManifestBind::Install();
        SteamCapture::Install();
        PacketRouter::Install();
        // PackagePatch::Install() is called early in entry.cpp InitThread,
        // immediately after LoadDiversion(), to catch LoadPackage before Steam calls it.
        LicenseHooks::Install();
    }

    void Detach() {
#ifdef LUMACORE_DIAGNOSTICS_ENABLED
        // A16 auto-flush: write the achievement diagnostic ring to
        // <AppData>\\SteaMidra\\lumacore_diag.txt before tearing down
        // the hooks. Steam restart wipes the ring otherwise.
        Diagnostics::DumpForDetach();
#endif
        DepotKeys::Uninstall();
        IPCBus::Uninstall();
        KVHooks::Uninstall();
        ManifestBind::Uninstall();
        SteamCapture::Uninstall();
        SteamUI::CoreUnhook();
        PacketRouter::Uninstall();
        PackagePatch::Uninstall();
        LicenseHooks::Uninstall();
        // Drop the runtime pattern map last; nothing else looks it up here.
        PatternFetcher::Reset();
    }
}
