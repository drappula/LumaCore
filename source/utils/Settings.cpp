// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "Settings.h"
#include "Logger.h"
#include <toml++/toml.hpp>
#include <filesystem>
#include <string_view>

namespace Settings {

    // Lookup table: TOML string → LogLevel enum.
    // Returns the current logLevel unchanged if the input string isn't recognised.
    static LogLevel ParseLogLevel(std::string_view s)
    {
        static const struct { std::string_view name; LogLevel lvl; } kLevels[] = {
            { "trace", LogLevel::Trace },
            { "debug", LogLevel::Debug },
            { "info",  LogLevel::Info  },
            { "warn",  LogLevel::Warn  },
            { "error", LogLevel::Error },
        };
        for (const auto& entry : kLevels)
            if (entry.name == s) return entry.lvl;
        return logLevel;
    }

    static const char* LevelName(LogLevel lvl)
    {
        switch (lvl) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        default:              return "unknown";
        }
    }

    void Load(const std::string& configPath)
    {
        std::filesystem::path cfgPath(configPath);
        logDir = (cfgPath.parent_path() / "lumacore").string();

        if (!std::filesystem::exists(cfgPath)) {
            LOG_INFO("Settings: config not found at '{}', using defaults", configPath);
            return;
        }

        try {
            auto tbl = toml::parse_file(configPath);

            // [log]
            if (auto logTbl = tbl["log"].as_table()) {
                if (auto lvl = (*logTbl)["level"].value<std::string>())
                    logLevel = ParseLogLevel(*lvl);
                if (auto v = (*logTbl)["verbose"].value<bool>())
                    verbose = *v;
            }

            // [lua]
            if (auto luaTbl = tbl["lua"].as_table()) {
                if (auto arr = (*luaTbl)["paths"].as_array()) {
                    for (const auto& elem : *arr) {
                        if (auto s = elem.value<std::string>())
                            luaPaths.push_back(*s);
                    }
                }
            }

            // [pattern_fetch]
            if (auto patternTbl = tbl["pattern_fetch"].as_table()) {
                if (auto m = (*patternTbl)["mirror"].value<std::string>())
                    patternMirror = *m;
                if (auto g = (*patternTbl)["gitflic_enabled"].value<bool>())
                    patternGitflicEnabled = *g;
            }

            // [manifest_fetch]
            // - urls = [...] takes priority and replaces the default chain
            // - url = "..." overrides the chain with a single endpoint
            //   (back-compat with the 6.2.x single-URL config)
            // - neither = the 3-default chain in Settings.h stays
            if (auto mfetch = tbl["manifest_fetch"].as_table()) {
                if (auto arr = (*mfetch)["urls"].as_array()) {
                    std::vector<std::string> chain;
                    chain.reserve(arr->size());
                    for (const auto& elem : *arr) {
                        if (auto s = elem.value<std::string>())
                            chain.push_back(*s);
                    }
                    if (!chain.empty())
                        manifestFetchUrls = std::move(chain);
                } else if (auto u = (*mfetch)["url"].value<std::string>()) {
                    manifestFetchUrls = { *u };
                }
                if (auto t = (*mfetch)["timeout_sec"].value<int64_t>())
                    manifestFetchTimeoutSec = static_cast<int>(*t);
            }

            std::string urlsLog;
            for (const auto& u : manifestFetchUrls) {
                if (!urlsLog.empty()) urlsLog += " | ";
                urlsLog += u;
            }
            if (urlsLog.empty()) urlsLog = "<disabled>";

            LOG_INFO("Settings: log.level={} log.verbose={} lua.paths_count={} "
                     "pattern_fetch.mirror={} manifest_fetch.urls=[{}] "
                     "manifest_fetch.timeout_sec={}",
                     LevelName(logLevel), verbose ? "true" : "false",
                     static_cast<uint32_t>(luaPaths.size()),
                     patternMirror.empty() ? "<none>" : patternMirror,
                     urlsLog,
                     manifestFetchTimeoutSec);

        } catch (const toml::parse_error& e) {
            LOG_WARN("Settings: TOML parse error: {}", e.what());
        } catch (...) {
            LOG_WARN("Settings: load failed, using defaults");
        }
    }

}
