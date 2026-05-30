// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Internal implementation header for the LuaLoader namespace.
//
// LuaLoader.cpp was split into three translation units (LuaState.cpp,
// LuaBindings.cpp, LuaQuery.cpp). Each of them needs the same module-level
// state plus a handful of helpers that don't belong in the public header.
// Consolidate the internals here so LuaLoader.h stays a tight public surface
// and the three .cpp files share one declaration set.

#pragma once

#include "entry.h"
#include "LuaLoader.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
    struct lua_State;
}

namespace LuaLoader::Internal {
    // ── Global lua_State + storage ────────────────────────────────────────
    extern lua_State* g_lua_state;

    extern std::unordered_map<AppId_t, std::string> DepotKeySet;
    extern std::unordered_map<AppId_t, uint64_t>    AccessTokenSet;
    extern std::unordered_set<AppId_t>              PinnedApps;
    extern std::unordered_map<uint64_t, ManifestOverride> ManifestOverrides;
    extern std::unordered_map<AppId_t, uint64_t>    StatSteamIdSet;
    extern std::unordered_set<AppId_t>              OwnedAppIdSet;
    // Per-app .lua mtime stamp, populated when ParseFile runs successfully.
    // Keyed by appid (whatever the .lua's stem encodes); seconds since epoch.
    extern std::unordered_map<AppId_t, int64_t>     LuaMtimeMap;

    // ── Per-file parse session ────────────────────────────────────────────
    // ParseFile populates `currentFile`, every successful binding pushes
    // depots through `recordDepot`, and the caller publishes pending
    // additions/removals once the session ends.
    struct ParseSession {
        std::string currentFile;
        // Records that `id` was contributed by `currentFile`. Bumps the
        // ref-count and flags it as a pending addition the first time.
        void recordDepot(AppId_t id);
    };

    // Active session pointer, set by ParseFile, cleared at end-of-scope.
    // LuaBindings.cpp reads this to know which file the caller belongs to.
    extern ParseSession* g_activeSession;

    // ── Shared cross-file state ───────────────────────────────────────────
    extern std::unordered_map<std::string, std::unordered_set<AppId_t>> g_fileDepots;
    extern std::unordered_map<AppId_t, uint32_t> g_depotRefCount;
    extern std::vector<AppId_t> g_pendingRemovals;
    extern std::vector<AppId_t> g_pendingAdditions;

    constexpr uint64_t kDefaultStatSteamId = 76561198028121353ULL;

    // Achievement-ringfenced: this pool feeds the wire-level UserStats
    // spoofer. Keep byte-identical with the rest of the achievement code.
    extern const uint64_t kStatSteamIdPool[15];

    // ── State lifecycle ───────────────────────────────────────────────────
    bool Initialize();
    void Cleanup();

    // ── Lua bindings (registered by LuaState.cpp, defined in LuaBindings.cpp)
    int Bind_addappid(lua_State* L);
    int Bind_addtoken(lua_State* L);
    int Bind_pinApp(lua_State* L);
    int Bind_setManifestid(lua_State* L);
    int Bind_setAppticket(lua_State* L);
    int Bind_setEticket(lua_State* L);
    int Bind_setStat(lua_State* L);
    int Bind_lcHttpGet(lua_State* L);

    // ── Typed argument validator ─────────────────────────────────────────
    // Returns the argument as a typed value when the position holds the
    // expected Lua type AND (for integers) fits the requested range. On
    // failure, raises a structured Lua error with `where` + `what` so the
    // user sees something useful instead of an empty string.
    struct ArgError {};

    // Reads arg `idx` as an integer and validates 0 <= value <= UINT32_MAX.
    // On success returns the value; on failure raises a Lua error and
    // never returns (luaL_error performs longjmp).
    AppId_t CheckAppId(lua_State* L, int idx, const char* where);

    // Reads arg `idx` as a string. On failure raises a Lua error.
    std::string_view CheckString(lua_State* L, int idx, const char* where);

    // Returns true when `s` is non-empty and consists only of decimal digits.
    bool IsDecimalDigits(std::string_view s);

    // Decodes a hex string (e.g. "0a1bff") into raw bytes using std::from_chars.
    // Replaces the older `char[3] + strtoul` byte-by-byte construction. Treats
    // odd-length input by zero-padding the trailing nibble (matches prior
    // behaviour). Returns nullopt on a non-hex character.
    std::optional<std::vector<uint8_t>> DecodeHex(std::string_view hex);
}
