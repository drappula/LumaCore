// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "LicenseHooks.h"

#include "Macros.h"
#include "RuntimeCapture.h"
#include "entry.h"
#include "utils/LuaLoader.h"

// LicenseHooks owns two steamclient surfaces:
//
//   * OptedInMask         -> CSteamController opt-in mask. With the OnlineFix
//                            CGameID rewrite in flight, the controller layer
//                            asks for appid 480 and gets Spacewar's empty
//                            mask back. The detour swaps the query back to
//                            the real appid so controllers stay live under
//                            -onlinefix.
//
//   * RequiresLegacyCDKey -> Steam asks the wrapper for a CD key on a small
//                            set of pre-2010 titles when ownership crosses
//                            certain code paths. For Lua-tracked appids the
//                            owner doesn't have a real key, so returning
//                            false short-circuits the legacy-key prompt.
//
// DLC ownership / install / cloud / license-update / subscribed-app /
// ownership-ticket queries (BIsDlcEnabled, IsAppDlcInstalled,
// IsCloudEnabledForApp, BUpdateLicenses, GetSubscribedApps,
// BUpdateAppOwnershipTicket) were intentionally NOT hooked here. Steam
// already returns the right answer for Lua-tracked appids through the
// existing CheckAppOwnership patch, so installing detours on top of those
// surfaces is redundant. Detouring them with hand-rolled signatures also
// risks stack corruption on x64 fastcall when an argument count or type
// is even slightly off, which is what surfaced as a random Steam crash a
// few minutes into a session and as cloud-save toggles flipping on for
// every tracked game.
//
// The patterns for those six functions still ride in the per-build TOML
// (the analyzer keeps detecting them) so any future hook code that needs
// them can resolve their addresses without changing the pattern publisher
// or the cache layout.

namespace {

    LC_HOOK_DEF(OptedInMask, __int64, void* pThis, unsigned int appId) {
        AppId_t realAppId = SteamCapture::OnlineFixRealAppId();
        if (appId == kOnlineFixAppId && realAppId) {
            LOG_MISC_INFO("OptedInMask: appid {} -> {}", appId, realAppId);
            return oOptedInMask(pThis, realAppId);
        }
        LOG_MISC_TRACE("OptedInMask: appid {} (realAppId={}, no redirect)",
                       appId, realAppId);
        return oOptedInMask(pThis, appId);
    }

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
        LC_ATTACH_D(OptedInMask);
        LC_ATTACH_D(RequiresLegacyCDKey);
        LC_TX_COMMIT();

        LOG_LICENSECH_INFO(
            "LicenseHooks::Install: OptedInMask={} RequiresLegacyCDKey={}",
            oOptedInMask         ? "attached" : "skipped (TOML entry missing)",
            oRequiresLegacyCDKey ? "attached" : "skipped (TOML entry missing)");
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(RequiresLegacyCDKey);
        LC_DETACH(OptedInMask);
        LC_TX_COMMIT();
        LOG_LICENSECH_INFO("LicenseHooks::Uninstall: complete");
    }

}
