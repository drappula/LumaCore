// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once

#include "entry.h"

// Captures runtime Steam object pointers and handles lightweight hooks that
// don't belong in a dedicated category:
//   * GetAppIDForCurrentPipe  -> Detours hook; captures the SteamEngine
//                                pointer on first call and applies the
//                                scoped real-appid override for IClientUserStats
//                                traffic (see SetUserStatsContext)
//   * SpawnProcess            -> OnlineFix detection + 480 rewrite
//   * GetAppDataFromAppInfo   -> int3 trap; captures the CAppInfoCache pointer
//   * MarkLicenseAsChanged    -> captures pCUser; resolved for NotifyLicenseChanged
//   * GetPackageInfo          -> captures pCPackageInfo; used by NotifyLicenseChanged to append AppIds
//   * ProcessPendingLicenseUpdates -> resolved for NotifyLicenseChanged
namespace SteamCapture {
    void Install();
    void Uninstall();

    // Returns the AppId for the current Steam pipe via the captured engine
    // pointer, or 0 if we haven't yet observed the host calling
    // GetAppIDForCurrentPipe.
    AppId_t GetAppIDForCurrentPipe();

    // Grow a CUtlBuffer to at least 'size' bytes and set m_Put = size.
    // Uses CUtlBuffer::EnsureCapacity from steamclient, resolved on first call.
    void EnsureBufferSize(CUtlBuffer* pWrite, int32 size);

    // Resolve the real appid: if OnlineFix is active return real appid,
    // otherwise fall back to GetAppIDForCurrentPipe().
    AppId_t ResolveAppId();

    // Scoped real-appid override for IClientUserStats traffic. Increments
    // a thread-local depth counter on active=true and decrements on
    // active=false (underflow guarded). The GetAppIDForCurrentPipe detour
    // returns the real appid only while depth > 0 AND OnlineFix is active
    // AND the original engine call returned the Spacewar masquerade.
    void SetUserStatsContext(bool active);

    // Get localized game name via GetAppDataFromAppInfo (cached).
    std::string GetGameNameByAppID(AppId_t appId);

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates
    // Requires pCUser to have been captured (happens on first natural call to
    // MarkLicenseAsChanged, which Steam makes during license load on startup).
    void NotifyLicenseChanged();

    // Returns true when all captures needed by NotifyLicenseChanged are ready.
    // Used by the startup injection thread to know when it's safe to call NotifyLicenseChanged.
    bool IsReadyForNotify();
}
