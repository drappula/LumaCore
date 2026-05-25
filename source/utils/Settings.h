// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace Settings {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    void Load(const std::string& configPath);

    // [log]
    inline LogLevel logLevel = LogLevel::Debug;

    // When true, every per-module logger is forced to Trace at startup so
    // we get the most detailed possible log of every IPC, network packet,
    // and hook call. Useful for diagnosing launch failures (Steam error 54
    // and similar). Defaults on so users do not need to touch config.toml
    // before sending logs.
    inline bool verbose = true;

    // derived from configPath: <steam>/lumacore/
    inline std::string logDir;

    // [lua]
    inline std::vector<std::string> luaPaths;

}
