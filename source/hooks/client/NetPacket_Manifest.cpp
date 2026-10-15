// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "hooks/client/NetPacket.h"
#include "hooks/client/ManifestBind.h"
#include "config/LuaLoader.h"
#include "runtime/ManifestCache.h"
#include "runtime/ManifestDonor.h"
#include "runtime/ManifestFetch.h"
#include "runtime/Logger.h"

#include <thread>

namespace NetPacket::Handlers::DepotFallback {

bool HandleSend(const uint8_t* pBody, uint32_t cbBody,
                const uint8_t* pHdr, uint32_t cbHdr) {
    CContentServerDirectory_GetManifestRequestCode_Request req;
    if (!req.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"err\":\"parse-fail\",\"size\":{}}}", cbBody);
        return false;
    }
    if (!req.has_depot_id() || !req.has_manifest_id()) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"skip\":\"no-depot-or-manifest\"}}}}");
        return false;
    }
    const AppId_t depotId = req.depot_id();
    const uint64_t gid    = req.manifest_id();
    const AppId_t appId   = req.has_app_id() ? req.app_id() : 0;

    CMsgProtoBufHeader hdr;
    if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_jobid_source()) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"err\":\"no-jobid\"}}}}");
        return false;
    }
    const uint64_t jobId = hdr.jobid_source();

    // Donor passive capture: remember EVERY outgoing code request while
    // enabled — not just Lua depots, genuine codes from actually-downloaded
    // depots are the valuable ones. Placed before the HasDepot and
    // owned-bypass early-returns below so those paths still record (the
    // bypass path receives a genuine code worth donating). No-op when
    // [donate] is disabled.
    ManifestDonor::NoteSent(jobId, depotId, gid);

    if (!LuaLoader::HasDepot(depotId)) {
        LOG_PKTRT_DEBUG("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"skip\":\"not-in-addappid\",\"depot\":{},\"gid\":{}}}",
                   depotId, gid);
        return false;
    }

    // Owned-depot bypass: BuildDepotDependency recorded what Steam itself
    // believes for this depot. When it belongs to an app the account owns,
    // Steam's genuine request code works — skip the fetch injection and
    // leave Steam's own code alone.
    AppId_t seenApp = 0;
    uint64_t seenGid = 0;
    if (ManifestBind::LookupDepot(depotId, seenApp, seenGid) &&
        seenApp && LuaLoader::IsOwned(seenApp)) {
        LOG_PKTRT_INFO("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"bypass\":\"owned\",\"depot\":{},\"app\":{},\"msg\":\"belongs to owned app {}, leaving Steam's own code alone\"}}",
                   depotId, seenApp, seenApp);
        return false;
    }

    LOG_PKTRT_INFO("{{\"evt\":\"DepotFallback\",\"act\":\"send\",\"depot\":{},\"gid\":{},\"app\":{},\"job\":{}}}",
               depotId, gid, appId, jobId);

    // Pre-seed <Steam>\depotcache from the archive for the EXACT manifest
    // Steam is asking a code for. Detached + best-effort: this runs on a
    // Steam net thread, so it must never block. v1 uses bypass=false
    // always — no download-activity signal exists yet, so scheduled-update
    // retries ride the negative cache instead of hammering the archive.
    {
        const AppId_t  preApp   = appId;
        const uint32_t preDepot = depotId;
        const uint64_t preGid   = gid;
        std::thread([preApp, preDepot, preGid]() {
            bool notArchived = false;
            const bool ok = ManifestCache::EnsureCached(
                preApp, preDepot, preGid, 0, &notArchived, false);
            if (!ok && notArchived)
                ManifestBind::ReportMissingManifest(preDepot, preGid);
        }).detach();
    }

    ManifestFetch::Submit(jobId, gid, appId, depotId);
    return false;
}

void HandleRecv(const uint8_t* pHdr, uint32_t cbHdr,
                const uint8_t* pBody, uint32_t cbBody) {
    CMsgProtoBufHeader hdr;
    if (!hdr.ParseFromArray(pHdr, cbHdr)) {
        LOG_PKTRT_WARN("{{{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"err\":\"header-parse-fail\"}}}}");
        return;
    }
    if (!hdr.has_jobid_target()) {
        LOG_PKTRT_DEBUG("{{{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"skip\":\"no-jobid\"}}}}");
        return;
    }
    const uint64_t jobId = hdr.jobid_target();

    // Active-probe replies (Task 8) are consumed here first: originated
    // frames bypass the TX hook, so HandleSend never saw them and neither
    // the donor-capture below nor ManifestFetch has an entry for them.
    // The explicit IsProbeJobId guard after that covers even untracked
    // (stale, timed-out) probe-range jobids, so no originated reply can
    // ever be mistaken for a real Steam reply. Real Steam jobids count up
    // from small values and can never reach the probe range.
    if (ManifestDonor::HandleProbeRecv(pBody, cbBody, pHdr, cbHdr))
        return;
    if (ManifestDonor::IsProbeJobId(jobId)) {
        LOG_PKTRT_DEBUG("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"skip\":\"stale-probe-job\",\"job\":{}}}",
                   jobId);
        return;
    }

    // Donor passive capture: harvest the genuine code from the UNPATCHED
    // reply before the injection path below can rewrite the body. Only
    // real successes for a request tracked by NoteSent on send; the
    // injected (non-owned) case filters out here because Steam's own reply
    // carries eresult != OK or no valid code. Runs before Resolve so the
    // early-return no-resolve path still donates. No-op when [donate] is
    // disabled.
    if (hdr.eresult() == static_cast<int32_t>(k_EResultOK)) {
        CContentServerDirectory_GetManifestRequestCode_Response donorResp;
        if (donorResp.ParseFromArray(pBody, cbBody) &&
            donorResp.has_manifest_request_code() &&
            donorResp.manifest_request_code() != 0) {
            ManifestDonor::NoteRecvOk(jobId, donorResp.manifest_request_code());
        }
    }

    auto resolved = ManifestFetch::Resolve(jobId);
    if (!resolved) {
        LOG_PKTRT_DEBUG("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"skip\":\"no-resolve\",\"job\":{},\"size\":{},\"eresult\":{}}}",
                   jobId, cbBody, hdr.eresult());
        return;
    }

    hdr.set_eresult(static_cast<int32_t>(k_EResultOK));
    const size_t hdrSize = hdr.ByteSizeLong();
    if (hdrSize > kHdrCap || !hdr.SerializeToArray(s_rx.Hdr, kHdrCap)) {
        LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"err\":\"header-encode-fail\",\"size\":{}}}", hdrSize);
        return;
    }
    s_rx.HdrLen = static_cast<uint32_t>(hdrSize);

    CContentServerDirectory_GetManifestRequestCode_Response resp;
    resp.set_manifest_request_code(*resolved);
    const size_t bodySize = resp.ByteSizeLong();
    if (bodySize > kBodyCap || !resp.SerializeToArray(s_rx.Body, kBodyCap)) {
        LOG_PKTRT_WARN("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"err\":\"body-encode-fail\",\"size\":{}}}", bodySize);
        return;
    }
    s_rx.BodyLen = static_cast<uint32_t>(bodySize);

    s_rx.PatchHdr = true;
    s_rx.PatchBody = true;
    LOG_PKTRT_INFO("{{\"evt\":\"DepotFallback\",\"act\":\"recv\",\"job\":{},\"code\":{},\"origSize\":{}}}",
               jobId, *resolved, cbBody);
}

} // namespace NetPacket::Handlers::DepotFallback
