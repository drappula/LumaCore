# LumaCore Automatic Game Updates (BST Manifest Port) — Design

Date: 2026-10-15
Status: approved in chat (sections 1–5), pending file review
Scope: full auto-update · with donor · respect pins
Approach: faithful BetterSteamTools port

## 1. Intent and success criteria

Goal: LumaCore auto-updates unlocked games through Steam with no manual
manifest grabbing/copying, matching BetterSteamTools behavior.

Today LumaCore has a gid-only request-code bridge
(`source/runtime/ManifestFetch.cpp`) and pin-only `BuildDepotDependency`
(`source/hooks/client/ManifestBind.cpp`). It lacks the disk pre-seed,
the `YldLoadDepotManifest` hook, and the depot-aware provider form, so
updates for unowned depots fail once Valve enforces depot-bound codes.

Success: unowned base game, DLC, and workshop updates download on first
attempt; pinned depots stay frozen; owned depots use Steam's own codes;
scheduled-update retries stay quiet; `[donate] enabled=false` uploads
nothing.

Non-goals: no new Lua API surface; no Cloud saves; no changes to
Denuvo/ticket/stats paths.

## 2. Architecture and components

- New `source/runtime/ManifestCache.{h,cpp}`: owns
  `<Steam>/depotcache/{depot}_{gid}.manifest` writes. Atomic
  tmp+`MoveFileExA` rename, payload/EOF magic validation, in-flight
  dedupe per depot:gid, 10-minute 404 negative cache (restart clears),
  64 MB body cap.
- Upgrade `source/runtime/ManifestFetch.cpp`: depot-aware `opensteamtool`
  provider (`{appid}/{depotid}/{gid}` preferred, gid-only fallback);
  keep wudrm/steamrun gid-only parsers (plain digits + `"content"` /
  `"code"` / `"manifest_request_code"` JSON fields). Lua
  `fetch_manifest_code_ex` > `fetch_manifest_code` > providers.
- Extend `source/hooks/client/ManifestBind.cpp`: record depot→(app,gid)
  seen map before overrides; keep `setManifestid` pin application;
  add transient 0-depot guard (return false so Steam keeps prior depot
  config, fixing the instant-complete bug).
- Add `YldLoadDepotManifest(app,depot,gid,branch)` pattern + hook (new
  file or inside ManifestBind): bounded blocking pre-seed (≤5 s) for
  Lua-tracked depots, covering workshop items that never pass through
  `BuildDepotDependency`. The pattern entry must be published through
  LumaCore's existing pattern chain (Steam-Auto-PT `pattern` branch +
  jsDelivr + local cache); until it exists the hook is skipped through
  the normal pattern-miss path (`status.json` miss, Steam runs that
  function unmodified) and the send-path pre-seed still covers
  non-workshop cases.
- Extend `source/hooks/client/NetPacket_Manifest.cpp` send path: detached
  `EnsureCached` pre-seed for the exact requested gid with
  `bypassNegativeCache=false` always (v1: no download-activity signal
  exists yet, so scheduled-update retries ride the negative cache
  instead of hammering the archive) + existing async code fetch
  joined by jobid. Recv path: patch `eresult=OK` + fetched code, except
  depots belonging to owned apps (leave Steam's genuine code alone).
- Add donor module (`ManifestDonor`): passive capture of genuine codes
  for requested jobids + active `ContentServerDirectory.
  GetManifestRequestCode#1` probe minting for the wanted list, behind
  `[donate]`. Rate-limited (25/cycle, ≥2 s gap, 30 s probe timeout),
  upload-only-owned, never on Steam threads.
- Graceful degradation: missing pattern → hook skipped + `status.json`
  miss; network fail → original Steam path; nothing blocks Steam startup.

## 3. Data flow

1. `BuildDepotDependency` records Steam's depot set, applies pins.
2. At download time `YldLoadDepotManifest` pre-seeds the exact manifest;
   on hit, Steam's disk check succeeds and no request code is needed.
3. Outgoing `GetManifestRequestCode` fires detached cache fill + async
   provider fetch; incoming reply is patched with the fetched code
   (owned depots untouched).
4. 404s enter the negative cache and feed one debounced,
    once-per-(depot,gid)-per-session missing-manifest notice for the
    batch (fired from both pre-seed call sites whenever the archive
    reports a definitive 404, regardless of download state).
5. Donor separately harvests genuine codes and mints wanted-list codes
   for upload; disabled state produces zero extra traffic.

## 4. Config and Lua surface

- No new Lua functions. Existing `fetch_manifest_code(_ex)` plus
  `http_get`/`http_post` keep priority.
- `[manifest_fetch] urls` templates gain `{appid}`/`{depotid}`
  substitution; `{gid}`-only templates keep working. New defaults:
  depot-aware opensteamtool URL first, then gid-only steamrun/wudrm.
- New `[donate]` section: `enabled=true` default with
  `interval_secs=30`, `wanted_refresh_secs=300`,
  `max_mints_per_cycle=25`, `min_mint_interval_ms=2000`,
  `max_mints_per_session=0`. `enabled=false` opts out fully.
- Pins respected: `setManifestid` freezes that depot; existing
  `skipManifestPin`/auto-update bookkeeping unchanged; unpinned depots
  track latest.

## 5. Error handling and safety

- Timeouts: Yld pre-seed ~5 s; background cache fill 60 s recv budget;
  code fetch keeps the existing ~12 s budget. Slow hosts fail fast to
  the next provider or the original path.
- 404 is definitive (negative-cached); other failures are transient
  (retried promptly, never cached as missing).
- Owned-depot bypass via the seen-map avoids replacing working Steam
  codes with carrier-minted ones (CDN 401 cause).
- Donor uploads only codes Steam actually granted — the reply is the
  ownership oracle (a refused mint reports nothing); no library
  enumeration.
- Disk writes validated (size + magic) and atomic; concurrent fetches
  deduped; per-cycle and per-session mint caps bound Steam traffic.

## 6. Testing

- Build: `build.bat` clean configure + compile; no new third-party deps.
- Unit-checkable logic: template expansion, digit/JSON parsers,
  negative-cache TTL, magic validation, atomic write.
- Manual Steam matrix: unowned base update; DLC during base download;
  workshop item; pinned depot frozen; owned depot uses Steam code;
  restart mid-refresh does not instant-complete; donor-off means no
  uploads.
- Observability: `manifest.log` / `manbnd.log` / `pktrt.log` for
  pre-seed hit/miss, provider shape, injection, mints; `status.json`
  for hook misses.

## 7. Open items resolved in chat

- Scope: full auto-update (not cache-only or codes-only).
- Donor: included, default on, opt-out supported.
- Pins: explicit pins freeze; everything else tracks latest.
- Approach: faithful BST port (not staged).
