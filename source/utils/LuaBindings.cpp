// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Implementation of the Lua C functions we expose to .lua scripts.
//
// Each binding goes through CheckAppId / CheckString / DecodeHex helpers
// instead of inlining the type / range checks. Errors raise a Lua error
// with a "where: what" message so script authors actually see what went
// wrong, rather than the empty-string error the older code threw.
//
// Bind_setStat stays achievement-ringfenced: same signature, same semantics,
// any change here risks the wire-level spoof gate that lives in PacketRouter.

#include "LuaLoaderInternal.h"
#include "Logger.h"
#include "Ticket.h"
#include "RuntimeHttp.h"

#include <lua.hpp>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
    // File-local strict-decimal uint64 parser used by every Lua binding that
    // takes a uint64-as-string argument. Rejects empty input, leading or
    // trailing whitespace, signs, and any 0x/0X prefix; the digit-only sweep
    // catches all of those before the std::stoull call. The try/catch keeps
    // both std::invalid_argument and std::out_of_range from escaping into
    // the Lua VM. `out` stays untouched on every failure path.
    bool TryParseUInt64Decimal(std::string_view text, uint64_t& out) {
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
        }
        try {
            std::string buf(text);
            size_t consumed = 0;
            uint64_t v = std::stoull(buf, &consumed, 10);
            if (consumed != buf.size()) return false;
            out = v;
            return true;
        } catch (const std::invalid_argument&) {
            return false;
        } catch (const std::out_of_range&) {
            return false;
        }
    }

    // Truncate to keep log lines bounded when a script ships a huge string.
    std::string_view TruncForLog(std::string_view s) {
        constexpr size_t kMax = 32;
        return s.size() > kMax ? s.substr(0, kMax) : s;
    }
}

namespace LuaLoader::Internal {

    // ── Typed argument helpers ───────────────────────────────────────────
    AppId_t CheckAppId(lua_State* L, int idx, const char* where) {
        if (!lua_isinteger(L, idx)) {
            luaL_error(L, "%s: arg #%d must be an integer", where, idx);
        }
        lua_Integer raw = lua_tointeger(L, idx);
        if (raw < 0 || raw > UINT32_MAX) {
            luaL_error(L, "%s: arg #%d out of uint32 range", where, idx);
        }
        return static_cast<AppId_t>(raw);
    }

    std::string_view CheckString(lua_State* L, int idx, const char* where) {
        if (!lua_isstring(L, idx)) {
            luaL_error(L, "%s: arg #%d must be a string", where, idx);
        }
        size_t len = 0;
        const char* p = lua_tolstring(L, idx, &len);
        return {p, len};
    }

    bool IsDecimalDigits(std::string_view s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    std::optional<std::vector<uint8_t>> DecodeHex(std::string_view hex) {
        std::vector<uint8_t> out;
        out.reserve((hex.size() + 1) / 2);

        for (size_t i = 0; i < hex.size(); i += 2) {
            char chunk[2];
            chunk[0] = hex[i];
            chunk[1] = (i + 1 < hex.size()) ? hex[i + 1] : '0';

            uint8_t byte = 0;
            auto [ptr, ec] = std::from_chars(chunk, chunk + 2, byte, 16);
            if (ec != std::errc{} || ptr != chunk + 2) {
                return std::nullopt;
            }
            out.push_back(byte);
        }
        return out;
    }

    // ── addappid(depotId [, _, key]) ─────────────────────────────────────
    int Bind_addappid(lua_State* L) {
        const int argc = lua_gettop(L);
        if (argc < 1) {
            return luaL_error(L, "addappid: need at least depotId");
        }

        AppId_t depotId = CheckAppId(L, 1, "addappid");

        // Optional 3rd arg is a 64-char hex key. Empty string keeps the
        // existing key (if any). Non-empty keys overwrite an empty key.
        std::string key;
        if (argc > 2) {
            std::string_view raw = CheckString(L, 3, "addappid");
            if (raw.size() == 64) {
                key.assign(raw.data(), raw.size());
            }
        }
        if (!key.empty() || !DepotKeySet.count(depotId)) {
            DepotKeySet[depotId] = key;
        }

        // Multi-account fix: clearing the owned flag on every addappid means
        // a secondary account adding a game previously marked owned by the
        // primary account re-patches correctly through CheckAppOwnership.
        OwnedAppIdSet.erase(depotId);

        if (g_activeSession) {
            g_activeSession->recordDepot(depotId);
        }
        return 0;
    }

    // ── addtoken(appId, tokenString) ─────────────────────────────────────
    int Bind_addtoken(lua_State* L) {
        const int argc = lua_gettop(L);
        if (argc < 1) {
            return luaL_error(L, "addtoken: need appId");
        }

        AppId_t appId = CheckAppId(L, 1, "addtoken");

        if (argc > 1) {
            std::string_view tok = CheckString(L, 2, "addtoken");
            uint64_t parsed = 0;
            if (!TryParseUInt64Decimal(tok, parsed)) {
                LOG_LUA_WARN("addtoken: rejected token '{}' (must be decimal uint64)",
                             TruncForLog(tok));
                return luaL_error(L, "addtoken: token must be a decimal uint64");
            }
            AccessTokenSet[appId] = parsed;
        }
        return 0;
    }

    // ── pinApp(appId) — currently unregistered; left compiled-in. ────────
    int Bind_pinApp(lua_State* L) {
        if (lua_gettop(L) < 1) {
            return luaL_error(L, "pinApp: need appId");
        }
        AppId_t appId = CheckAppId(L, 1, "pinApp");
        PinnedApps.insert(appId);
        return 0;
    }

    // ── setManifestid(depotId, gidString [, size]) ──────────────────────
    // The optional `size` is intentionally ignored — Steam rejects manifests
    // when the size doesn't line up with what the depot reports, so we force
    // 0 and let Steam fill it in.
    int Bind_setManifestid(lua_State* L) {
        if (lua_gettop(L) < 2) {
            return luaL_error(L, "setManifestid: need depotId, gid");
        }

        const AppId_t depotIdRaw = CheckAppId(L, 1, "setManifestid");
        const uint64_t depotId = static_cast<uint64_t>(depotIdRaw);

        std::string_view gid = CheckString(L, 2, "setManifestid");
        uint64_t parsedGid = 0;
        if (!TryParseUInt64Decimal(gid, parsedGid)) {
            LOG_LUA_WARN("setManifestid: rejected gid '{}' (must be decimal uint64)",
                         TruncForLog(gid));
            return luaL_error(L, "setManifestid: gid must be a decimal uint64");
        }

        ManifestOverrides[depotId] = { parsedGid, 0 };
        return 0;
    }

    // ── setAppTicket(appId, hexTicket) ───────────────────────────────────
    int Bind_setAppticket(lua_State* L) {
        if (lua_gettop(L) < 2) {
            return luaL_error(L, "setAppTicket: need appId and hex string");
        }

        AppId_t appId = CheckAppId(L, 1, "setAppTicket");
        std::string_view hex = CheckString(L, 2, "setAppTicket");

        auto decoded = DecodeHex(hex);
        if (!decoded) {
            return luaL_error(L, "setAppTicket: ticket must be hex");
        }

        if (!Ticket::WriteAppOwnershipTicket(appId, *decoded)) {
            return luaL_error(L, "setAppTicket: registry write failed");
        }
        return 0;
    }

    // ── setETicket(appId, hexTicket) ─────────────────────────────────────
    int Bind_setEticket(lua_State* L) {
        if (lua_gettop(L) < 2) {
            return luaL_error(L, "setETicket: need appId and hex string");
        }

        AppId_t appId = CheckAppId(L, 1, "setETicket");
        std::string_view hex = CheckString(L, 2, "setETicket");

        auto decoded = DecodeHex(hex);
        if (!decoded) {
            return luaL_error(L, "setETicket: ticket must be hex");
        }

        if (!Ticket::WriteEncryptedTicket(appId, *decoded)) {
            return luaL_error(L, "setETicket: registry write failed");
        }
        return 0;
    }

    // ── setStat(appId, "steamid") ────────────────────────────────────────
    // Achievement ringfence: behaviour identical to prior implementation.
    int Bind_setStat(lua_State* L) {
        if (lua_gettop(L) < 2) {
            return luaL_error(L, "setStat: need appId and steamId string");
        }

        AppId_t appId = CheckAppId(L, 1, "setStat");
        std::string_view sid = CheckString(L, 2, "setStat");

        uint64_t parsedSid = 0;
        if (!TryParseUInt64Decimal(sid, parsedSid)) {
            LOG_LUA_WARN("setStat: rejected steamId '{}' (must be decimal uint64)",
                         TruncForLog(sid));
            return luaL_error(L, "setStat: steamId must be a decimal uint64");
        }

        StatSteamIdSet[appId] = parsedSid;
        return 0;
    }

    // ── lcHttpGet(url) -> body, status ───────────────────────────────────
    // Plugin-side runtime HTTP GET. The 00_LetUpdate_override and any
    // future user-supplied .lua that wants to fetch a manifest GID
    // off a clearnet host can call this without going back through the
    // SteaMidra GUI. Body cap is 8 MiB and the total budget is 12s; both
    // are enforced inside RuntimeHttp::Get. On a network error the body
    // returned is the empty string and status is 0.
    int Bind_lcHttpGet(lua_State* L) {
        if (lua_gettop(L) < 1) {
            return luaL_error(L, "lcHttpGet: need url string");
        }
        std::string_view url = CheckString(L, 1, "lcHttpGet");
        const auto resp = RuntimeHttp::Get(url);
        if (resp.networkError) {
            LOG_LUA_DEBUG("lcHttpGet: net error '{}' for url='{}'",
                          resp.diagnostic, TruncForLog(url));
        }
        lua_pushlstring(L, resp.body.data(), resp.body.size());
        lua_pushinteger(L, static_cast<lua_Integer>(resp.status));
        return 2;
    }
}
