// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once

// ── ISteamUser callbacks (base = 100) ───────────────────────────────

constexpr int k_iSteamUserCallbacks = 100;

//-----------------------------------------------------------------------------
// Purpose: Result from RequestEncryptedAppTicket (async)
//-----------------------------------------------------------------------------
struct EncryptedAppTicketResponse_t
{
	enum { k_iCallback = k_iSteamUserCallbacks + 54 };

	EResult m_eResult;
};

//-----------------------------------------------------------------------------
// Purpose: Broadcast when app licenses change (additions / removals / reload).
//          Sent by CClientAppManager after ProcessPendingLicenseUpdates.
//          Size: 0x118 (280 bytes).
//-----------------------------------------------------------------------------
struct AppLicensesChanged_t
{
	enum { k_iCallback = 1020094 };

	bool      m_bReloadAll;                // 0x00  — true = full library refresh
	bool      m_bIsFirstLoad;              // 0x01
	uint32    m_unRemainingPackets;         // 0x04
	uint32    m_unCount;                    // 0x08  — number of entries in m_rgAppsUpdated
	AppId_t   m_rgAppsUpdated[64];         // 0x0C  — batch of updated AppIds
	uint64    m_unAppsAdded;               // 0x110 — bitmask: bit N = m_rgAppsUpdated[N] was added
};
static_assert(sizeof(AppLicensesChanged_t) == 0x118,
              "AppLicensesChanged_t must be 0x118 bytes");

//-----------------------------------------------------------------------------
// Purpose: Fires when an achievement is committed to Steam's servers.
//          Steam re-emits this for cached unlocks at login, which is how
//          stale unlocks from a prior account/install bleed back into the
//          overlay after switching users. SendCallbackToPipe drops the
//          callback for configured appids before it reaches the UI.
//
//          Layout: starts with a CGameID (low 24 bits = AppId), so we can
//          read the appid by reinterpreting the first 8 bytes of the
//          callback payload.
//-----------------------------------------------------------------------------
struct UserAchievementStored_t
{
	static constexpr int k_iCallback = 1103;
};

//-----------------------------------------------------------------------------
// Purpose: Schema delivery for an appid (callback id 1102). Steam re-emits
//          cached schemas at login, which is how stale unlocks for an appid
//          bind back into the overlay even after the values cache (callback
//          1103) is wiped. A16 drops 1102 for configured appids alongside
//          the existing 1103 drop.
//
//          Layout: starts with a CGameID, so the appid extraction matches
//          1103 (low 24 bits of the first 8 bytes of the payload).
//-----------------------------------------------------------------------------
struct UserStatsReceived_t
{
	static constexpr int k_iCallback = 1102;
};
