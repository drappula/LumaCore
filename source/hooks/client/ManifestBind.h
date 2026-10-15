// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once
#include <cstdint>
#include "Steam/Types.h"

namespace ManifestBind {
    void Install();
    void Uninstall();

    // depotId -> (appId, Steam's own manifest gid) recorded from
    // BuildDepotDependency output before pin overrides are applied.
    // Final home: consumed by NetPacket_Manifest (Task 6). Returns false
    // when the depot has not been seen this session.
    bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid);

    // Once-per-(depot,gid)-per-session warning for manifests the archive
    // reports as not archived yet (404). Final home is here in
    // ManifestBind (implemented, not a stub); Task 6 calls it from the
    // request-code fallback path. Safe to call from any thread.
    void ReportMissingManifest(uint32_t depotId, uint64_t gid);
}
