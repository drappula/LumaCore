// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "HookStatus.h"

#include "Logger.h"
#include "../entry.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace HookStatus {

    namespace {

        std::mutex g_mu;

        std::string g_buildId;
        std::string g_steamclientSha;
        std::string g_steamuiSha;
        bool        g_steamclientToml = false;
        bool        g_steamuiToml     = false;
        std::uint64_t            g_installed = 0;
        std::vector<std::string> g_missed;
        bool        g_initDone        = false;

        // Conservative escaper for JSON string literals. The values we emit are
        // ASCII function names, hex SHAs, and decimal build ids, so anything
        // outside printable ASCII falls through to \uXXXX.
        std::string JsonEscape(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char ch : s) {
                unsigned char c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20 || c > 0x7E) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                            out += buf;
                        } else {
                            out += static_cast<char>(c);
                        }
                        break;
                }
            }
            return out;
        }

        // Caller already owns g_mu.
        std::string SerializeLocked() {
            std::string out;
            out.reserve(256 + g_missed.size() * 32);
            out += "{\n";
            out += "  \"build_id\": \"";
            out += JsonEscape(g_buildId);
            out += "\",\n";
            out += "  \"toml_found\": {\n";
            out += "    \"steamclient\": ";
            out += g_steamclientToml ? "true" : "false";
            out += ",\n";
            out += "    \"steamui\": ";
            out += g_steamuiToml ? "true" : "false";
            out += "\n  },\n";
            out += "  \"hooks_installed\": ";
            out += std::to_string(g_installed);
            out += ",\n";
            out += "  \"hooks_missed\": [";
            for (size_t i = 0; i < g_missed.size(); ++i) {
                if (i) out += ", ";
                out += "\"";
                out += JsonEscape(g_missed[i]);
                out += "\"";
            }
            out += "],\n";
            out += "  \"steamclient_sha\": \"";
            out += JsonEscape(g_steamclientSha);
            out += "\",\n";
            out += "  \"steamui_sha\": \"";
            out += JsonEscape(g_steamuiSha);
            out += "\"\n";
            out += "}\n";
            return out;
        }

        bool WriteBodyAtomic(const std::string& body) {
            if (!SteamInstallPath[0]) {
                LOG_WARN("HookStatus: SteamInstallPath unset, skipping write");
                return false;
            }
            std::filesystem::path dir = std::filesystem::path(SteamInstallPath) / "lumacore";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                LOG_WARN("HookStatus: create_directories failed: {}", ec.message());
                return false;
            }

            std::filesystem::path target = dir / "status.json";
            std::filesystem::path tmp    = target;
            tmp += ".tmp";

            std::string narrowTmp    = tmp.string();
            std::string narrowTarget = target.string();

            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    LOG_WARN("HookStatus: open tmp failed for {}", narrowTarget);
                    DeleteFileA(narrowTmp.c_str());
                    return false;
                }
                f.write(body.data(), static_cast<std::streamsize>(body.size()));
                f.flush();
                if (!f) {
                    LOG_WARN("HookStatus: write tmp failed for {}", narrowTarget);
                    f.close();
                    DeleteFileA(narrowTmp.c_str());
                    return false;
                }
            }

            if (!MoveFileExA(narrowTmp.c_str(), narrowTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
                DWORD err = GetLastError();
                LOG_WARN("HookStatus: MoveFileExA failed err={} for {}",
                         err, narrowTarget);
                DeleteFileA(narrowTmp.c_str());
                return false;
            }
            return true;
        }

        // Called from any mutator while holding g_mu. Re-publishes the file
        // only after the first explicit WriteToDisk has flipped g_initDone.
        void MaybeRepublishLocked() {
            if (!g_initDone) return;
            std::string body = SerializeLocked();
            (void)WriteBodyAtomic(body);
        }

    }  // namespace

    void SetBuildId(std::string buildId) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buildId = std::move(buildId);
    }

    void SetTomlAvailability(std::string_view moduleName, bool found) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (moduleName == "steamclient") {
            g_steamclientToml = found;
        } else if (moduleName == "steamui") {
            g_steamuiToml = found;
        } else {
            LOG_WARN("HookStatus: unknown module '{}' in SetTomlAvailability",
                     std::string(moduleName));
            return;
        }
    }

    void SetShas(std::string steamclientSha, std::string steamuiSha) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_steamclientSha = std::move(steamclientSha);
        g_steamuiSha     = std::move(steamuiSha);
    }

    void RecordInstalled() {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_installed;
    }

    void RecordMissed(std::string hookName) {
        if (hookName.empty()) return;
        std::lock_guard<std::mutex> lk(g_mu);
        g_missed.push_back(std::move(hookName));
    }

    void WriteToDisk() {
        std::string body;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            body = SerializeLocked();
            g_initDone = true;
        }
        try {
            (void)WriteBodyAtomic(body);
        } catch (const std::exception& e) {
            LOG_WARN("HookStatus: write threw '{}'", e.what());
        } catch (...) {
            LOG_WARN("HookStatus: write threw unknown");
        }
    }

}  // namespace HookStatus
