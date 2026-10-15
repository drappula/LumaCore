// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Passive donor capture of genuine manifest request codes.
//
// Steam's own downloads already mint codes for depots this account can
// access. This module remembers every outgoing GetManifestRequestCode
// jobid (NoteSent) and, when the matching unpatched reply carries
// eresult=OK with a nonzero code, uploads (depot,gid,code) to the
// archive immediately (NoteRecvOk). Codes rotate minutes after issuance,
// so there is no store-and-forward: upload on a detached worker, never
// stored. No-op when [donate] is disabled.
//
// Wire format mirrors BST ManifestDonor::SubmitCapturedCode read-only:
// POST <base>/manifestcode/submit with a text/plain body of
// "depot:gid:code\n" lines (here always exactly one).
//
// Task 8 will add the active probe + loop into this module; this file
// intentionally stays open for that (no probe code here).

#pragma once

#include <cstdint>
#include <string>

namespace ManifestDonor {

    // Remembers jobId -> (depotId, gid) for an outgoing request. Call for
    // EVERY outgoing request while enabled (not just Lua depots); the map
    // is capped at 4096 entries. Returns immediately when
    // [donate] is disabled.
    void NoteSent(uint64_t jobId, uint32_t depotId, uint64_t gid);

    // Looks up the (depot,gid) recorded for jobId and uploads
    // (depot,gid,code) on a detached worker. The caller must only invoke
    // this for an UNPATCHED reply with eresult=OK; a zero code or an
    // untracked jobId is a no-op. Returns immediately (zero traffic) when
    // [donate] is disabled.
    void NoteRecvOk(uint64_t jobId, uint64_t code);

    // Overrides the archive base URL (default is the baked-in
    // https://manifest.luastools.xyz). Empty string restores the default.
    void SetArchiveBase(std::string url);

}
