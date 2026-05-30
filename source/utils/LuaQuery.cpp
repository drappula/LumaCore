// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Public LuaLoader query API plus the directory / per-file parser orchestration.
//
// ParseFile uses a stack-allocated ParseSession that records depots through
// the bindings as they fire, then publishes pending additions/removals when
// the session ends. The chunk-by-chunk line accumulator the previous
// implementation used is gone; modern Lua handles multi-line statements
// with a single luaL_loadstring call, and per-line error context is
// available through luaL_loadbuffer's chunk name.

#include "LuaLoaderInternal.h"
#include "Logger.h"

#include <lua.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    // File-local strict-decimal uint64 parser, re-declared per translation
    // unit so neither LuaBindings.cpp nor LuaQuery.cpp pulls a shared header.
    // Behaviour matches the LuaBindings copy: digit-only, no whitespace, no
    // signs, no 0x/0X prefix; std::stoull is wrapped so neither
    // std::invalid_argument nor std::out_of_range escapes.
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
}

namespace LuaLoader {

    // ── public query surface ──────────────────────────────────────────────
    bool HasDepot(AppId_t depotId) {
        using namespace Internal;
        return DepotKeySet.count(depotId) && !OwnedAppIdSet.count(depotId);
    }

    bool IsOwned(AppId_t appId) {
        using namespace Internal;
        return OwnedAppIdSet.count(appId) > 0;
    }

    int64_t GetLuaMtime(AppId_t appId) {
        using namespace Internal;
        auto it = LuaMtimeMap.find(appId);
        return it == LuaMtimeMap.end() ? 0 : it->second;
    }

    void MarkOwned(AppId_t appId) {
        using namespace Internal;
        if (OwnedAppIdSet.insert(appId).second) {
            LOG_PACKAGE_INFO("Marking app {} as owned", appId);
        }
    }

    std::vector<AppId_t> GetAllDepotIds() {
        using namespace Internal;
        std::vector<AppId_t> ids;
        ids.reserve(DepotKeySet.size());
        for (const auto& [id, _] : DepotKeySet) ids.push_back(id);
        return ids;
    }

    std::vector<uint8> GetDecryptionKey(AppId_t depotId) {
        using namespace Internal;
        std::vector<uint8> bytes;
        auto it = DepotKeySet.find(depotId);
        if (it == DepotKeySet.end()) return bytes;

        const std::string& hex = it->second;
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            uint8_t b = 0;
            auto [_, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, b, 16);
            if (ec == std::errc{}) {
                bytes.push_back(b);
            }
        }
        return bytes;
    }

    uint64_t GetAccessToken(AppId_t appId) {
        using namespace Internal;
        auto it = AccessTokenSet.find(appId);
        return it != AccessTokenSet.end() ? it->second : 0;
    }

    bool pinApp(AppId_t appId) {
        return Internal::PinnedApps.count(appId) > 0;
    }

    // Achievement ringfence: byte-identical semantics with prior version.
    uint64_t GetStatSteamId(AppId_t appId) {
        using namespace Internal;
        auto it = StatSteamIdSet.find(appId);
        return it != StatSteamIdSet.end() ? it->second : kDefaultStatSteamId;
    }

    // Achievement ringfence: hands the wire-level UserStats spoofer either
    // a single configured stat steamid or the full fallback pool.
    const uint64_t* GetStatSteamIdPool(AppId_t appId, size_t& outCount) {
        using namespace Internal;
        auto it = StatSteamIdSet.find(appId);
        if (it != StatSteamIdSet.end()) {
            outCount = 1;
            return &it->second;
        }
        outCount = sizeof(kStatSteamIdPool) / sizeof(kStatSteamIdPool[0]);
        return kStatSteamIdPool;
    }

    const std::unordered_map<uint64_t, ManifestOverride>& GetManifestOverrides() {
        return Internal::ManifestOverrides;
    }

    // ── per-file unload ───────────────────────────────────────────────────
    void UnloadFile(const std::string& filePath) {
        using namespace Internal;
        auto it = g_fileDepots.find(filePath);
        if (it == g_fileDepots.end()) return;

        for (AppId_t id : it->second) {
            LOG_PACKAGE_DEBUG("UnloadFile:Ref count for AppId {} is {}", id, g_depotRefCount[id]);
            auto refIt = g_depotRefCount.find(id);
            if (refIt != g_depotRefCount.end() && --refIt->second == 0) {
                g_depotRefCount.erase(refIt);
                DepotKeySet.erase(id);
                g_pendingRemovals.push_back(id);
            }
        }

        LOG_PACKAGE_INFO("UnloadFile: removed {} depots from {}", it->second.size(), filePath);
        g_fileDepots.erase(it);
    }

    std::vector<AppId_t> TakePendingRemovals() {
        std::vector<AppId_t> out;
        out.swap(Internal::g_pendingRemovals);
        return out;
    }

    std::vector<AppId_t> TakePendingAdditions() {
        std::vector<AppId_t> out;
        out.swap(Internal::g_pendingAdditions);
        return out;
    }

    // ── single-file parser ───────────────────────────────────────────────
    void ParseFile(const std::string& filePath) {
        using namespace Internal;
        if (!Initialize()) return;

        UnloadFile(filePath);

        ParseSession session;
        session.currentFile = filePath;
        g_activeSession = &session;
        struct SessionGuard {
            ~SessionGuard() { g_activeSession = nullptr; }
        } guard;

        std::filesystem::path path(filePath);

        // Stamp the .lua's last-write time so the host can tell Steam's
        // appinfo "added" timestamp where the user dropped the file. Library
        // sort by Date Added relies on that field; without it Steam picks
        // the install/launch order which is wrong for fake-owned games.
        int64_t lua_mtime_secs = 0;
        {
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (GetFileAttributesExA(filePath.c_str(), GetFileExInfoStandard, &attr)) {
                ULARGE_INTEGER ull{};
                ull.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
                ull.HighPart = attr.ftLastWriteTime.dwHighDateTime;
                // FILETIME is 100ns ticks since 1601-01-01. Shift to unix
                // epoch and convert to seconds.
                constexpr uint64_t kEpochOffset = 116444736000000000ull;
                if (ull.QuadPart >= kEpochOffset) {
                    lua_mtime_secs = static_cast<int64_t>((ull.QuadPart - kEpochOffset) / 10000000ull);
                }
            }
        }

        // Auto-register the appid that the filename stem encodes (e.g. a
        // file named "3764200.lua" registers depot 3764200 even if the
        // .lua body only calls addappid() on auxiliary depots). Also
        // re-clears OwnedAppIdSet for that appid so multi-account swaps
        // don't keep showing "Purchase".
        {
            const std::string stem = path.stem().string();
            if (!stem.empty()
                && std::all_of(stem.begin(), stem.end(),
                                [](unsigned char c){ return std::isdigit(c); })) {
                uint64_t val = 0;
                if (TryParseUInt64Decimal(stem, val) && val > 0 && val <= UINT32_MAX) {
                    AppId_t fileAppId = static_cast<AppId_t>(val);

                    if (OwnedAppIdSet.erase(fileAppId)) {
                        LOG_PACKAGE_INFO("ParseFile: clearing owned status for appid={} (Lua re-added)", fileAppId);
                    }
                    if (!DepotKeySet.count(fileAppId)) {
                        DepotKeySet[fileAppId] = "";
                        session.recordDepot(fileAppId);
                        LOG_DEBUG("ParseFile: auto-registered appid={} from filename {}", fileAppId, stem);
                    }
                    if (lua_mtime_secs > 0) {
                        LuaMtimeMap[fileAppId] = lua_mtime_secs;
                    }
                }
            }
        }

        // Slurp the file in one shot. The previous chunk-accumulator loop
        // existed only to retry per line on syntax errors; modern Lua
        // handles multi-line statements directly through luaL_loadbuffer.
        std::ifstream file(path);
        if (!file) {
            LOG_WARN("ParseFile: failed to open {}", path.filename().string());
            return;
        }
        std::stringstream buf;
        buf << file.rdbuf();
        std::string body = buf.str();

        const std::string chunkName = path.filename().string();
        lua_settop(g_lua_state, 0);
        int rc = luaL_loadbuffer(g_lua_state, body.data(), body.size(), chunkName.c_str());
        if (rc == LUA_OK) {
            if (lua_pcall(g_lua_state, 0, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(g_lua_state, -1);
                LOG_WARN("{}: {}", chunkName, err ? err : "unknown");
                lua_pop(g_lua_state, 1);
            }
        } else {
            const char* err = lua_tostring(g_lua_state, -1);
            LOG_WARN("{}: {}", chunkName, err ? err : "unknown");
            lua_pop(g_lua_state, 1);
        }
    }

    // ── directory scanner ────────────────────────────────────────────────
    void ParseDirectory(const std::string& directory) {
        using namespace Internal;
        if (!Initialize()) return;

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            std::filesystem::create_directories(directory, ec);
        }
        if (!std::filesystem::exists(directory, ec)
            || !std::filesystem::is_directory(directory, ec)) {
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".lua") continue;
            ParseFile(entry.path().string());
        }

        // The first directory pass populates DepotKeySet but we don't want
        // those entries to count as "post-startup additions" — they were
        // present at boot. Discard the queue.
        g_pendingAdditions.clear();
    }

    // ── startup injection ────────────────────────────────────────────────
    // Re-queue every loaded depot as a pending addition. RuntimeCapture
    // calls this after MarkLicenseAsChanged fires post-login so package 0
    // can absorb everything in one go via NotifyLicenseChanged.
    void QueueStartupInjection() {
        using namespace Internal;
        g_pendingAdditions.clear();
        g_pendingAdditions.reserve(DepotKeySet.size());
        for (const auto& [id, _] : DepotKeySet) {
            g_pendingAdditions.push_back(id);
        }
        LOG_PACKAGE_INFO("QueueStartupInjection: queued {} depot IDs for injection",
                         g_pendingAdditions.size());
    }
}
