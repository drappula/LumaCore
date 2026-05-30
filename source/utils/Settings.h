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

    // [pattern_fetch] mirror
    // Optional URL template for the pattern repo, with {subdir} and {sha}
    // placeholders. Empty string means "no override; go straight to the
    // GitHub primary -> jsDelivr -> gitflic -> local cache fallback chain".
    inline std::string patternMirror;

    // [pattern_fetch] gitflic_enabled
    // gitflic.ru fallback for users in regions where github + jsdelivr are
    // blocked or rate-limited (RU primarily). Sits after the github + cdn
    // legs and before the local cache. Set to false to skip it entirely.
    inline bool patternGitflicEnabled = true;

    // [manifest_fetch]
    // URL templates the wire-level GetManifestRequestCode bridge hits when
    // Steam asks for a manifest gid we have a depot binding for but the
    // server returned eresult != OK. Placeholders: {gid}, {appid}, {depotid}.
    // The body is parsed as either a plain decimal uint64 OR a JSON object
    // with a "content" digit-string field, so wudrm-style and steam.run-style
    // endpoints both work as is.
    //
    // The list is tried in order, first one that gives back a parseable code
    // wins. An empty list (e.g. user set [manifest_fetch] urls = []) disables
    // the bridge and lets the original eresult fall through.
    //
    // Single-string [manifest_fetch] url = "..." is honoured for back-compat
    // by Settings::Load: when present it OVERRIDES the chain (single-URL mode).
    // [manifest_fetch] urls = [...] takes precedence over the single form.
    inline std::vector<std::string> manifestFetchUrls = {
        "https://manifest.opensteamtool.com/{gid}",
        "http://gmrc.wudrm.com/manifest/{gid}",
        "https://manifest.steam.run/api/manifest/{gid}",
    };

    // Total wall clock the recv handler waits on the HTTP future before
    // giving up and letting the original eresult fall through. Applied
    // per-provider; the chain stops as soon as one returns a code, so a
    // healthy first provider doesn't pay the budget of the slow ones.
    inline int manifestFetchTimeoutSec = 12;

}
