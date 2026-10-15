// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Passive donor capture of genuine manifest request codes (Task 7) plus
// the active probe and wanted-list loop (Task 8).
//
// Passive path: Steam's own downloads already mint codes for depots this
// account can access. This module remembers every outgoing
// GetManifestRequestCode jobid (NoteSent) and, when the matching
// unpatched reply carries eresult=OK with a nonzero code, uploads
// (depot,gid,code) to the archive immediately (NoteRecvOk). Codes rotate
// minutes after issuance, so there is no store-and-forward: upload on a
// detached worker, never stored. No-op when [donate] is disabled.
//
// Wire format mirrors BST ManifestDonor::SubmitCapturedCode read-only:
// POST <base>/manifestcode/submit with a text/plain body of
// "depot:gid:code\n" lines (here always exactly one).
//
// Active path: instead of waiting for Steam to ask, the probe originates
// ContentServerDirectory.GetManifestRequestCode#1 frames itself (Request)
// with jobids from a high range (kProbeJobIdBase) so replies route to
// HandleProbeRecv, never to the passive/inject recv path. A loop thread
// (Start/Stop) pulls the wanted list (GET <base>/manifestwanted,
// "app:depot:gid" lines), mints only listed depots within the [donate]
// caps, and batch-submits the codes. enabled=false (or no send context
// captured yet) means the loop idles with zero traffic.

#pragma once

#include <cstdint>
#include <future>
#include <string>

#include "Steam/Enums.h"

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

    // ── Active probe (Task 8) ──────────────────────────────────────
    //
    // Trampoline type for the TX frame-send hook. Must stay identical to
    // the BBuildAndAsyncSendFrame hook signature in
    // source/hooks/client/NetPacket.cpp (LM_HOOK declares
    // bool(__fastcall*)(void*, EWebSocketOpCode, uint8_t*, uint32_t));
    // BST's ManifestProbe::SendFrameFn is the same shape.
    using SendFrameFn = bool(__fastcall*)(void*, EWebSocketOpCode,
                                          uint8_t*, uint32_t);

    // Captures what originating a frame needs. Called from the TX hook:
    // every binary frame refreshes (ws, send) so a reconnect is tracked,
    // and every k_EMsgServiceMethodCallFromClient frame refreshes the
    // header template. Any argument may be null/empty to leave that slot
    // unchanged. Safe to call from any thread.
    void CaptureSendContext(void* ws, SendFrameFn send,
                            const uint8_t* hdr, uint32_t hdrLen);

    // Originates one GetManifestRequestCode#1 and returns a future for the
    // code. A future holding 0 means not sent (disabled / no context yet),
    // refused by Steam, or timed out — callers treat all three the same.
    // Never blocks the caller: use wait_for, the probe fulfils the promise
    // from the recv path (or not at all — see the 30 s expiry note).
    std::future<uint64_t> Request(uint32_t appId, uint32_t depotId,
                                  uint64_t gid);

    // Consumes replies to originated requests. Returns true when the frame
    // belonged to the probe (caller must skip all other handling).
    // Unknown jobids — including real Steam jobids, which live far below
    // the probe range — return false untouched.
    bool HandleProbeRecv(const uint8_t* body, uint32_t bodyLen,
                         const uint8_t* hdr, uint32_t hdrLen);

    // True once the TX hook has supplied everything originating a frame
    // needs. The loop uses this to idle with zero traffic until Steam is
    // connected (and after a failed hook install, forever).
    bool ProbeReady();

    // True for jobids in the originated range. The passive/inject recv
    // path uses this as a guard so even an untracked (stale, timed-out)
    // probe reply can never be mistaken for a real Steam reply.
    bool IsProbeJobId(uint64_t jobId);

    // ── Wanted-list loop (Task 8) ──────────────────────────────────
    //
    // Starts the donor worker (idempotent). The worker re-reads
    // Settings::donate every cycle, so toggling [donate] takes effect
    // without a restart; while disabled it idles with zero traffic.
    // Wired to NetPacket::Install/Uninstall (same lifetime as the hooks
    // the probe needs). Safe to call from any thread, never from DllMain.
    void Start();

    // Signals the worker to stop. Returns immediately; the worker exits
    // within ~1 s (it sleeps in 1 s slices).
    void Stop();

}
