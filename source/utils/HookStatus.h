// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once

// Tracks which hook installers landed and which couldn't resolve their target
// through the runtime TOML. The result lands in <Steam>\lumacore\status.json
// so SteaMidra can surface a banner when the running Steam build doesn't have
// a pattern emitted yet.
//
// Threading: every public function takes the same internal mutex, so call
// sites don't have to coordinate. Mutator calls made after init completes
// (signalled by the first WriteToDisk) re-publish the file in place so the
// banner reflects the latest counts.
//
// Schema produced by WriteToDisk (top-level keys only, exact set):
//   build_id            string
//   toml_found          object with exactly steamclient and steamui booleans
//   hooks_installed     non-negative integer (count of RecordInstalled calls)
//   hooks_missed        array of strings (names from RecordMissed)
//   steamclient_sha     string (empty when unknown)
//   steamui_sha         string (empty when unknown)

#include <string>
#include <string_view>

namespace HookStatus {

    void SetBuildId(std::string buildId);

    // Module names accepted: "steamclient" and "steamui". Anything else is
    // ignored with a warning log line.
    void SetTomlAvailability(std::string_view moduleName, bool found);

    void SetShas(std::string steamclientSha, std::string steamuiSha);

    void RecordInstalled();
    void RecordMissed(std::string hookName);

    // Writes the current snapshot to <Steam>\lumacore\status.json via a
    // tmp + MoveFileExA(MOVEFILE_REPLACE_EXISTING) swap. Best-effort: failures
    // log a warning and never throw. The first successful or attempted write
    // marks init as complete, after which every mutator re-publishes.
    void WriteToDisk();

}  // namespace HookStatus

