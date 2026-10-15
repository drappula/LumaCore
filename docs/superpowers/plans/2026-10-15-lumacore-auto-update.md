# LumaCore Automatic Updates (BST Manifest Port) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port BetterSteamTools' automatic manifest handling into LumaCore so unlocked games update through Steam with no manual manifest work.

**Architecture:** New `ManifestCache` disk pre-seed (`depotcache/{depot}_{gid}.manifest`) plus depot-aware request-code provider upgrade plus donor upload loop, all behind existing Lua/pattern/hook plumbing with graceful degradation when patterns or network are missing.

**Tech Stack:** C++20, CMake 3.20+, MSVC x64, WinHTTP (`RuntimeHttp`), Detours (`LM_HOOK`/`LM_INSTALL`), protobuf service frames (`NetPacket` TX/RX dispatch).

**Spec:** `docs/superpowers/specs/2026-10-15-lumacore-auto-update-design.md`

## Global Constraints

- Windows-only: build with `build.bat` (or the same CMake/MSVC steps CI `release.yml` runs on `windows-latest`); this Linux checkout cannot compile or run Steam — verification is compile + log/matrix on Windows or via CI.
- No unit-test harness exists in this repo: each task's test cycle is a clean build plus the named log assertions and, where marked, the manual Steam matrix. Do not invent a test framework.
- `LOG_*` macros compile to no-ops in Release: use `LOG_MANIFESTCH_*` / `LOG_MANBND_*` / `LOG_PKTRT_*` so Debug logs verify behavior.
- Never blocking Steam threads except the bounded Yld pre-seed (≤5 s); everything else is detached workers.
- Donor uploads only depots Steam itself requested and the account owns; `enabled=false` means zero extra traffic.
- Explicit `setManifestid` pins always freeze; unpinned depots track latest.

## Review Focus

- Depot-bound code rejected by CDN (401 → "Failed downloading 1 manifests"): provider must send `app/depot/gid` shape for opensteamtool on non-carrier depots; Task 4 test pins the URL shape in logs.
- 19 MB manifest truncated by the 8 MiB `RuntimeHttp` cap: Task 3 test asserts the cache-hit log shows full byte count and magic validation passed (truncation would fail the EOF-magic check and log a rejection instead).
- Idle-startup popup for scheduled retries of missing manifests (no download-activity signal exists in v1): accepted v1 behavior is one popup per depot per session; Task 6 test asserts single-fire.
- Depot both Lua-added and genuinely owned: Task 6 test asserts Steam's genuine code is left untouched (seen-map bypass).
- `YldLoadDepotManifest` pattern missing for the running build: Task 5 test asserts hook skipped + `status.json` miss while everything else works.

---

### Task 1: Large-body HTTP variant for manifest downloads

**Files:**
- Modify: `source/runtime/RuntimeHttp.h`
- Modify: `source/runtime/RuntimeHttp.cpp`
- Test: Windows build + a Debug-logged large download check

**Interfaces:**
- Consumes: existing `RuntimeHttp::Get(url, userAgent)` (12 s timeouts, 8 MiB cap).
- Produces: `RuntimeHttp::Response GetLarge(std::string_view url, std::wstring_view userAgent, uint32_t maxBytes, uint32_t recvTimeoutMs)` — same `Response` struct, `maxBytes` cap enforced, longer recv budget; existing `Get` behavior unchanged.

- [ ] **Step 1: Add `GetLarge` declaration to `source/runtime/RuntimeHttp.h`.**
- [ ] **Step 2: Implement `GetLarge` in `source/runtime/RuntimeHttp.cpp`** by parameterizing the existing WinHTTP path on timeout/cap (no behavior change to `Get`).
- [ ] **Step 3: Build on Windows and verify it compiles.**

Run: `build.bat`
Expected: `Releases/Debug/LumaCore.dll` (or Release equivalent) produced, no new warnings from `RuntimeHttp.cpp`.

- [ ] **Step 4: Commit.**

```bash
git add source/runtime/RuntimeHttp.h source/runtime/RuntimeHttp.cpp
git commit -m "feat: add RuntimeHttp::GetLarge for manifest-size bodies"
```

### Task 2: Settings — depot-aware defaults plus `[donate]` section

**Files:**
- Modify: `source/config/Settings.h`
- Modify: `source/config/Settings.cpp`
- Modify: `lumacore.toml.example`
- Test: Windows build + config-load log check

**Interfaces:**
- Consumes: `Settings::Load`, `manifestFetchUrls`, `manifestFetchTimeoutSec`.
- Produces: `inline std::vector<std::string> manifestFetchUrls` defaulting to `{"https://manifest.opensteamtool.com/{appid}/{depotid}/{gid}", "https://manifest.steam.run/api/manifest/{gid}", "http://gmrc.wudrm.com/manifest/{gid}"}`; new `struct DonateSettings { bool enabled=true; int intervalSecs=30; int wantedRefreshSecs=300; int maxMintsPerCycle=25; int minMintIntervalMs=2000; int maxMintsPerSession=0; };` + `inline DonateSettings donate;` parsed from `[donate]` (absent section keeps defaults; `enabled=false` opts out).

- [ ] **Step 1: Update defaults and add `DonateSettings` in `source/config/Settings.h`.**
- [ ] **Step 2: Parse `[donate]` keys in `Settings::Load` in `source/config/Settings.cpp`** (same toml++ style as `[manifest_fetch]`; unknown keys ignored).
- [ ] **Step 3: Document `[donate]` and the new default URL in `lumacore.toml.example`.**
- [ ] **Step 4: Build on Windows and verify defaults load.**

Run: `build.bat`
Expected: compiles; Debug `misc.log`/`main.log` shows depot-aware opensteamtool URL first in the fetch chain (log line already exists in `ManifestFetch.cpp:179`).

- [ ] **Step 5: Commit.**

```bash
git add source/config/Settings.h source/config/Settings.cpp lumacore.toml.example
git commit -m "feat: depot-aware manifest defaults and [donate] settings"
```

### Task 3: `ManifestCache` disk pre-seed module

**Files:**
- Create: `source/runtime/ManifestCache.h`
- Create: `source/runtime/ManifestCache.cpp`
- Modify: `source/CMakeLists.txt` (add `runtime/ManifestCache.cpp` to `add_library(LumaCore ...)`)
- Test: Windows build + log-verified cache behavior

**Interfaces:**
- Consumes: `RuntimeHttp::GetLarge` (Task 1), `Settings::manifestFetchTimeoutSec` resolve/connect/send values, Lua-state-independent.
- Produces: `namespace ManifestCache { bool EnsureCached(uint32_t appId, uint32_t depotId, uint64_t gid, uint32_t recvTimeoutMs, bool* outNotArchived, bool bypassNegativeCache); }` — writes `<Steam>/depotcache/{depot}_{gid}.manifest` atomically (tmp + `MoveFileExA`), validates payload magic `0x71F617D0` / EOF `0x32C415AB`, dedupes concurrent same-key fetches, 10-minute 404 negative cache (process-lifetime only), archive URL `https://manifest.luastools.xyz/m/{depot}/{gid}`, 64 MB cap, 60 s default recv budget. Returns true when the file exists afterwards; `outNotArchived` true only on definitive 404.

- [ ] **Step 1: Write `source/runtime/ManifestCache.h`** with the `EnsureCached` signature above.
- [ ] **Step 2: Implement `source/runtime/ManifestCache.cpp`** (BST `ManifestCache.cpp` is the reference: `DepotCacheDir` via main-executable path, `LooksLikeManifest`, `WriteAtomic`, `InFlightGuard`, `g_negCache`).
- [ ] **Step 3: Register the file in `source/CMakeLists.txt` next to `runtime/ManifestFetch.cpp`.**
- [ ] **Step 4: Build on Windows and verify no regressions.**

Run: `build.bat`
Expected: compiles; existing manifest-code path behavior unchanged (no callers of `EnsureCached` yet, so no log changes).

- [ ] **Step 5: Commit.**

```bash
git add source/runtime/ManifestCache.h source/runtime/ManifestCache.cpp source/CMakeLists.txt
git commit -m "feat: add ManifestCache depotcache pre-seed"
```

### Task 4: Depot-aware code fetch in `ManifestFetch`

**Files:**
- Modify: `source/runtime/ManifestFetch.cpp`
- Test: Windows build + provider-shape log check

**Interfaces:**
- Consumes: `Settings::manifestFetchUrls` with `{appid}`/`{depotid}` templates (Task 2), existing `ExpandTemplate` (already substitutes all three).
- Produces: same `Submit`/`Resolve`/`Discard`/`ClearCache` signatures; behavior change only: opensteamtool-style templates containing `{appid}` or `{depotid}` are expanded with the real `appId`/`depotId` from the request (BST rule: `appId` may be 0, `depotId` must be right), and the resolve log names the shape (`app/depot/gid` vs `gid-only`) like BST `ManifestClient.cpp:129`.

- [ ] **Step 1: Verify `ExpandTemplate` already handles `{appid}`/`{depotid}`** (it does per `ManifestFetch.cpp:97-113`); add the shape tag to the `LOG_MANIFESTCH_INFO` provider line.
- [ ] **Step 2: Build on Windows and verify the log shape.**

Run: `build.bat`
Expected: compiles; Debug `manifest.log` shows `provider 1/N GET https://manifest.opensteamtool.com/{app}/{depot}/{gid}` with `shape=app/depot/gid` on a real request.

- [ ] **Step 3: Commit.**

```bash
git add source/runtime/ManifestFetch.cpp
git commit -m "feat: depot-aware manifest request-code shape"
```

### Task 5: `ManifestBind` seen-map, empty-config guard, `YldLoadDepotManifest` hook

**Files:**
- Modify: `source/hooks/client/ManifestBind.cpp`
- Modify: `source/hooks/client/ManifestBind.h` (export `LookupDepot` + `ReportMissingManifest` if the header owns the namespace; otherwise add them where `NetPacket_Manifest` can reach)
- Modify: `source/core/Orchestrator.cpp` only if hook install lists need the new hook (no — same `ManifestBind::Install` entry)
- Test: Windows build + pattern-miss graceful test + guard log test

**Interfaces:**
- Consumes: `ManifestCache::EnsureCached` (Task 3), `LuaLoader::GetManifestOverrides` / `HasDepot` / `IsOwned`, `LM_HOOK`/`LM_INSTALL` macros, pattern key `"YldLoadDepotManifest"` (external dependency: entry must be published in the Steam-Auto-PT `pattern` branch; until present the hook logs a miss and skips).
- Produces: `bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid)` (depot→(app,gid) recorded from Steam's own `BuildDepotDependency` output before overrides); `void ReportMissingManifest(uint32_t depotId, uint64_t gid)` (once-per-depot-per-session debounced warning, declared here, delivered in Task 6 or here — implementer picks one file and documents it); `BuildDepotDependency` returns false for Lua-depot apps with a transient 0-depot list (log `app {} returned 0 depots (appinfo mid-refresh)`); new `YldLoadDepotManifest(appId, depotId, manifestGid, branch)` hook doing bounded blocking `EnsureCached(..., 5000 ms)` for `HasDepot(depotId)` only, then calling the original.

- [ ] **Step 1: Add the seen-map + `LookupDepot` and record depots before applying pin overrides in `ManifestBind.cpp`.**
- [ ] **Step 2: Add the transient 0-depot guard** (BST `Hooks_Manifest.cpp:164-172` is the reference wording and scoping: `AppId != 0`, `pDepotInfo` empty, `HasDepot(AppId)`).
- [ ] **Step 3: Add the `YldLoadDepotManifest` hook** via `LM_HOOK`/`LM_INSTALL` with signature `(void* a1, void* a2, int appId, uint32_t depotId, uint64_t gid, const char* branch)`; verify against BST `Hooks_Manifest.cpp:224` calling convention before committing to it.
- [ ] **Step 4: Publish the `YldLoadDepotManifest` pattern entry to the Steam-Auto-PT `pattern` branch** (external repo PR with `name`, `rva`, `sig` for the current Steam build, following the existing pattern-file format in `docs/LumaCore.md`). Until it merges, the hook stays gracefully skipped per the test below — this step may land after the code.

- [ ] **Step 5: Build on Windows and verify graceful behavior without the pattern entry.**

Run: `build.bat`
Expected: compiles; Debug log shows `YldLoadDepotManifest skipped (TOML entry missing…)` + `status.json` miss, and `BuildDepotDependency` still applies pins for all other apps.

- [ ] **Step 6: Commit.**

```bash
git add source/hooks/client/ManifestBind.cpp source/hooks/client/ManifestBind.h
git commit -m "feat: depot seen-map, empty-config guard, Yld pre-seed hook"
```

### Task 6: Send-path pre-seed, owned-depot bypass, missing notice

**Files:**
- Modify: `source/hooks/client/NetPacket_Manifest.cpp`
- Test: Windows build + manual Steam matrix (Debug logs)

**Interfaces:**
- Consumes: `ManifestCache::EnsureCached` (Task 3), `ManifestBind::LookupDepot` (Task 5), `Settings::donate.enabled` for capture gating (Task 7 reads it; this task only needs `LuaLoader::HasDepot`/`IsOwned`).
- Produces: same `HandleSend`/`HandleRecv` signatures; new behavior — `HandleSend`: after parsing `depot/gid/app/jobId`, spawn a detached `EnsureCached(app,depot,gid, 0, &notArchived, bypass=false)` worker and, when `notArchived` on a Lua depot, call `ReportMissingManifest` (once per depot per session, debounced single popup via `MessageBoxA` on the worker, never on Steam threads). Injection path: if `LookupDepot` maps the depot to an `IsOwned` app, skip `ManifestFetch::Submit` and leave Steam's genuine code alone (log `belongs to owned app …, leaving Steam's own code alone`).

- [ ] **Step 1: Add the detached pre-seed worker to `HandleSend`** (copy BST `Hooks_NetPacket.cpp:1066-1090` structure; v1 uses `bypass=false` always — no download-activity signal exists yet, see Review Focus).
- [ ] **Step 2: Add the owned-depot bypass before `ManifestFetch::Submit`.**
- [ ] **Step 3: Build on Windows and run the manual matrix.**

Run: `build.bat`, then in Steam (Debug build): update an unowned Lua game, a DLC, and a pinned depot.
Expected: `manifest.log` shows pre-seed hit/miss + `shape=`; unpinned depots download first attempt; pinned depot stays frozen; owned depot log shows the bypass line with Steam's code untouched.

- [ ] **Step 4: Commit.**

```bash
git add source/hooks/client/NetPacket_Manifest.cpp
git commit -m "feat: manifest pre-seed on code requests with owned bypass"
```

### Task 7: Donor passive capture (standalone value, no probe yet)

**Files:**
- Create: `source/runtime/ManifestDonor.h`
- Create: `source/runtime/ManifestDonor.cpp`
- Modify: `source/hooks/client/NetPacket_Manifest.cpp` (record + harvest calls)
- Modify: `source/CMakeLists.txt`
- Test: Windows build + `[donate] enabled=false` silence test

**Interfaces:**
- Consumes: `Settings::donate.enabled`, existing `HandleSend`/`HandleRecv` jobids, `LuaLoader` ownership not needed (capture is for whatever Steam requested; the server only accepts codes for owned depots).
- Produces: `namespace ManifestDonor { void NoteSent(uint64_t jobId, uint32_t depotId, uint64_t gid); void NoteRecvOk(uint64_t jobId, uint64_t code); void SetArchiveBase(std::string url); }` — remembers `jobid→(depot,gid)` on send (cap 4096 entries), and when the matching reply carries `eresult=OK` with a genuine code, uploads `(depot,gid,code)` to the archive immediately (never stored); all uploads on detached workers; when `donate.enabled=false`, both functions return immediately.

- [ ] **Step 1: Write `ManifestDonor.h`/`ManifestDonor.cpp`** with the above interface (BST `ManifestDonor`/`Hooks_NetPacket` capture blocks are the reference).
- [ ] **Step 2: Wire `NoteSent` into `HandleSend` and `NoteRecvOk` into `HandleRecv`** (harvest only when the unpatched reply is `eresult=OK` with a nonzero code).
- [ ] **Step 3: Register the file in `source/CMakeLists.txt`.**
- [ ] **Step 4: Build on Windows and verify silence when disabled.**

Run: `build.bat`
Expected: compiles; with `[donate] enabled=false` a full Steam update produces zero donor HTTP traffic in logs; with `enabled=true` genuine codes for downloaded depots log one upload line each.

- [ ] **Step 5: Commit.**

```bash
git add source/runtime/ManifestDonor.h source/runtime/ManifestDonor.cpp source/hooks/client/NetPacket_Manifest.cpp source/CMakeLists.txt
git commit -m "feat: donor passive capture of genuine manifest codes"
```

### Task 8: Donor active probe and wanted-list loop

**Files:**
- Modify: `source/runtime/ManifestDonor.cpp` (probe + loop)
- Modify: `source/hooks/client/PacketRouter.cpp` or `source/hooks/client/NetPacket.cpp` (capture TX send-context for originated frames — implementer confirms the exact `BBuildAndAsyncSendFrame` hook signature first)
- Test: Windows build + rate-limit log test

**Interfaces:**
- Consumes: `Settings::donate` intervals/caps (Task 2), donor archive base URL, the TX hook's frame-send trampoline + a real outbound header template.
- Produces: internal `Request(appId, depotId, gid) -> std::future<uint64>` originating `ContentServerDirectory.GetManifestRequestCode#1` with jobids from a high range (`0x7E51'0000'0000'0000` base, BST convention) so replies route to the probe, not `HandleRecv`; a loop thread (30 s cycle, `wanted_refresh_secs=300`, ≤25 mints/cycle, ≥2000 ms gap, session cap) that pulls the wanted list, probes only listed depots, and uploads minted codes. Replies with unknown jobids are ignored by `HandleRecv`.

- [ ] **Step 1: Confirm the TX hook signature and add send-context capture** (websocket object + send trampoline + header template, BST `ManifestProbe::CaptureContext` pattern adapted to LumaCore's `PacketPool` frame assembly).
- [ ] **Step 2: Implement the probe request/reply path with the high-range jobid base.**
- [ ] **Step 3: Implement the paced wanted-list loop honoring all `donate` caps.**
- [ ] **Step 4: Build on Windows and verify pacing.**

Run: `build.bat`
Expected: compiles; Debug `manifest.log` shows `ManifestProbe send/recv` lines spaced ≥2 s apart, ≤25 per cycle, and stops entirely with `enabled=false`.

- [ ] **Step 5: Commit.**

```bash
git add source/runtime/ManifestDonor.cpp source/hooks/client/PacketRouter.cpp source/hooks/client/NetPacket.cpp
git commit -m "feat: donor active probe and wanted-list loop"
```

### Task 9: Docs, example config, final verification

**Files:**
- Modify: `docs/LumaCore.md` (extend `Manifest Bind & Fetch` section)
- Modify: `lumacore.toml.example` (verify `[donate]` docs match `Settings.cpp` keys exactly)
- Test: full manual matrix + spec cross-check

**Interfaces:**
- Consumes: all prior tasks.
- Produces: documented `[manifest_fetch]` `{appid}`/`{depotid}` templates, `[donate]` keys, pin semantics, log file pointers; every spec §6 item verified or explicitly deferred.

- [ ] **Step 1: Update `docs/LumaCore.md`** (provider chain, depot-aware shape note, depotcache pre-seed, donor behavior + opt-out, pin semantics).
- [ ] **Step 2: Re-check `lumacore.toml.example` key-for-key against `Settings.cpp`.**
- [ ] **Step 3: Build Release + Debug on Windows and run the full matrix from spec §6.**

Run: `build.bat`
Expected: all seven matrix items pass (base, DLC, workshop, pinned frozen, owned untouched, no instant-complete on restart refresh, donor-off silence); any failure is filed as a follow-up, not silently accepted.

- [ ] **Step 4: Commit.**

```bash
git add docs/LumaCore.md lumacore.toml.example
git commit -m "docs: automatic updates and donor configuration"
```

## Scope note

Tasks 1–6 deliver working auto-updates without any donor code; Task 7 adds passive contribution; Task 8 adds active minting. If review prefers a smaller landing, Tasks 1–6 + 9 merge first and 7–8 follow.
