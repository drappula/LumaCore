// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "TicketProvider.h"
#include "CredentialStore.h"
#include "config/LuaLoader.h"
#include "hooks/client/DecryptionKeyHook.h"
#include "Logger.h"

namespace AppTicket {

    constexpr AppId_t kForgeSourceAppId = 7;
    constexpr size_t kMinTicketSize = 16;

    std::vector<uint8_t> ReadTicketFromStore(AppId_t appId) {
        if (!LuaLoader::HasDepot(appId)) {
            LOG_DEBUG("AppTicket::ReadTicketFromStore: AppId={} not tracked", appId);
            return {};
        }
        std::vector<uint8_t> out;
        auto st = CredentialStore::ReadTicket(appId, out);
        if (st != CredentialStore::Status::Ok) {
            LOG_TRACE("AppTicket::ReadTicketFromStore: AppId={} status={}", appId, CredentialStore::ToString(st));
            return {};
        }
        LOG_INFO("AppTicket::ReadTicketFromStore: AppId={} bytes={}", appId, out.size());
        return out;
    }

    std::vector<uint8_t> ReadETicketFromStore(AppId_t appId) {
        if (!LuaLoader::HasDepot(appId)) {
            LOG_DEBUG("AppTicket::ReadETicketFromStore: AppId={} not tracked", appId);
            return {};
        }
        std::vector<uint8_t> out;
        auto st = CredentialStore::ReadETicket(appId, out);
        if (st != CredentialStore::Status::Ok) {
            LOG_TRACE("AppTicket::ReadETicketFromStore: AppId={} status={}", appId, CredentialStore::ToString(st));
            return {};
        }
        LOG_INFO("AppTicket::ReadETicketFromStore: AppId={} bytes={}", appId, out.size());
        return out;
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability: locate any
    // signed AppTicket in the Windows registry, clone the signed portion,
    // inject the target AppId before the signature block, and append the
    // original signature. steamdrmp reads the AppId from the cloned ticket
    // by overflowing its four-byte parse buffer.
    static std::vector<uint8_t> ForgeFromSource(AppId_t sourceAppId, AppId_t targetAppId) {
        (void)sourceAppId;

        std::vector<uint8_t> source;
        {
            HKEY hApps = nullptr;
            if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\Apps", 0,
                              KEY_READ, &hApps) == ERROR_SUCCESS) {
                char subName[32];
                DWORD idx = 0;
                while (true) {
                    DWORD nameLen = sizeof(subName);
                    if (RegEnumKeyExA(hApps, idx++, subName, &nameLen,
                                      nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                        break;
                    char* end = nullptr;
                    unsigned long sid = strtoul(subName, &end, 10);
                    if (!end || *end != '\0' || sid == 0) continue;
                    if (static_cast<AppId_t>(sid) == targetAppId) continue;
                    source = DecryptionKeyHook::GetCachedAppTicket(static_cast<AppId_t>(sid));
                    if (!source.empty() && source.size() > 128) break;
                    source.clear();
                }
                RegCloseKey(hApps);
            }
        }
        if (source.size() <= 128) {
            LOG_DEBUG("AppTicket::ForgeFromSource: target={} no signed source in registry", targetAppId);
            return {};
        }

        // clone the signed portion, inject target app id before the sig
        const size_t bodyLen = source.size() - 128;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        std::copy_n(source.begin(), bodyLen, std::back_inserter(ticket));
        const uint8_t* idBytes = reinterpret_cast<const uint8_t*>(&targetAppId);
        std::copy_n(idBytes, sizeof(AppId_t), std::back_inserter(ticket));
        std::copy(source.begin() + bodyLen, source.end(), std::back_inserter(ticket));

        LOG_INFO("AppTicket::ForgeFromSource: target={} source_app=7 physical={} signed_part={}",
                 targetAppId, ticket.size(), source.size());
        return ticket;
    }

    std::vector<uint8_t> ForgeFromApp7(AppId_t appId) {
        return ForgeFromSource(kForgeSourceAppId, appId);
    }

    bool GetTicket(AppId_t appId, OwnershipTicket& out, Source src) {
        out = {};

        if (src == Source::CredentialOnly || src == Source::CredentialThenForge) {
            out.data = ReadTicketFromStore(appId);
            if (!out.data.empty() && out.data.size() >= sizeof(uint32_t)) {
                out.totalSize      = static_cast<uint32>(out.data.size());
                out.appIdOffset    = 16;
                out.steamIdOffset  = 8;
                out.signatureOffset = *reinterpret_cast<const uint32*>(out.data.data());
                out.signatureSize  = 128;
                return true;
            }
        }

        if (src == Source::CredentialOnly) return false;

        out.data = ForgeFromApp7(appId);
        if (out.data.empty()) return false;

        // set offsets in reverse — signature first, then appid, then total
        out.signatureSize  = 128;
        out.signatureOffset = (static_cast<uint32>(out.data.size()) - sizeof(AppId_t)) - 128 + sizeof(AppId_t);
        out.appIdOffset    = out.signatureOffset - sizeof(AppId_t);
        out.steamIdOffset  = 8;
        out.totalSize      = static_cast<uint32>(out.data.size()) - sizeof(AppId_t);
        return true;
    }

    uint64_t GetSpoofSteamID(AppId_t appId) {
        if (!LuaLoader::HasDepot(appId)) {
            LOG_DEBUG("AppTicket::GetSpoofSteamID: AppId={} not tracked", appId);
            return 0;
        }

        uint64_t steamId = 0;
        auto st = CredentialStore::ReadSteamId(appId, steamId);
        if (st == CredentialStore::Status::Ok && steamId != 0) {
            LOG_DEBUG("AppTicket::GetSpoofSteamID: AppId={} -> 0x{:X}", appId, steamId);
            return steamId;
        }

        // fall back to parsing the SteamID from the cached ticket
        std::vector<uint8_t> ticket = ReadTicketFromStore(appId);
        if (ticket.size() >= kMinTicketSize) {
            const uint64_t parsed = reinterpret_cast<const uint64_t*>(ticket.data())[1];
            LOG_DEBUG("AppTicket::GetSpoofSteamID: AppId={} ticket-parse -> 0x{:X}", appId, parsed);
            return parsed;
        }
        return 0;
    }

    bool WriteTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        auto st = CredentialStore::WriteTicket(appId, data);
        return st == CredentialStore::Status::Ok;
    }

    bool WriteETicket(AppId_t appId, const std::vector<uint8_t>& data) {
        auto st = CredentialStore::WriteETicket(appId, data);
        return st == CredentialStore::Status::Ok;
    }

    bool WriteSteamID(AppId_t appId, uint64_t steamId) {
        auto st = CredentialStore::WriteSteamId(appId, steamId);
        return st == CredentialStore::Status::Ok;
    }

}
