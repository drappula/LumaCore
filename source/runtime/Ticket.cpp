// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "Ticket.h"
#include "hooks/client/DecryptionKeyHook.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace Ticket {

    namespace {
        constexpr const char* kSteamIdValue = "SteamID";
        constexpr const char* kAppTicketValue = "AppTicket";
        constexpr const char* kEncryptedTicketValue = "ETicket";
        constexpr DWORD kMaxTicketRegistryBytes = 1024 * 1024;

        std::string AppRegistryPath(AppId_t appId) {
            return "Software\\Valve\\Steam\\Apps\\" + std::to_string(appId);
        }

        bool IsAppManagedByLua(AppId_t appId, const char* caller) {
            if (LuaLoader::HasDepot(appId)) return true;
            LOG_DEBUG("{} for AppId {}: not in addappid, skip", caller, appId);
            return false;
        }

        std::vector<uint8_t> ReadBinaryValue(AppId_t appId, const char* valueName) {
            const std::string regPath = AppRegistryPath(appId);
            DWORD valueType = 0;
            DWORD valueSize = 0;
            LSTATUS status = RegGetValueA(
                HKEY_CURRENT_USER,
                regPath.c_str(),
                valueName,
                RRF_RT_REG_BINARY,
                &valueType,
                nullptr,
                &valueSize);
            if (status != ERROR_SUCCESS || valueSize == 0) {
                LOG_DEBUG("ReadBinaryValue: AppId={} value={} unavailable status={} size={}",
                          appId, valueName, status, valueSize);
                return {};
            }
            if (valueSize > kMaxTicketRegistryBytes) {
                LOG_WARN("ReadBinaryValue: AppId={} value={} too large size={}",
                         appId, valueName, valueSize);
                return {};
            }

            std::vector<uint8_t> value(valueSize);
            status = RegGetValueA(
                HKEY_CURRENT_USER,
                regPath.c_str(),
                valueName,
                RRF_RT_REG_BINARY,
                &valueType,
                value.data(),
                &valueSize);
            if (status != ERROR_SUCCESS || valueType != REG_BINARY) {
                LOG_WARN("ReadBinaryValue: AppId={} value={} read failed status={} type={}",
                         appId, valueName, status, valueType);
                return {};
            }

            value.resize(valueSize);
            LOG_INFO("ReadBinaryValue: AppId={} value={} bytes={}", appId, valueName, value.size());
            return value;
        }

        bool WriteRegistryValue(AppId_t appId, const char* valueName, DWORD type,
                                const uint8_t* data, DWORD size) {
            HKEY hKey = nullptr;
            const std::string regPath = AppRegistryPath(appId);
            DWORD disposition = 0;
            LSTATUS status = RegCreateKeyExA(
                HKEY_CURRENT_USER,
                regPath.c_str(),
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE,
                nullptr,
                &hKey,
                &disposition);
            if (status != ERROR_SUCCESS) {
                LOG_ERROR("WriteRegistryValue: failed to open {} status={}", regPath, status);
                return false;
            }

            status = RegSetValueExA(hKey, valueName, 0, type, data, size);
            RegCloseKey(hKey);
            if (status != ERROR_SUCCESS) {
                LOG_ERROR("WriteRegistryValue: AppId={} value={} status={}", appId, valueName, status);
                return false;
            }
            return true;
        }

        std::string ReadRegistryString(HKEY root, const char* keyPath, const char* valueName) {
            DWORD valueType = 0;
            DWORD valueSize = 0;
            LSTATUS status = RegGetValueA(
                root, keyPath, valueName, RRF_RT_REG_SZ,
                &valueType, nullptr, &valueSize);
            if (status != ERROR_SUCCESS) {
                LOG_DEBUG("ReadRegistryString: {}\\{} missing status={}", keyPath, valueName, status);
                return {};
            }
            if (valueSize == 0 || valueType != REG_SZ) {
                LOG_DEBUG("ReadRegistryString: {}\\{} invalid size={} type={}",
                          keyPath, valueName, valueSize, valueType);
                return {};
            }

            std::vector<char> value(valueSize + 1, '\0');
            status = RegGetValueA(
                root, keyPath, valueName, RRF_RT_REG_SZ,
                &valueType, value.data(), &valueSize);
            if (status != ERROR_SUCCESS || valueType != REG_SZ) {
                LOG_WARN("ReadRegistryString: {}\\{} read failed status={} type={}",
                         keyPath, valueName, status, valueType);
                return {};
            }

            std::string result(value.data());
            while (!result.empty() && result.back() == '\0')
                result.pop_back();
            return result;
        }

        bool ParseDecimalU64(std::string_view text, uint64_t& out) {
            if (text.empty())
                return false;
            uint64_t value = 0;
            for (char c : text) {
                if (c < '0' || c > '9')
                    return false;
                uint64_t digit = static_cast<uint64_t>(c - '0');
                if (value > (UINT64_MAX - digit) / 10)
                    return false;
                value = value * 10 + digit;
            }
            if (value == 0)
                return false;
            out = value;
            return true;
        }

        uint64_t ComposeSteamID64(uint32_t accountId, uint32_t universe) {
            if (accountId == 0)
                return 0;
            if (universe == 0)
                universe = 1; // Public
            constexpr uint64_t kIndividual = 1;
            constexpr uint64_t kDesktopInstance = 1;
            return (static_cast<uint64_t>(universe) << 56) |
                   (kIndividual << 52) |
                   (kDesktopInstance << 32) |
                   static_cast<uint64_t>(accountId);
        }

        uint32_t ParseUniverseName(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (value == "public") return 1;
            if (value == "beta") return 2;
            if (value == "internal") return 3;
            if (value == "dev") return 4;
            return 1;
        }
    }

    static uint64_t GetSteamIDFromRegistryString(AppId_t appId) {
        const std::string regPath = AppRegistryPath(appId);
        const std::string steamIdStr = ReadRegistryString(HKEY_CURRENT_USER,
                                                          regPath.c_str(),
                                                          kSteamIdValue);
        uint64_t steamID = 0;
        if (!ParseDecimalU64(steamIdStr, steamID)) {
            return 0;
        }

        LOG_DEBUG("GetSpoofSteamID for AppId {}: SteamID REG_SZ -> 0x{:X}({})", appId, steamID, steamID);
        return steamID;
    }

    std::vector<uint8_t> GetAppOwnershipTicketFromRegistry(AppId_t appId) {
        LOG_INFO("GetAppOwnershipTicketFromRegistry: ENTER AppId={}", appId);
        if (!IsAppManagedByLua(appId, "GetAppOwnershipTicketFromRegistry")) return {};
        return ReadBinaryValue(appId, kAppTicketValue);
    }

    std::vector<uint8_t> GetEncryptedTicketFromRegistry(AppId_t appId) {
        LOG_INFO("GetEncryptedTicketFromRegistry: ENTER AppId={}", appId);
        if (!IsAppManagedByLua(appId, "GetEncryptedTicketFromRegistry")) return {};
        return ReadBinaryValue(appId, kEncryptedTicketValue);
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        if (!WriteRegistryValue(appId, kAppTicketValue, REG_BINARY,
                                data.data(), static_cast<DWORD>(data.size())))
            return false;
        LOG_INFO("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        if (!WriteRegistryValue(appId, kEncryptedTicketValue, REG_BINARY,
                                data.data(), static_cast<DWORD>(data.size())))
            return false;
        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteSteamID(AppId_t appId, uint64_t steamId) {
        if (appId == 0 || appId == k_uAppIdInvalid || steamId == 0)
            return false;
        const std::string value = std::to_string(steamId);
        if (!WriteRegistryValue(appId, kSteamIdValue, REG_SZ,
                                reinterpret_cast<const uint8_t*>(value.c_str()),
                                static_cast<DWORD>(value.size() + 1)))
            return false;
        LOG_INFO("Wrote SteamID for AppId {} ({})", appId, steamId);
        return true;
    }

    uint64_t GetSpoofSteamID(AppId_t appId) {
        // exclude those appids that are not in addappid
        if (!LuaLoader::HasDepot(appId)) {
            LOG_DEBUG("GetSpoofSteamID for AppId {}: not in addappid, skip spoofing", appId);
            return 0;
        }
        const uint64_t registrySteamID = GetSteamIDFromRegistryString(appId);
        if (registrySteamID != 0) {
            return registrySteamID;
        }

        // The SteamID baked into the cached AppOwnershipTicket is the same
        // one Steam itself uses for this app — pull it straight out of the
        // ticket so spoofed responses match what the DRM layer expects.
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        std::vector<uint8_t> ticket = GetAppOwnershipTicketFromRegistry(appId);
        if (ticket.size() >= 16) {
            const uint64_t steamID = reinterpret_cast<const uint64_t*>(ticket.data())[1];
            LOG_DEBUG("GetSpoofSteamID for AppId {}: -> 0x{:X}({})", appId, steamID, steamID);
            return steamID;
        }
        return 0;
    }

    // ════════════════════════════════════════════════════════════════
    //  Active SteamID lookup — used for fabricating tickets and for
    //  detecting "user switched accounts since the cached ticket was
    //  written" cases.
    //
    //  Lookup order:
    //   1. HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser (DWORD).
    //      Set by Steam at runtime, reset to 0 when Steam isn't running.
    //   2. Walk %SteamPath%\userdata\<accountid>\ folders. Steam keeps
    //      one folder per account that's ever logged in. If exactly one
    //      exists we use it; if multiple, we pick the most recently
    //      modified (best heuristic for "current user").
    // ════════════════════════════════════════════════════════════════
    uint64_t GetActiveSteamID64() {
        // 1. ActiveProcess\ActiveUser (live value while Steam is running)
        DWORD accountId = 0;
        DWORD size = sizeof(accountId);
        DWORD type = 0;
        LSTATUS s = RegGetValueA(
            HKEY_CURRENT_USER,
            "Software\\Valve\\Steam\\ActiveProcess",
            "ActiveUser",
            RRF_RT_REG_DWORD,
            &type,
            &accountId,
            &size);
        if (s == ERROR_SUCCESS && size == sizeof(accountId) && type == REG_DWORD && accountId != 0) {
            std::string universe = ReadRegistryString(
                HKEY_CURRENT_USER,
                "Software\\Valve\\Steam\\ActiveProcess",
                "Universe");
            const uint64_t steamID64 = ComposeSteamID64(accountId, ParseUniverseName(universe));
            LOG_DEBUG("GetActiveSteamID64: ActiveProcess\\ActiveUser={} universe={} -> SteamID64=0x{:X}",
                      accountId, universe.empty() ? "Public" : universe, steamID64);
            return steamID64;
        }
        if (s == ERROR_SUCCESS && (size != sizeof(accountId) || type != REG_DWORD)) {
            LOG_WARN("GetActiveSteamID64: ActiveUser invalid size={} type={}", size, type);
        }

        // 2. Filesystem fallback — pick the most recently modified
        //    userdata\<accountid>\ folder. This survives Steam being
        //    closed at the moment we query.
        DWORD pathLen = MAX_PATH;
        char steamPath[MAX_PATH] = {};
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
                         RRF_RT_REG_SZ, nullptr, steamPath, &pathLen) != ERROR_SUCCESS) {
            LOG_DEBUG("GetActiveSteamID64: no ActiveUser, no SteamPath — give up");
            return 0;
        }

        char userdataPath[MAX_PATH];
        std::snprintf(userdataPath, MAX_PATH, "%s\\userdata", steamPath);

        char searchPattern[MAX_PATH];
        std::snprintf(searchPattern, MAX_PATH, "%s\\*", userdataPath);

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            LOG_DEBUG("GetActiveSteamID64: no userdata folder at {}", userdataPath);
            return 0;
        }

        DWORD bestAccountId = 0;
        FILETIME bestMtime = {};
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;

            char* end = nullptr;
            unsigned long aid = strtoul(fd.cFileName, &end, 10);
            if (!end || *end != '\0' || aid == 0) continue;

            // Pick the most recently written folder.
            if (bestAccountId == 0
                || CompareFileTime(&fd.ftLastWriteTime, &bestMtime) > 0) {
                bestAccountId = static_cast<DWORD>(aid);
                bestMtime = fd.ftLastWriteTime;
            }
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);

        if (bestAccountId == 0) {
            LOG_DEBUG("GetActiveSteamID64: no userdata\\<accountid>\\ folders found");
            return 0;
        }

        const uint64_t steamID64 = ComposeSteamID64(bestAccountId, 1);
        LOG_DEBUG("GetActiveSteamID64: userdata\\{}\\ -> SteamID64=0x{:X} (filesystem fallback)",
                  bestAccountId, steamID64);
        return steamID64;
    }

    // ════════════════════════════════════════════════════════════════
    //  Known Steam DRM (Steam Stub) appid table.
    //
    //  This is a hand-curated, deliberately-small list. We only flag
    //  titles where we have direct evidence of error-54 reports against
    //  LumaCore. The list is not security-sensitive — it only changes
    //  the wording of the diagnostic log line so users get a "try
    //  Steamless" hint instead of generic "ownership patched" output.
    // ════════════════════════════════════════════════════════════════
    bool IsKnownSteamDrmApp(AppId_t appId) {
        switch (appId) {
        case 1167630:  // Teardown
        case 782330:   // DOOM Eternal
        case 17390:    // Spore (legacy v2 wrapper)
        case 21660:    // Mirror's Edge (legacy v1.5 wrapper)
            return true;
        default:
            return false;
        }
    }

    // ════════════════════════════════════════════════════════════════
    //  Build a minimal AppTicket-shaped blob.
    //
    //  Layout matches what Steam's wrapper writes into the registry:
    //    [uint32 sigOffset]
    //    [uint32 version=4]
    //    [uint64 steamID]
    //    [uint32 appId]
    //    [uint32 ticketGenerated (Unix epoch)]
    //    [uint32 ticketExpires]
    //    [uint32 licenseFlags]
    //    [uint32 licenseCount=0]    // empty license list
    //    [uint32 dlcCount=0]        // empty DLC list
    //    [uint16 reserved=0]
    //    [128 bytes of zeros]       // signature placeholder
    //
    //  This is unsigned. Steam Stub v2.2+ verifies the signature against
    //  Valve's public key so this blob alone does NOT bypass error 54
    //  on modern Steam DRM titles. It does help older v1.5 / early v2
    //  wrappers and tools that only inspect the SteamID/AppID fields.
    //  Steamless on the .exe is the actual fix for v3 titles like
    //  Teardown — this is just a best-effort fallback.
    // ════════════════════════════════════════════════════════════════
    std::vector<uint8_t> BuildMinimalAppTicket(AppId_t appId) {
        const uint64_t steamID = GetActiveSteamID64();
        if (steamID == 0) {
            LOG_DEBUG("BuildMinimalAppTicket: AppId={} no active SteamID — skip", appId);
            return {};
        }

        // Header before signature: 4 + 4 + 8 + 4 + 4 + 4 + 4 + 4 + 4 + 2 = 42 bytes.
        constexpr size_t kHeaderBytes = 42;
        constexpr size_t kSignatureBytes = 128;
        const size_t total = kHeaderBytes + kSignatureBytes;

        std::vector<uint8_t> blob(total, 0);
        uint8_t* p = blob.data();

        const uint32_t sigOffset = static_cast<uint32_t>(kHeaderBytes);
        std::memcpy(p +  0, &sigOffset, 4);
        const uint32_t version = 4;
        std::memcpy(p +  4, &version, 4);
        std::memcpy(p +  8, &steamID, 8);
        std::memcpy(p + 16, &appId, 4);
        const uint32_t now = static_cast<uint32_t>(time(nullptr));
        std::memcpy(p + 20, &now, 4);
        const uint32_t expires = now + (60u * 60u * 24u * 30u);  // +30 days
        std::memcpy(p + 24, &expires, 4);
        // licenseFlags=0, licenseCount=0, dlcCount=0, reserved=0 are already zero-init.

        LOG_INFO("BuildMinimalAppTicket: AppId={} steamID=0x{:X} -> {} bytes (unsigned)",
                 appId, steamID, total);
        return blob;
    }

    // ════════════════════════════════════════════════════════════════
    //  EnsureRegistryTicketsForApp
    //
    //  Called from the SpawnProcess VEH right before a configured-appid
    //  game is allowed to launch. Two responsibilities:
    //
    //  1. If a cached AppTicket exists but its embedded SteamID does NOT
    //     match the currently-logged-in Steam user, wipe it. Otherwise
    //     the wrapper would compare the stale SteamID against the new
    //     user and fail.
    //
    //  2. If no cached AppTicket is present, write a fabricated minimal
    //     blob baked with the active user's SteamID. Helps older Steam
    //     Stub wrappers; harmless for v3 (those still need Steamless).
    //
    //  Same logic for ETicket.
    //
    //  Returns true if any write happened.
    // ════════════════════════════════════════════════════════════════
    bool EnsureRegistryTicketsForApp(AppId_t appId) {
        const uint64_t activeID = GetActiveSteamID64();
        if (activeID == 0) {
            LOG_INFO("EnsureRegistryTicketsForApp: AppId={} no active user — skip", appId);
            return false;
        }

        bool wrote = false;

        if (GetSteamIDFromRegistryString(appId) != activeID) {
            wrote = WriteSteamID(appId, activeID) || wrote;
        }

        // ── AppTicket ──
        std::vector<uint8_t> existing = GetAppOwnershipTicketFromRegistry(appId);
        if (!existing.empty() && existing.size() >= 16) {
            // Layout: [u32 sigOffset][u32 version][u64 steamID][...]
            const uint64_t cachedID = reinterpret_cast<const uint64_t*>(existing.data())[1];
            if (cachedID != activeID) {
                LOG_INFO("EnsureRegistryTicketsForApp: AppId={} cached SteamID 0x{:X} != active 0x{:X}, wiping",
                         appId, cachedID, activeID);
                HKEY hKey;
                const std::string regPath = "Software\\Valve\\Steam\\Apps\\" + std::to_string(appId);
                if (RegOpenKeyExA(HKEY_CURRENT_USER, regPath.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
                    RegDeleteValueA(hKey, "AppTicket");
                    RegCloseKey(hKey);
                }
                existing.clear();
            }
        }

        if (existing.empty()) {
            std::vector<uint8_t> blob = BuildMinimalAppTicket(appId);
            if (!blob.empty()) {
                if (WriteAppOwnershipTicket(appId, blob)) {
                    LOG_INFO("EnsureRegistryTicketsForApp: AppId={} wrote fabricated AppTicket ({} bytes)",
                             appId, blob.size());
                    wrote = true;
                    if (IsKnownSteamDrmApp(appId)) {
                        LOG_INFO("EnsureRegistryTicketsForApp: AppId={} is a known Steam-DRM title — "
                                 "the fabricated ticket is unsigned and will likely be rejected by the "
                                 "wrapper's signature check (error 54). Use Steamless from SteaMidra "
                                 "to strip the wrapper if launch fails.",
                                 appId);
                    }
                }
            }
        }

        return wrote;
    }

    constexpr AppId_t kLocalAppTicketSourceAppId = 7;

    std::vector<uint8_t> ForgeAppTicket(AppId_t sourceAppId, AppId_t targetAppId) {
        // Uses DecryptionKeyHook to get the cached app ticket from the local config store.
        // The source ticket must be from a known-signed app (app 7 is the canonical source).
        (void)sourceAppId;
        std::vector<uint8_t> source = DecryptionKeyHook::GetCachedAppTicket(kLocalAppTicketSourceAppId);
        if (source.empty()) {
            LOG_DEBUG("ForgeAppTicket for AppId {}: no cached ticket source", targetAppId);
            return {};
        }

        if (source.size() <= kAppTicketSignatureSize) {
            LOG_DEBUG("ForgeAppTicket for AppId {}: source ticket too small ({} bytes)", targetAppId, source.size());
            return {};
        }

        const size_t bodyLen = source.size() - 128;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        std::copy_n(source.begin(), bodyLen, std::back_inserter(ticket));
        const uint8_t* idBytes = reinterpret_cast<const uint8_t*>(&targetAppId);
        std::copy_n(idBytes, sizeof(AppId_t), std::back_inserter(ticket));
        std::copy(source.begin() + bodyLen, source.end(), std::back_inserter(ticket));

        LOG_INFO("Forged App Ownership Ticket, AppId: {}, SourceAppId: {}, Physical Size: {}, Source Size: {}",
                 targetAppId, sourceAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket) {
        ticket = {};

        ticket.data = GetAppOwnershipTicketFromRegistry(appId);
        if (!ticket.data.empty() && ticket.data.size() >= sizeof(uint32_t)) {
            ticket.totalSize      = static_cast<uint32>(ticket.data.size());
            ticket.appIdOffset    = kAppTicketAppIdOffset;
            ticket.steamIdOffset  = kAppTicketSteamIdOffset;
            ticket.signatureOffset = *reinterpret_cast<const uint32*>(ticket.data.data());
            ticket.signatureSize  = kAppTicketSignatureSize;
            return true;
        }

        ticket.data = ForgeAppTicket(kLocalAppTicketSourceAppId, appId);
        if (ticket.data.empty()) {
            LOG_DEBUG("GetAppOwnershipTicket: AppId={} forge failed, no ticket available", appId);
            return false;
        }

        ticket.totalSize      = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset    = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset  = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize  = kAppTicketSignatureSize;
        return true;
    }
}

