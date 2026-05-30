// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#ifndef LUALOADER_H
#define LUALOADER_H

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>

namespace LuaLoader {
    bool HasDepot(AppId_t appId);
    bool IsOwned(AppId_t appId);
    void MarkOwned(AppId_t appId);
    std::vector<AppId_t> GetAllDepotIds();
    std::vector<uint8> GetDecryptionKey(AppId_t appId);
    uint64_t GetAccessToken(AppId_t appId);
    uint64_t GetStatSteamId(AppId_t appId);
    // Returns the full fallback pool of SteamIDs for achievement schema fetching.
    // If setStat() was configured for appId, outCount=1 and returns pointer to that ID.
    // Otherwise returns the built-in pool. PacketRouter tries each in order.
    const uint64_t* GetStatSteamIdPool(AppId_t appId, size_t& outCount);
    bool pinApp(AppId_t appId);

    // .lua mtime indexed per appid so the host can feed Steam's "added"
    // timestamp from when the user dropped the .lua file in. The library
    // "Date Added" sort wants seconds since epoch as int64 RTC time. Zero
    // when the appid wasn't registered through ParseFile (legacy entries
    // captured by addappid in the same process before ParseFile ran).
    int64_t GetLuaMtime(AppId_t appId);

    struct ManifestOverride {
          uint64_t gid;
          uint64_t size;
    };
    const std::unordered_map<uint64_t, ManifestOverride>& GetManifestOverrides();

    void ParseFile(const std::string& filePath);
    void UnloadFile(const std::string& filePath);
    // Returns and clears the list of depot IDs removed/added since last call.
    std::vector<AppId_t> TakePendingRemovals();
    std::vector<AppId_t> TakePendingAdditions();
    void ParseDirectory(const std::string& directory);

    // Re-queue all currently loaded depot IDs as pending additions.
    // Called after startup when hooks are ready but LoadPackage already fired.
    // This allows NotifyLicenseChanged to inject all startup Lua files into
    // the already-loaded package 0.
    void QueueStartupInjection();

}

#endif // LUALOADER_H
