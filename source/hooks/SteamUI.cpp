// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "SteamUI.h"
#include "CoreLoader.h"
#include "Macros.h"
#include "SigTypes.h"
#include "utils/ByteScan.h"
#include "utils/HookStatus.h"
#include "utils/LuaLoader.h"
#include "steam_messages.pb.h"

#include <psapi.h>

#include <chrono>
#include <thread>
#include <vector>

namespace {
    using namespace std::chrono_literals;
    constexpr int  MAX_RETRY      = 20;
    constexpr auto RETRY_INTERVAL = 300ms;

    // ▌ STEAMUI ▌ function type aliases
    using AddProtobufAsBinary_t = void*(__fastcall*)(void* /*args*/, void* /*proto*/);
    using GetAppByID_t          = void*(__fastcall*)(void* /*controller*/, AppId_t, bool /*create*/);
    using GetTopManager_t       = void*(__fastcall*)();

    // ▌ STEAMUI ▌ resolved function pointers
    inline AddProtobufAsBinary_t oAddProtobufAsBinary = nullptr;
    inline GetAppByID_t          oGetAppByID          = nullptr;
    inline GetTopManager_t       oGetTopManager       = nullptr;

    // CSteamUIAppController offsets (see its Validate() method):
    //   +0xAB8 from top-manager -> CSteamUIAppController*
    //   +1744  m_vecAppOverviewChanged (subscriber vec data ptr)
    //   +1760  m_vecAppOverviewChanged size
    constexpr size_t kControllerInTopManager     = 0xAB8;
    constexpr size_t kSubscriberVecOffset        = 1744;
    constexpr size_t kSubscriberVecSizeOffset    = 1760;

    constexpr size_t kArgsSize                   = 64;
    constexpr size_t kSubscriberInvokeVtableSlot = 4;

    // Cleared so BuildCompleteAppOverviewChange's filter (BIsOwned via
    // vtable[22]) also excludes the app on the next full snapshot.
    constexpr size_t kCSteamAppOwnedFlagOffset   = 28;

    // TOML lookup-key candidates for AddProtobufAsBinary. The .xref field
    // carries the historical string-xref anchor that used to identify the
    // function before the TOML-only refactor; it stays as documentation so
    // the analyzer side can still find it. Lookup runs over .name only.
    static constexpr StringXRefSig AddProtobufAsBinaryStrSigs[] = {
        { "AddProtobufAsBinary", "CJSMethodArgs::AddProtobufAsBinary" },
    };

    // ▌ STEAMUI ▌ LoadModuleWithPath hook
    LC_HOOK_DEF(LoadModuleWithPath, HMODULE, const char* path, bool flags) {
        LOG_STEAMUICH_INFO("LoadModuleWithPath called with path: {} , flags: {}", path, flags);
        // First steamui-mapped callback also primes the pattern fetcher worker
        // when the loader had not mapped steamui.dll at InitThread dispatch.
        DispatchSteamUiPatternFetch();
        // Wait for steamclient hooks to be installed before redirecting.
        for (int idx = 0; idx < MAX_RETRY && !g_HooksInstalled.load(); ++idx) {
            LOG_STEAMUICH_DEBUG("LoadModuleWithPath: waiting for hooks... (attempt {}/{})", idx + 1, MAX_RETRY);
            std::this_thread::sleep_for(RETRY_INTERVAL);
        }
        HMODULE h = oLoadModuleWithPath(path, flags);
        if (!strcmp(path, "steamclient64.dll"))
            h = diversion_hModule;
        return h;
    }

    // ▌ STEAMUI ▌ GetTopManager
    // The pattern publisher schema points GetTopManager directly at the
    // 2-instruction getter (mov rax, [rip+disp]; ret), so the resolved
    // address IS the function pointer. No anchor decode, no rel32 walk.

    // Fetch the CSteamUIAppController via the captured getter.
    void* ResolveController() {
        if (!oGetTopManager) return nullptr;
        void* topMgr = oGetTopManager();
        if (!topMgr) return nullptr;
        return *reinterpret_cast<void**>(static_cast<uint8_t*>(topMgr) + kControllerInTopManager);
    }

    // Synthesize a CAppOverview_Change proto with removed_appid=[appId] and
    // dispatch to every registered webhelper subscriber.
    bool EmitRemovedAppIds(void* pController, const AppId_t* ids, size_t count);

    bool EmitRemovedAppId(void* pController, AppId_t appId) {
        return EmitRemovedAppIds(pController, &appId, 1);
    }

    bool EmitRemovedAppIds(void* pController, const AppId_t* ids, size_t count) {
        if (!ids || count == 0) return false;
        alignas(8) uint8_t argsBuf[kArgsSize] = {};

        ::CAppOverview_Change msg;
        for (size_t idx = 0; idx < count; ++idx) msg.add_removed_appid(ids[idx]);
        msg.set_update_complete(true);
        oAddProtobufAsBinary(argsBuf, &msg);

        void** vecData = *reinterpret_cast<void***>(
            static_cast<uint8_t*>(pController) + kSubscriberVecOffset);
        uint32_t subCount = *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(pController) + kSubscriberVecSizeOffset);

        if (!vecData || subCount == 0) {
            LOG_STEAMUICH_WARN("EmitRemovedAppIds: no subscribers; count={}", count);
            return false;
        }

        for (uint32_t idx = 0; idx < subCount; ++idx) {
            void* subscriber = vecData[idx];
            if (!subscriber) continue;
            void** vtable = *reinterpret_cast<void***>(subscriber);
            auto invoke = reinterpret_cast<void(__fastcall*)(void*, void*)>(
                vtable[kSubscriberInvokeVtableSlot]);
            invoke(subscriber, argsBuf);
        }

        return true;
    }

} // anonymous namespace

namespace SteamUI {

    // The LC_* macros target diversion_hModule (steamclient64.dll). SteamUI
    // hooks live in steamui.dll, so the install path resolves through
    // ByteSearch(hSteamUI, ...) directly and runs the Detours attach plus
    // HookStatus reporting by hand. The shape mirrors what LC_ATTACH_D and
    // LC_RESOLVE_D expand to, just with a different module handle.
    void CoreHook() {
        HMODULE hSteamUI = GetModuleHandleA("steamui.dll");
        if (!hSteamUI) {
            LOG_STEAMUICH_WARN("steamui.dll not loaded; SteamUI hooks disabled");
            return;
        }

        LC_TX_OPEN();
        {
            void* _p_ = ByteSearch(hSteamUI, "LoadModuleWithPath");
            if (_p_) {
                LOG_STEAMUICH_DEBUG("Hook: LoadModuleWithPath attached @ 0x{:X}",
                                    reinterpret_cast<uintptr_t>(_p_));
                oLoadModuleWithPath = reinterpret_cast<LoadModuleWithPath_t>(_p_);
                DetourAttach(reinterpret_cast<PVOID*>(&oLoadModuleWithPath),
                             reinterpret_cast<PVOID>(hkLoadModuleWithPath));
                HookStatus::RecordInstalled();
            } else {
                LOG_STEAMUICH_WARN("Hook: LoadModuleWithPath skipped (TOML entry missing for steamui)");
                HookStatus::RecordMissed("LoadModuleWithPath");
            }
        }
        LC_TX_COMMIT();

        // Helper resolves (no Detours attach, just bind the trampoline slot).
        {
            void* _p_ = ByteSearch(hSteamUI, "GetAppByID");
            oGetAppByID = reinterpret_cast<GetAppByID_t>(_p_);
            if (_p_) {
                LOG_STEAMUICH_DEBUG("Resolve: GetAppByID bound @ 0x{:X}",
                                    reinterpret_cast<uintptr_t>(_p_));
                HookStatus::RecordInstalled();
            } else {
                LOG_STEAMUICH_WARN("Resolve: GetAppByID skipped (TOML entry missing for steamui)");
                HookStatus::RecordMissed("GetAppByID");
            }
        }

        // AddProtobufAsBinary: walk the StringXRefSig array and use the first
        // candidate name that resolves through the steamui TOML. Each .name
        // is a TOML lookup key; .xref is documentation only.
        {
            void* _p_ = nullptr;
            const char* _matched_ = nullptr;
            for (const auto& _s_ : AddProtobufAsBinaryStrSigs) {
                _p_ = ByteSearch(hSteamUI, _s_.name);
                if (_p_) { _matched_ = _s_.name; break; }
            }
            oAddProtobufAsBinary = reinterpret_cast<AddProtobufAsBinary_t>(_p_);
            if (_p_) {
                LOG_STEAMUICH_DEBUG("Resolve: AddProtobufAsBinary bound via TOML key \"{}\" @ 0x{:X}",
                                    _matched_, reinterpret_cast<uintptr_t>(_p_));
                HookStatus::RecordInstalled();
            } else {
                LOG_STEAMUICH_WARN("Resolve: AddProtobufAsBinary skipped (no TOML key in array resolved)");
                HookStatus::RecordMissed("AddProtobufAsBinary");
            }
        }

        // GetTopManager: per the canonical pattern publisher schema, rva
        // points directly at the 2-instruction getter
        // (mov rax, [rip+disp]; ret). No rel32 decode needed; the resolved
        // address IS the function pointer we call.
        {
            void* _p_ = ByteSearch(hSteamUI, "GetTopManager");
            oGetTopManager = reinterpret_cast<GetTopManager_t>(_p_);
            if (_p_) {
                LOG_STEAMUICH_DEBUG("Resolve: GetTopManager bound @ 0x{:X}",
                                    reinterpret_cast<uintptr_t>(_p_));
                HookStatus::RecordInstalled();
            } else {
                LOG_STEAMUICH_WARN("Resolve: GetTopManager skipped (TOML entry missing for steamui)");
                HookStatus::RecordMissed("GetTopManager");
            }
        }

        LOG_STEAMUICH_INFO("Install: GetAppByID={}, AddProtobufAsBinary={}, GetTopManager={}",
                         reinterpret_cast<void*>(oGetAppByID),
                         reinterpret_cast<void*>(oAddProtobufAsBinary),
                         reinterpret_cast<void*>(oGetTopManager));
    }

    void CoreUnhook() {
        LC_TX_OPEN();
        LC_DETACH(LoadModuleWithPath);
        LC_TX_COMMIT();

        oAddProtobufAsBinary = nullptr;
        oGetAppByID          = nullptr;
        oGetTopManager       = nullptr;
    }

    void RemoveAppOverview(AppId_t appId) {
        if (!oAddProtobufAsBinary || !oGetTopManager || !oGetAppByID) {
            LOG_STEAMUICH_WARN("RemoveAppOverview: primitives unresolved; appId={}", appId);
            return;
        }

        // Skip eviction for genuinely owned apps. CheckAppOwnership has
        // already marked them via MarkOwned and the dual-account refresh
        // path was tearing them out of the library card view because
        // every appid in the package vector got the eviction treatment.
        // This keeps the legitimate library entry alive while still
        // dropping the fake-owned cards on hot-reload.
        if (LuaLoader::IsOwned(appId)) {
            LOG_STEAMUICH_DEBUG("RemoveAppOverview: appId={} is owned; skipping eviction", appId);
            return;
        }

        void* pController = ResolveController();
        if (!pController) {
            LOG_STEAMUICH_WARN("RemoveAppOverview: controller singleton not initialized; appId={}", appId);
            return;
        }

        // Clear the host-side CSteamApp owned flag if a CSteamApp exists for
        // this id. Sub-depots (e.g. HL1's 221-234) won't have one, so just
        // skip the flag-clear and still emit the removal so any stale
        // subscriber state for the id gets cleaned up.
        if (void* pApp = oGetAppByID(pController, appId, /*create=*/false)) {
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pApp) + kCSteamAppOwnedFlagOffset) &= ~1u;
        }

        if (!EmitRemovedAppId(pController, appId)) return;

        LOG_STEAMUICH_INFO("RemoveAppOverview: appId={} done", appId);
    }

    // Kept for API stability; only used when callers explicitly want a
    // multi-id dispatch. The live NotifyLicenseChanged path uses per-id
    // RemoveAppOverview because Steam's webhelper handler crashes on
    // multi-id CAppOverview_Change bursts in some build/load combos.
    void RemoveAppOverviewBatch(const AppId_t* ids, size_t count) {
        if (!ids || count == 0) return;
        for (size_t idx = 0; idx < count; ++idx) {
            RemoveAppOverview(ids[idx]);
        }
    }

} // namespace SteamUI

