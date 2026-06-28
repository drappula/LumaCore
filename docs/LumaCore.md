# LumaCore — Feature Reference

This document describes every subsystem in LumaCore, its purpose, the Steam internals it touches, and the configuration interface exposed to SteaMidra via Lua scripts.

---

## Injection chain

Steam loads DLLs from its own directory on startup.  LumaCore exploits this by placing two thin proxy DLLs alongside `steam.exe`:

- `dwmapi.dll` — forwards the full DWM API surface and loads `LumaCore.dll` on attach
- `xinput1_4.dll` — forwards XInput 1.4 exports; acts as a backup load gate, calling `LoadLibraryA("LumaCore.dll")` on process attach as well

When Steam starts, Windows loads the proxy DLLs before any game code runs.  The proxy's `DllMain` loads `LumaCore.dll` and returns.

`LumaCore.dll` then:

1. Copies `steamclient64.dll` to `bin\lcoverlay.dll` (with retry logic in case the file is locked).
2. Loads `lcoverlay.dll` explicitly so it has an independent module handle.
3. Reads the current Steam build ID from `steam.exe!GetBootstrapperVersion` and stores it for diagnostics + status surfacing.
4. Synchronously primes the runtime pattern cache from disk for both `steamclient64.dll` and `steamui.dll`, so the first hook installer sees a populated pattern map without waiting on the network.
5. Spawns a worker thread that installs all hooks, kicks off the network refresh path for the per-build pattern files in the background, and starts the Lua directory watcher.

The copy step is necessary because hooking the live `steamclient64.dll` while it is already mapped into the process would require patching code that is in use.  Hooking the private copy avoids race conditions and keeps the original file untouched on disk.

---

## Pattern resolution (`hooks/PatternFetcher.cpp` + `utils/ByteScan.cpp`)

LumaCore locates Steam internal functions through a runtime pattern map.  At startup the fetcher hashes `steamclient64.dll` and `steamui.dll` (lowercase hex SHA-256), looks up a matching `<sha>.toml` for each, and stores the parsed entries in an in-memory map keyed by function name.  Each entry is a `name`, an `rva` relative to that DLL's image base, and a byte `sig` (hex with `??` wildcards) used to verify the bytes at that rva before any hook attaches.

### Pattern file format

Section keys are FNV-1a 32-bit of the function name (offset basis `0x811c9dc5`, prime `0x01000193`), filenames are the lowercase-hex SHA-256 of the inspected DLL, fields are `name`, `rva`, `sig`:

```toml
[0x82428E37]
name = "BBuildAndAsyncSendFrame"
rva  = "0xD15DD0"
sig  = "48 8B C4 55 48 8D 68 A1 48 81 EC C0 00 00 00 48 89 70 18"
```

The schema matches the runtime pattern map format — TOML files dropped into `<Steam>\lumacore\pattern\` resolve without further conversion.

### Source priority

For each DLL, the fetcher tries sources in this order:

1. **User mirror** (optional). If `[pattern_fetch] mirror` is set in `lumacore.toml`, the fetcher substitutes `{subdir}` (`steamclient` or `steamui`) and `{sha}` into the URL and treats it as the first try. Any failure (HTTP 4xx/5xx, network error, parse error) logs a debug line and falls through.
2. **GitHub raw** — `raw.githubusercontent.com/KoriaPolis/Steam-Auto-PT/pattern/<subdir>/<sha>.toml`.
3. **jsDelivr CDN** — `cdn.jsdelivr.net/gh/KoriaPolis/Steam-Auto-PT@pattern/<subdir>/<sha>.toml`. Used only on transport failure since GitHub raw and jsDelivr serve the same content; a 404 from either short-circuits to the cache step.
4. **Local cache** — `<Steam>\lumacore\pattern\<sha>.toml`. Always written-through on a successful fetch and always read on a network miss.

### Cache and atomic writes

Cache writes go through `<sha>.toml.tmp` followed by `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`, so a writer crash, power loss, or concurrent reads from multiple Steam-instance processes never expose a partially written file. Surviving `.tmp` files from a crashed writer get swept on the next successful fetch into the same directory.

### Fallback and graceful degradation

The hook installer macros call `ByteSearch(module, "FunctionName")`, which consults the in-memory pattern map, verifies the bytes at `module_base + entry.rva` match the TOML's sig, and returns the address. Out-of-range rva values, sig mismatches, or missing names log a warning and `RecordMissed` into `status.json`; the hook is silently skipped and Steam runs that function unmodified. A missing TOML for one DLL never blocks hook installs in the other DLL, so a partial pattern set still produces a partially-functional LumaCore install instead of aborting.

There are no compiled-in `*Sigs[]` arrays anymore. The runtime pattern map is the single source of truth; the legacy `hooks/PatternDb.h` header is gone.

### Pattern refresh and the analyzer

`cleintcheck/steamclient_analyzer.py` is the maintainer-side tool that produces `<sha>.toml` files for the runtime fetcher.

```
cd cleintcheck
python steamclient_analyzer.py "C:\Program Files (x86)\Steam\steamclient64.dll" \
       --steamui "C:\Program Files (x86)\Steam\steamui.dll" \
       --emit toml --out-dir PatternsUpdate
```

The script computes the SHA-256 of each DLL, locates every target function via a hybrid of string-XRef and byte-pattern matching, and writes `PatternsUpdate/steamclient/<sha>.toml` and `PatternsUpdate/steamui/<sha>.toml` ready for upload to the pattern repo. Pass `--no-canonical-overlay` to skip the merge pass.

The runtime fetcher's own logs (`<Steam>\lumacore\misc.log`) note every overlay, cache, and network step so it's straightforward to triage a build that doesn't resolve cleanly.

---

## Hook modules

### DepotKeys (`hooks/client/DepotKeys.cpp`)

Hooks `LoadDepotDecryptionKey`.

When Steam mounts a depot it calls this function to fetch the AES-128 decryption key for that depot from the user's license data.  The hook intercepts the call, checks whether `LuaLoader` has a key for the requested depot ID (loaded from the `.lua` script provided by SteaMidra), and writes it into the output buffer.  If no key is known, the call falls through to the original function.

Lua interface:

```lua
addappid(1234567, 1, "0A1B2C3D...")  -- depot 1234567, decryption key
```

---

### IPCBus (`hooks/client/IPCBus.cpp`)

Hooks `IPCProcessMessage` and resolves `GetPipeClient` — both via pure byte-pattern matching.

Steam uses an internal IPC bus to route messages between its client service and the UI process.  The hook intercepts `IPCProcessMessage`, inspects the command code, and dispatches it to any registered LumaCore handlers.  Currently the following handlers are active:

- `GetSteamID` — returns a spoofed SteamID (see CmdUser below)
- `GetAppOwnershipTicketExtendedData` — produces a synthetic AppTicket for apps in the Lua config

All other messages pass through unmodified.

Both `GetPipeClient` and `IPCProcessMessage` resolve through the same runtime pattern map every other hook uses; the address is verified against the TOML's sig at `module_base + rva` before any detour attaches. String cross-reference resolution was previously attempted for these two and reverted because the referenced strings can resolve to helper functions at early startup, producing a null pipe pointer and crashing Steam on the first IPC Handshake. Pattern-only resolution sidesteps that hazard.

---

### CmdUser (`hooks/CmdUser.cpp`)

Handles the `GetSteamID` and `GetAppOwnershipTicketExtendedData` IPC commands.

**GetSteamID**: returns the SteamID configured in `lumacore.toml` under `[user] steam_id`.  For Denuvo-protected titles, which embed the owning SteamID in the AppTicket and validate it at runtime, LumaCore uses `GetDynamicOwnerSteamID`.  That function searches `Steam\userdata\` directories for an account that has local app data for the requested game and returns that account's ID.  This avoids hardcoding a single SteamID for users who run multiple accounts.

**GetAppOwnershipTicketExtendedData**: builds a synthetic AppTicket and ETicket for apps listed in the active `.lua` config.  The ticket includes the SteamID resolved above, the app's package ID read from the Lua script, and a minimal set of ownership flags.

---

### ManifestBind (`hooks/ManifestBind.cpp`)

Handles the manifest-key binding that associates a depot manifest with the active decryption key.  When Steam mounts a manifest, it calls this function to verify that the manifest's encryption was produced with the key the user holds.  The hook ensures keys supplied via Lua are accepted for this check.

---

### DecryptionKeyHook (`hooks/client/DecryptionKeyHook.cpp`)

Hooks `ConfigStoreGetBinary` to intercept Steam's license decryption config reads.

When Steam needs a decryption key for an app license, it calls `ConfigStoreGetBinary` to fetch the encrypted blob. The hook checks the Lua config for a matching app and, if a key is available, writes it to the output buffer. Falls through to the original when no key is known.

Caches app tickets read from the Windows registry under `HKEY_CURRENT_USER\Software\Valve\Steam\Apps\` for use by the AppTicket forge pipeline.

---

### DenuvoAuth (`hooks/client/DenuvoAuth.cpp` + `runtime/ProtectionScan.cpp`)

Handles Denuvo-protected game authorization through Steam's internal pipe handshake.

When a Denuvo game launches, its DRM layer makes several IPC requests to Steam to verify the owner identity. DenuvoAuth tracks these via `PipeWatch` handshake events, scans the game's process modules for Denuvo packer signatures, and enters an authorization window for the first N handshakes. While the window is open, all IPC calls from that pipe see the spoofed owner SteamID instead of the borrower's.

Detection uses three methods in priority order:
1. **OEP pattern** — scans the entry-point section for the packed OEP byte signature
2. **Protected blob** — detects W+X sections with high entropy (7.0+), typical of Denuvo's code virtualization
3. **Legacy section string** — searches for the `DENUVO` string inside known Denuvo PE sections (`.arch`, `.srdata`, `.xpdata`, `.xdata`, `.xtls`)

If all three methods miss but an injected encrypted app ticket exists for the app, the auth path engages anyway as a safety net. The main game executable is always scanned regardless of size; DLLs below 80 MB are skipped as a perf optimization.

Lua interface:

```lua
forcedenuvo(1234567)  -- force Denuvo auth for this app even if scan misses
addprocess("game.exe", 1234567)  -- map process name to appId for match-by-exe
```

---

### PipeWatch (`hooks/client/PipeWatch.cpp`)

Monitors Steam's internal pipe handshake messages to build a live map of connected processes.

On every `Handshake` IPC command, PipeWatch inspects the connecting process: reads its environment block for `SteamAppId`/`SteamGameId`/`SteamOverlayGameId` variables, enumerates loaded modules to detect Steam/EOS/denuvo presence, and builds a `ProcessSnapshot` with the collected data. The snapshot feeds downstream systems like DenuvoAuth (pipe authorization window) and PacketRouter (appId resolution with retry).

---

### IpcDispatch + IpcHooks (`hooks/client/IpcDispatch.cpp`, `hooks/client/IpcHooks.cpp`)

Registers runtime ticket-spoofing IPC handlers through IPCBus's existing dispatch system.

Instead of installing its own `IPCProcessMessage` hook (which would collide with IPCBus), IpcDispatch converts a pre/post handler model into `IpcHandlerEntry` slots that IPCBus dispatches from its own hook. Handlers are registered at startup from per-interface registration functions and keyed on the same `funcHash` values resolved by `IpcLoader`.

The dispatch layer handles response buffer modification for `GetSteamID`, `GetAppOwnershipTicketExtendedData`, `RequestEncryptedAppTicket`, `GetEncryptedAppTicket`, `GetAppID`, and `GetAPICallResult`. All other messages pass through unmodified.

---

### EticketFetcher (`runtime/EticketFetcher.cpp`)

On-demand encrypted app ticket minting via HTTP GET.

When the Lua config calls `setEticket()` or `seteticketurl()`, the fetcher issues an HTTP request to the configured URL and writes the returned blob into LumaCore's credential store. The eticket then feeds into the AppTicket forge pipeline for Denuvo-protected games that need a valid encrypted ticket to pass the DRM check.

---

### OnlineFixInject (`hooks/client/OnlineFixInject.cpp`)

Detours `CreateProcessW` and `CreateProcessAsUserW` to inject `LumaCorePayload.dll` into game processes launched with `-onlinefix`.

When Steam spawns a game process, the hook checks the command line for the `-onlinefix` flag. If present, it creates the process suspended, allocates remote memory for the payload DLL path, queues an APC to call `LoadLibraryW`, and resumes the thread. LumaCorePayload then handles EOS bridge / lobby redirection for online-fix multiplayer.

---

### ManifestOverride (`hooks/client/ManifestBind.cpp` + `runtime/ManifestFetch.cpp`)

Manifest download bridge with HTTPS-first URL chain fallback.

When Steam requests a depot manifest (gid) and the original call fails with a network error, the bridge tries a chain of mirror URLs with `{gid}` substituted into the path. The first server that returns HTTP 200 wins; the response body is written into Steam's internal buffer as if the original call succeeded. Trusted host checking prevents redirects to unexpected domains.

Default URL chain (HTTPS first, HTTP as last resort):
1. `https://manifest.opensteamtool.com/{gid}`
2. `https://manifest.steam.run/api/manifest/{gid}`
3. `http://gmrc.wudrm.com/manifest/{gid}`

---

### KVHooks (`hooks/client/KeyValues.cpp`)

Hooks `ReadAsBinary` and `FindOrCreateKey` on Steam's internal KeyValues tree.

These serve as anchor points for the online-fix pipeline and depot key resolution. `ReadAsBinary` intercepts reads of depot manifest data and allows the manifest bridge to inject fetched content. `FindOrCreateKey` is an install/uninstall logging anchor.

---

### SteamCapture (`hooks/capture/RuntimeCapture.cpp`)

Uses VEH one-shot int3 captures (not Detours hooks) to resolve internal Steam object pointers at runtime.

This module arms single-byte breakpoints at the entry of several Steam functions.  When each fires for the first time, the VEH handler records `RCX` (the `this` pointer) into a module-level variable, then restores the original byte and resumes execution normally.  The captured pointers are:

| Function | Captured into |
|---|---|
| `GetAppIDForCurrentPipe` | `g_steamEngine` |
| `GetAppDataFromAppInfo` | `g_pCAppInfoCache` |
| `MarkLicenseAsChanged` | `g_pCUser` |
| `GetPackageInfo` | `g_pCPackageInfo` |

`ProcessPendingLicenseUpdates`, `CUtlBufferEnsureCapacity`, and `CUtlMemoryGrow` are resolved without int3 (address-only).

`SteamCapture::NotifyLicenseChanged` uses the captured `g_pCUser` and resolved function pointers to push new ownership records into Steam's in-memory license tables and trigger an ownership refresh without restarting Steam.

---

### PacketRouter (`hooks/PacketRouter.cpp`)

Hooks `BBuildAndAsyncSendFrame` and `RecvPkt`.

Steam communicates with the Steam Network (CM servers) using a protobuf-over-TCP framing.  PacketRouter intercepts outgoing and incoming packet frames and replaces the content of specific message types:

- `FamilyGroupsClient.NotifyRunningApps` — replaces the running-app list so family-sharing session checks on the CM side see the correct owner rather than the borrower account.
- `Player.GetUserStats` — rewrites the SteamID in the stats request so achievements are loaded from the account configured in the Lua `setStat` call.

Packet replacement uses a fixed-size ring-buffer pool to avoid heap allocation on the hot path.

Lua interface:

```lua
setStat(1234567, "76561198028121353")  -- load stats from this SteamID for app 1234567
```

If no `setStat` is provided for an app, the fallback SteamID defined in `entry.h` (`ONLINE_FIX_APP_ID`) is used.

---

### PackagePatch (`hooks/PackagePatch.cpp`)

Hooks `LoadPackage`, `CheckAppOwnership`, and `SendCallbackToPipe`.

- **`LoadPackage`** — intercepts the call for Package 0 (the free-to-play base package) and appends all app IDs from the active Lua config to its `AppIdVec`, so Steam considers them part of the base license.
- **`CheckAppOwnership`** — patches the returned `CAppOwnershipInfo` struct for apps present in the Lua config so they show as owned, released, and playable.  If the app is genuinely owned it is marked as such and excluded from future patching.
- **`SendCallbackToPipe`** — intercepts `AppLicensesChanged` callbacks and forces `m_bReloadAll = true` so Steam fully refreshes its license state after an ownership injection.

---

### LicenseHooks (`hooks/LicenseHooks.cpp`)

Detours `OptedInMask` and `RequiresLegacyCDKey` against `steamclient64.dll`.

- **`OptedInMask`** — when the OnlineFix CGameID rewrite is in flight, the controller layer asks for appid 480 (Spacewar) and gets the empty mask back. The detour swaps the query back to the real appid so controllers stay live under `-onlinefix`.
- **`RequiresLegacyCDKey`** — Steam asks the wrapper for a CD key on a small set of pre-2010 titles when ownership crosses certain code paths. For Lua-tracked appids the user has no real key, so the detour answers `false` and the prompt never fires. Without this hook those games refuse to launch.

DLC ownership / install / cloud / license-update / subscribed-app / ownership-ticket queries (`BIsDlcEnabled`, `IsAppDlcInstalled`, `IsCloudEnabledForApp`, `BUpdateLicenses`, `GetSubscribedApps`, `BUpdateAppOwnershipTicket`) are intentionally not detoured here. Steam already returns the right answer for Lua-tracked appids through the existing `CheckAppOwnership` patch, so detouring those is redundant and risks stack corruption on x64 fastcall when an argument count or type is even slightly off. The patterns for those six still ride in the per-build TOML, so future code that needs their addresses can resolve them without changing the publisher or the cache layout.

---

### RuntimeCapture (`hooks/capture/RuntimeCapture.cpp`)

VEH-based captures and hooks used by the `-onlinefix` game-launch path.

- Arms a one-shot int3 on `CUser_SpawnProcess`.  When Steam is about to launch a game, the VEH fires, checks whether `-onlinefix` is present in the launch command, and if so records the real app ID for the session.  The original byte is restored and execution continues.
- Hooks `BuildSpawnEnvBlock` (via string XRef, since this function is only called at launch — not at startup — making string-based resolution safe here) to patch `SteamOverlayGameId` and `SteamAppId` environment variables so overlays and stats bind to the correct app.
- Uses `GetAppDataFromAppInfo` captures from `SteamCapture` to resolve game names for rich-presence labelling.

---

### RichPresence (`hooks/client/RichPresence.cpp`)

Patches `CMsgClientPersonaState` protobuf messages intercepted by PacketRouter.

When an online-fix game is running, Steam's presence broadcasts the SpaceWar app ID (480) rather than the real game ID.  `RichPresence::HandleRecv` rewrites the `game_played_app_id` field to the real app ID resolved by RuntimeCapture, so friends see the correct game name in their friend list.

---

### StringFind (`patterns/StringFind.cpp`)

Implements the string cross-reference search used by the `_STR_D` hook macros.  Scans the `.rdata` section of a module for a target string, finds all code locations that reference it via RIP-relative `LEA`/`MOV` instructions, locates the enclosing function via `.pdata` RUNTIME_FUNCTION lookup, and returns the function entry point.

This is more update-proof for functions called only at game-launch time.  It is intentionally **not** used for hooks that fire during early Steam startup (e.g. `IPCBus`) — those resolve through the runtime pattern fetcher only, since the rva pin plus byte verification rules out the risk of the string residing in a helper function and resolving to the wrong address.

---

## Lua configuration format

SteaMidra writes `.lua` files to `Steam\config\stplug-in\<appid>.lua`.  LumaCore watches this directory and reloads files as they change.

### App and depot registration

```lua
addappid(1234567)
addappid(1001, 1, "0A1B2C3D4E5F6071820394A5B6C7D8E9")
```

`addappid(appId)` — registers ownership of appId without a depot key.
`addappid(depotId, 1, "hexkey")` — registers ownership and provides the AES-128 decryption key for depotId.

```lua
addtoken(1234567, 12345678901234567890)
```

`addtoken(appId, accessToken)` — registers a package access token for appId used during license validation.

### Manifest pinning

```lua
setManifestid(1001, "1234567890123456789")
```

Pins the manifest GID for depot 1001. LumaCore reports this GID when Steam asks for the active manifest.

### App tickets and etickets

```lua
setAppticket(1234567, "base64encodedticketdata")
setEticket(1234567, "base64encodedeticketdata")
```

Inject pre-built AppTicket and EncryptedAppTicket blobs for appId. These flow through the credential store and are served by the IPC ticket handlers. Required for Denuvo-protected games.

### Eticket URL configuration

```lua
seteticketurl("https://example.com/api/eticket/{appid}")
```

Sets the URL template for on-demand eticket minting. `{appid}` is replaced with the requesting app's ID. The fetcher issues an HTTP GET and writes the returned blob into the credential store for Denuvo auth.

### Denuvo auth controls

```lua
forcedenuvo(1234567)
```

Forces Denuvo authorization for appId even when ProtectionScan misses the packer signature. Use when a game crashes with Denuvo error 012.

```lua
addprocess("game.exe", 1234567)
```

Maps a process name to an appId for match-by-exe when the process environment block doesn't contain a SteamAppId variable.

### Stats and achievements

```lua
setStat(1234567, "76561198028121353")
```

Instructs PacketRouter to load achievement and stats data from the given SteamID for app 1234567.

### Manifest and key fetching

```lua
fetchManifestCode("1234567890123456789")
fetchManifestCodeEx("1234567890123456789", "base64data")
```

Fetches depot manifest content from the configured HTTP bridge URLs.

```lua
getCachedAppTicket(1234567)
getDecryptionKey(1234567)
```

Reads cached app ticket and depot decryption key from the Windows registry credential store.

### HTTP helpers

```lua
lcHttpGet("https://example.com/api/data")
lcHttpPost("https://example.com/api/submit", "payload")
```

General-purpose HTTP GET and POST from within Lua scripts. Host-gated to a hardcoded allowlist to prevent data exfiltration by malicious scripts.

---

## Configuration file (`lumacore.toml`)

Placed in the Steam installation directory.  SteaMidra writes this file during LumaCore setup.

```toml
[user]
steam_id = "76561198028121353"  # SteamID64 to spoof in GetSteamID responses
```

All other settings use built-in defaults.

---

## Pattern maintenance

When a Steam client update lands and the new SHA-256 isn't in the pattern repo yet, generate a fresh `<sha>.toml` with the analyzer:

```
cd cleintcheck
python steamclient_analyzer.py "C:\Program Files (x86)\Steam\steamclient64.dll" \
       --steamui "C:\Program Files (x86)\Steam\steamui.dll" \
       --emit toml --out-dir PatternsUpdate
```

Two files land under `PatternsUpdate\steamclient\` and `PatternsUpdate\steamui\` — upload both to the pattern repo, and LumaCore picks them up on the next launch (or immediately if the user drops the files into `<Steam>\lumacore\pattern\` themselves).

See the **Pattern resolution** section above for the full details on the canonical-overlay merge, the FNV-1a section keys, and the source-priority chain.

---

## Logging

Logging is compiled in only for Debug builds (`LUMACORE_LOGGING_ENABLED` define).  Release builds compile all `LOG_*` macros to no-ops so there is no runtime overhead.

When enabled, logs are written to `Steam\lumacore\` alongside `LumaCore.dll`.  Each module writes to its own file:

| File | Module |
|---|---|
| `main.log` | Core init, Lua parsing, DLL loading, hook install events |
| `corein.log` | Bootstrap pipeline — build ID, diversion load, pattern priming |
| `ipc.log` | IPCBus + IpcDispatch — IPC handler registration and dispatch |
| `ipcrtr.log` | IPC router internal trace — per-packet command/pipe/interface logging |
| `usrcmd.log` | CmdUser — GetSteamID, ticket, and achievement callback handling |
| `package.log` | PackagePatch — CheckAppOwnership, LoadPackage, NotifyLicenseChanged |
| `license.log` | LicenseHooks — OptedInMask, RequiresLegacyCDKey, ConfigStoreGetBinary |
| `decryptionkey.log` | DecryptionKeyHook — license decryption config interception |
| `auth.log` | DenuvoAuth — authorization window state, SteamID persistence |
| `eticket.log` | EticketFetcher — HTTP eticket minting calls |
| `manifest.log` | ManifestFetch — manifest download bridge HTTP steps |
| `manbnd.log` | ManifestBind — BuildDepotDependency hook events |
| `onlinefix.log` | OnlineFixInject — CreateProcess hook, payload injection events |
| `netpacket.log` | PacketRouter + handlers — protobuf frame interception and rewrite |
| `pktrt.log` | PacketRouter internal trace |
| `keyvalue.log` | KVHooks — ReadAsBinary / FindOrCreateKey hook events |
| `steamui.log` | SteamUI — MarkAppChange, RunFrame drain, library removal batching |
| `achievement.log` | Achievement callback diagnostics |
| `misc.log` | Miscellaneous — pattern fetcher cache/network steps, VEH captures |
| `status.json` | Machine-readable snapshot: build id, per-DLL TOML status, hooks installed / missed |

The `pattern\` subdirectory next to these logs holds the cached `<sha>.toml` files the runtime fetcher uses. Files there are safe to delete; they get re-fetched on next launch.

Log level is controlled by `lumacore.toml` under `[log] level = "debug"` (default: `info`).
