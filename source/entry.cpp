// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "entry.h"
#include "hooks/CoreLoader.h"
#include "hooks/PackagePatch.h"
#include "hooks/PatternFetcher.h"
#include "utils/DirWatch.h"
#include "utils/Diagnostics.h"
#include "utils/HookStatus.h"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// Latest-known cache-key SHA per module. HookStatus::SetShas takes the pair
// at once, but the steamclient and steamui legs of the pattern fetch can
// finish independently (the steamclient leg lands inline in InitThread,
// the steamui leg defers to LoadModuleWithPath when steamui.dll is not yet
// mapped at InitThread time). The helper threads each leg's update through
// SetShas with the most recent pair so the on-disk Status File never
// regresses an already-known SHA when only one module has just refreshed.
static std::mutex   g_shaMu;
static std::string  g_currentSteamclientSha;
static std::string  g_currentSteamuiSha;

static void PublishSha(std::string_view moduleName, std::string sha) {
    std::lock_guard<std::mutex> lk(g_shaMu);
    if (moduleName == "steamclient") {
        g_currentSteamclientSha = std::move(sha);
    } else if (moduleName == "steamui") {
        g_currentSteamuiSha = std::move(sha);
    }
    HookStatus::SetShas(g_currentSteamclientSha, g_currentSteamuiSha);
}

// Set when the steamui leg has been resolved (either inline in InitThread
// when steamui.dll was already mapped, or via LoadModuleWithPath when
// Steam's loader maps it later). Prevents the deferred-dispatch handler
// in SteamUI::LoadModuleWithPath from running the fetch twice.
std::atomic<bool> g_steamUiPatternDispatched{false};

// Fetches the steamui.dll TOML synchronously the moment Steam's loader
// maps the module. Called from SteamUI::LoadModuleWithPath the first time
// the loader resolves the module when InitThread had to skip the steamui
// leg because the module wasn't mapped yet. The call site already sits
// outside the loader lock so blocking on a network fetch is fine here.
//
// Hook installs against steamui happen in SteamUI::CoreHook(), which fires
// at the end of InitThread. By the time LoadModuleWithPath runs the very
// first time, CoreHook has already passed once. We re-run any steamui-only
// installer logic here? No, the design intentionally keeps SteamUI::CoreHook
// gated on steamui being mapped at InitThread time. The deferred case
// (steamui mapped late) still gets the TOML cached so the next session
// has it primed; current session's steamui hooks may miss but Steam is
// alive and the cache lands for next launch.
void DispatchSteamUiPatternFetch() {
    bool expected = false;
    if (!g_steamUiPatternDispatched.compare_exchange_strong(expected, true))
        return;
    HMODULE ui = GetModuleHandleA("steamui.dll");
    if (!ui) return;
    auto r = PatternFetcher::LoadFor(ui, "steamui");
    LOG_INFO("PatternFetcher: steamui (deferred) sha={} entries={} ok={}",
             r.sha.empty() ? "<unknown>" : r.sha,
             static_cast<unsigned>(r.entries.size()),
             r.ok ? 1 : 0);
    PublishSha("steamui", r.sha);
    HookStatus::SetTomlAvailability("steamui", r.ok);
}

// Prepares the runtime paths and loads the hooked copy of steamclient64.dll.
//
// The diversion pattern: instead of hooking the real steamclient64.dll directly,
// LumaCore copies it to bin\lcoverlay.dll and loads that copy. The SteamUI hook then
// intercepts steamui.dll's LoadModuleWithPath("steamclient64.dll") call and returns
// diversion_hModule, so Steam's UI layer ends up using the hooked copy transparently.
//
// CopyFileA is retried up to 30 times (3 seconds total) because steamclient64.dll can be
// briefly locked by the Steam service during early startup. Same retry logic for LoadLibraryA.
// Returns false if either operation fails after all retries.
bool LoadDiversion()
{
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&LoadDiversion), &hSelf);
    if (!GetModuleFileNameA(hSelf, SteamInstallPath, MAX_PATH))
        return false;
    char* lastSlash = strrchr(SteamInstallPath, '\\');
    if (lastSlash) *lastSlash = '\0';

    sprintf_s(SteamclientPath, MAX_PATH, "%s\\steamclient64.dll",   SteamInstallPath);
    sprintf_s(DiversionPath,   MAX_PATH, "%s\\bin\\lcoverlay.dll",  SteamInstallPath);
    sprintf_s(LuaDir,          MAX_PATH, "%s\\config\\stplug-in", SteamInstallPath);
    sprintf_s(ConfigPath,      MAX_PATH, "%s\\lumacore.toml",      SteamInstallPath);
    // ensure bin\ directory exists before copying
    char binDir[MAX_PATH];
    sprintf_s(binDir, MAX_PATH, "%s\\bin", SteamInstallPath);
    CreateDirectoryA(binDir, nullptr);  // no-op if already exists
    // Retry: steamclient64.dll may be briefly locked during Steam startup
    {
        int attempts = 0;
        while (!CopyFileA(SteamclientPath, DiversionPath, FALSE)) {
            if (++attempts >= 30) {
                LOG_ERROR("CopyFileA failed after 30 attempts: {} -> {}", SteamclientPath, DiversionPath);
                return false;
            }
            LOG_WARN("CopyFileA attempt {}/30 failed (err={}), retrying...", attempts, GetLastError());
            Sleep(100);
        }
    }
    {
        int attempts = 0;
        while (!(diversion_hModule = LoadLibraryA(DiversionPath))) {
            if (++attempts >= 30) {
                LOG_ERROR("LoadLibraryA failed after 30 attempts: {}", DiversionPath);
                return false;
            }
            LOG_WARN("LoadLibraryA attempt {}/30 failed (err={}), retrying...", attempts, GetLastError());
            Sleep(100);
        }
    }
    LOG_INFO("LumaCore: loaded lcoverlay.dll from {}", DiversionPath);
    return true;
}

// Reads the current Steam build number from steam.exe and stores it as a string in g_steamBuildId.
// Steam exports GetBootstrapperVersion from steam.exe, which returns the build number as an int64.
// Converting it to a string gives us the label format used by the analyzer (e.g. "1779918128").
// If steam.exe is not yet loaded or doesn't export this function, g_steamBuildId stays empty
// and the banner reports "unknown" build id.
static void DetectSteamBuildId() {
    using GetBootstrapperVersion_t = int64_t (*)();
    HMODULE hSteam = GetModuleHandleA("steam.exe");
    if (!hSteam) {
        LOG_WARN("SteamVersion: steam.exe module not loaded; build id unavailable");
        return;
    }
    auto fn = reinterpret_cast<GetBootstrapperVersion_t>(
        GetProcAddress(hSteam, "GetBootstrapperVersion"));
    if (!fn) {
        LOG_WARN("SteamVersion: steam.exe!GetBootstrapperVersion not exported; "
                 "build id unavailable");
        return;
    }
    g_steamBuildId = std::to_string(fn());
    LOG_INFO("SteamVersion: build id = {}", g_steamBuildId);
}

// Worker thread that runs all real startup work outside of DllMain.
// Windows holds the loader lock during DllMain, which means calling LoadLibrary, doing
// file I/O, or installing Detours hooks from DllMain risks a deadlock. Spinning up a
// separate thread lets us do all of that safely once the loader lock is released.
//
// Pattern fetch policy: each LC_ATTACH below resolves through PatternFetcher::Get,
// which means the per-module entry map MUST be populated before the install pass
// runs. PatternFetcher::LoadFor does cache-first read, falls back to network on
// cache miss, writes the body to <Steam>\lumacore\pattern\<sha>.toml on success,
// and installs the parsed entries — all synchronously. We block InitThread on
// it so the very first session after a fresh install actually picks up the TOML
// and lands all hooks instead of racing the install pass against a detached
// network worker. The Steam loader thread is not waiting on us; only the
// LumaCore init worker.
static DWORD WINAPI InitThread(LPVOID param) {
    HMODULE selfModule = static_cast<HMODULE>(param);
    Logger::Init(selfModule);
    LOG_INFO("LumaCore init thread started (build " __DATE__ " " __TIME__ ")");

    // Build id first so HookStatus has a value to surface even if the
    // diversion copy below fails. A bare status.json with the build id is
    // still useful to SteaMidra's banner.
    DetectSteamBuildId();
    HookStatus::SetBuildId(g_steamBuildId);

    if (!LoadDiversion()) {
        LOG_ERROR("LoadDiversion failed");
        // Surface the absence so the banner explains itself instead of
        // silently going stale. SteaMidra reads the file each tick.
        HookStatus::SetTomlAvailability("steamclient", false);
        HookStatus::SetTomlAvailability("steamui", false);
        HookStatus::WriteToDisk();
        return 1;
    }

    Settings::Load(ConfigPath);
    Logger::InitModules();

    // ── Steamclient leg: synchronous cache + network ─────────────────────────
    // Block until either the cached TOML for this build is installed or the
    // network fetcher landed a fresh one. If both fail (offline + no cache),
    // r.ok is false and the install pass below records every hook as missed
    // without crashing — Steam stays alive, banner explains the situation.
    PatternFetcher::PatternResult pcResult =
        PatternFetcher::LoadFor(diversion_hModule, "steamclient");
    LOG_INFO("PatternFetcher: steamclient sha={} entries={} ok={}",
             pcResult.sha.empty() ? "<unknown>" : pcResult.sha,
             static_cast<unsigned>(pcResult.entries.size()),
             pcResult.ok ? 1 : 0);

    // ── Steamui leg ──────────────────────────────────────────────────────────
    // Same synchronous load when steamui.dll is already mapped. When the
    // loader has not mapped it yet, defer to SteamUI::LoadModuleWithPath
    // (the diversion-loader hook) — that fires the moment Steam pulls
    // steamui in, and DispatchSteamUiPatternFetch runs the same LoadFor
    // path on that thread (still outside the loader lock).
    PatternFetcher::PatternResult puResult{};
    bool steamUiMapped = (GetModuleHandleA("steamui.dll") != nullptr);
    if (steamUiMapped) {
        bool expected = false;
        if (g_steamUiPatternDispatched.compare_exchange_strong(expected, true)) {
            puResult = PatternFetcher::LoadFor(
                GetModuleHandleA("steamui.dll"), "steamui");
            LOG_INFO("PatternFetcher: steamui sha={} entries={} ok={}",
                     puResult.sha.empty() ? "<unknown>" : puResult.sha,
                     static_cast<unsigned>(puResult.entries.size()),
                     puResult.ok ? 1 : 0);
        }
    } else {
        LOG_INFO("PatternFetcher: steamui.dll not yet mapped; deferring fetch "
                 "to first LoadModuleWithPath callback");
    }

    // SHAs first, then per-module availability, then the initial publish.
    // Empty SHAs round-trip as empty strings per the contract.
    {
        std::lock_guard<std::mutex> lk(g_shaMu);
        g_currentSteamclientSha = pcResult.sha;
        g_currentSteamuiSha     = puResult.sha;
    }
    HookStatus::SetShas(pcResult.sha, puResult.sha);
    HookStatus::SetTomlAvailability("steamclient", pcResult.ok);
    HookStatus::SetTomlAvailability("steamui",     puResult.ok);
    HookStatus::WriteToDisk();

    // ── SteamUI::CoreHook() must be early to catch LoadModuleWithPath ────────
    // But AFTER Logger::InitModules() so module loggers are available.
    SteamUI::CoreHook();

    std::vector<std::string> watchDirs = Settings::luaPaths;
    watchDirs.push_back(std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaLoader::ParseDirectory(dir);

    DirWatch::Start(watchDirs);

    // LC_ATTACH macro chain. Each macro re-publishes through the HookStatus
    // mutator path (RecordInstalled / RecordMissed), so the banner reflects
    // every miss without an extra WriteToDisk call here. Returns cleanly on
    // every TOML-absent branch: Steam stays alive even if the pattern repo
    // has not yet shipped a TOML for this build.
    PackagePatch::Install();
    LumaCore::Attach();
    g_HooksInstalled.store(true);
    HookStatus::WriteToDisk();
    LOG_INFO("LumaCore init complete");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Pin the module so a stray FreeLibrary cannot unmap LumaCore while
        // hooks and worker threads are still live. Failure is non-fatal; we
        // just lose the unmap protection and continue attach.
        HMODULE selfPin = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCSTR>(&DllMain), &selfPin)) {
            LOG_MISC_WARN("DllMain: module pin failed (err={}), continuing without pin",
                          GetLastError());
        }
        // Start InitThread to do all real work outside the loader lock.
        // DllMain must return quickly and must not call LoadLibrary, open files,
        // or install hooks - doing so under the loader lock causes deadlocks.
        g_InitThread = CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
#ifdef LUMACORE_DIAGNOSTICS_ENABLED
        // A16 belt-and-suspenders: flush the achievement diagnostic ring
        // first thing on DLL detach so a crash inside CoreLoader::Detach
        // never loses the captured events. Defensive write-and-return.
        Diagnostics::DumpForDetach();
#endif
        if (g_InitThread) {
            WaitForSingleObject(g_InitThread, 5000);
            CloseHandle(g_InitThread);
            g_InitThread = nullptr;
        }
        if (g_HooksInstalled.load()) {
            DirWatch::Stop();
            if (pvReserved == nullptr) {
                // Graceful FreeLibrary path. The module pin in
                // DLL_PROCESS_ATTACH makes this unreachable in practice, but
                // we still keep the hook teardown for the defensive case.
                SteamUI::CoreUnhook();
                LumaCore::Detach();
            }
            // pvReserved != nullptr is process termination: the loader lock
            // is held and MinHook teardown can deadlock under it. Skip
            // MH_DisableHook, FreeLibrary, and LoadLibrary; the OS reclaims
            // the trampolines as the address space goes away.
        }
    }

    return TRUE;
}
