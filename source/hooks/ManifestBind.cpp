// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "ManifestBind.h"
#include "Macros.h"
#include "entry.h"
#include <format>
#include <string>

// ▌▌ LumaCore ▌ MANIFEST ▌ Manifest override hook
//  BuildDepotDependency patches depot entries' gid/size directly in the
//  output vector after Steam builds the depot list.
//
//  DLC observation hooks (BIsDlcEnabled / IsAppDlcInstalled /
//  IsCloudEnabledForApp) were intentionally NOT hooked. Steam already
//  returns the right answer for Lua-tracked appids through the existing
//  CheckAppOwnership patch, so adding those would be redundant and would
//  re-introduce wrong-target risk on builds where their TOML rva drifts.
// ▌▌
namespace {

    // ▌ MANIFEST ▌ helper

    std::string DepotStr(const DepotEntry& e) {
        return std::format("[DepotId={} | AppId={} | Gid={} | Size={} | Dlc={} | Lcs={} | Carry={} | Shared={}]",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    // ▌ MANIFEST ▌ BuildDepotDependency hook
    // After Steam builds the depot list for an app, patch ManifestGid
    // and ManifestSize for any depots we have overrides for.

    LC_HOOK_DEF(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool outcome = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFESTCH_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, outcome, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFESTCH_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            const DepotEntry* dBase = pDepotInfo->m_Memory.m_pMemory;
            for (uint32 n = 0; n < pDepotInfo->m_Size; ++n)
                LOG_MANIFESTCH_TRACE("  [{}] {}", n, DepotStr(dBase[n]));
        }
        if (pSharedDepotInfo) {
            LOG_MANIFESTCH_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            const DepotEntry* sBase = pSharedDepotInfo->m_Memory.m_pMemory;
            for (uint32 n = 0; n < pSharedDepotInfo->m_Size; ++n)
                LOG_MANIFESTCH_TRACE("  shared[{}] {}", n, DepotStr(sBase[n]));
        }

        if (!outcome) return outcome;

        const auto& overrides = LuaLoader::GetManifestOverrides();
        if (overrides.empty()) return outcome;

        if (pDepotInfo && pDepotInfo->m_Size) {
            DepotEntry* pBegin = pDepotInfo->m_Memory.m_pMemory;
            DepotEntry* pEnd   = pBegin + pDepotInfo->m_Size;
            for (DepotEntry* ep = pBegin; ep != pEnd; ++ep) {
                auto it = overrides.find(ep->DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : ep->ManifestSize;
                    LOG_MANIFESTCH_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        ep->DepotId, ep->ManifestGid, it->second.gid,
                        ep->ManifestSize, newSize);
                    ep->ManifestGid  = it->second.gid;
                    ep->ManifestSize = newSize;
                }
            }
        }
        return outcome;
    }

} // anonymous namespace

namespace ManifestBind {

    void Install() {
        LC_TX_OPEN();
        LC_ATTACH_D(BuildDepotDependency);
        LC_TX_COMMIT();
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(BuildDepotDependency);
        LC_TX_COMMIT();
    }
}
