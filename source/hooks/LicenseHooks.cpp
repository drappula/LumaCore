// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "LicenseHooks.h"

#include "Macros.h"
#include "PatternDb.h"
#include "entry.h"
#include "utils/LuaLoader.h"

// Single hook: RequiresLegacyCDKey.
//
// Steam asks the wrapper for a CD key on a small set of pre-2010 titles
// (classic GTA, early Codemasters / Activision, some Bethesda back-catalog).
// For an app the user does not legitimately own there is no key to type and
// the prompt blocks launch. For Lua-tracked apps we answer false so the
// prompt never fires.
//
// The earlier attempt to use string-xref attach landed on the wrong target
// because the literal "RequiresLegacyCDKey" appears in a debug-name table
// inside steamclient64.dll, not inside the real function. Byte pattern is
// uniquely anchored on the function prologue + IPC allocator constants
// (BA 40 00 00 00 41 B8 20 00 00 00) which the steamclient_analyzer
// verified at matches=1 against the live on-disk DLL. Skipping string-xref
// avoids that hazard entirely.
//
// DLC ownership / install / cloud checks were intentionally NOT hooked.
// Steam already returns the right answer for Lua-tracked appids through the
// existing CheckAppOwnership patch, so adding BIsDlcEnabled etc would be
// redundant and re-introduce the same wrong-target risk.

namespace {

    LC_HOOK_DEF(RequiresLegacyCDKey, bool, void* pUser, AppId_t appId, uint32_t* pOut) {
        if (LuaLoader::HasDepot(appId)) {
            LOG_LICENSECH_INFO("RequiresLegacyCDKey: appId={} suppressed (Lua-tracked)", appId);
            if (pOut) *pOut = 0;
            return false;
        }
        return oRequiresLegacyCDKey(pUser, appId, pOut);
    }

}

namespace LicenseHooks {

    void Install() {
        LC_TX_OPEN();
        // Byte-pattern only — the *Sigs array entry is the analyzer-verified
        // prologue match (matches=1 against the loaded steamclient64.dll).
        LC_ATTACH_EX_D(RequiresLegacyCDKey, RequiresLegacyCDKeySigs);
        LC_TX_COMMIT();

        if (oRequiresLegacyCDKey) {
            LOG_LICENSECH_INFO("LicenseHooks::Install: RequiresLegacyCDKey attached at {}",
                               reinterpret_cast<void*>(oRequiresLegacyCDKey));
        } else {
            LOG_LICENSECH_INFO(
                "LicenseHooks::Install: RequiresLegacyCDKey skipped "
                "(byte pattern did not match this Steam build)");
        }
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(RequiresLegacyCDKey);
        LC_TX_COMMIT();
        LOG_LICENSECH_INFO("LicenseHooks::Uninstall: complete");
    }
}
