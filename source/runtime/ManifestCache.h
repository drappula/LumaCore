// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.
//
// Pre-seed <Steam>\depotcache from the manifest archive.
//
// Fetches an archived depot manifest from the archive (base URL baked
// into ManifestCache.cpp) and drops it into the root depotcache before
// Steam asks for a manifest request code. Steam then reads the cached
// manifest and skips the request-code/CDN path entirely.
//
// All calls are best-effort: a miss (404), a bad body, or an
// unresolvable Steam dir simply leaves no file, and Steam falls through
// to the existing request-code path. Safe to call from a worker thread.
// Lua-state-independent.

#ifndef LUMACORE_MANIFEST_CACHE_H
#define LUMACORE_MANIFEST_CACHE_H

#include <cstdint>

namespace ManifestCache {

    // Ensure <Steam>\depotcache\<depotId>_<gid>.manifest exists, fetching
    // it from https://manifest.luastools.xyz/m/<depotId>/<gid> if missing.
    // Returns true when the file is present afterwards (already cached
    // or freshly written). Returns false — quietly — when the manifest
    // is not archived, the body fails validation, or the fetch fails.
    // Never throws.
    //
    // recvTimeoutMs bounds the body-receive wait; 0 keeps the built-in
    // 60 s default. Resolve/connect/send keep RuntimeHttp's own budgets,
    // mirroring ManifestFetch (which calls RuntimeHttp with defaults).
    //
    // outNotArchived is null-safe: when non-null it is set to true ONLY
    // when the archive gave a definitive 404 (the manifest is not
    // archived yet) — not for timeouts or other transient failures.
    // Callers use it to tell "not archived" apart from a passing blip.
    //
    // bypassNegativeCache forces a fresh fetch even for a depot:gid that
    // recently 404'd. A depot that returns 404 is remembered for 10
    // minutes (process lifetime only) and skipped (no GET) on
    // subsequent calls — this stops scheduled-update retries from
    // hammering the archive. Pass true only when a download is actually
    // starting, so a just-supplied manifest is picked up immediately.
    bool EnsureCached(uint32_t appId, uint32_t depotId, uint64_t gid,
                      uint32_t recvTimeoutMs, bool* outNotArchived,
                      bool bypassNegativeCache);
}

#endif
