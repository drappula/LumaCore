// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "hooks/client/ManifestBind.h"
#include "hooks/Macros.h"
#include "core/entry.h"
#include "runtime/ManifestCache.h"
#include <cstdint>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

// hook that patches depot gid/size in the output vector after Steam builds it.
// we don't hook BIsDlcEnabled / IsAppDlcInstalled / IsCloudEnabledForApp —
// CheckAppOwnership already covers those, adding em would be redundant.

namespace ManifestBind::Internal {

    constexpr uint32_t kDepotHardCap = 8192;

    // Per-fetch timeout for the on-demand pre-seed in YldLoadDepotManifest.
    // Most manifests are edge-cache hits (~100-300 ms); this cap keeps one
    // slow or missing manifest from stalling Steam, and on timeout the
    // original simply takes its normal path (worst case is old behaviour).
    constexpr uint32_t kPreseedFetchTimeoutMs = 5000;

    // depotId -> (appId, Steam's own manifest gid). Recorded from
    // BuildDepotDependency output BEFORE pin overrides, so this is what
    // Steam believes rather than what we told it.
    struct DepotSeen { AppId_t appId; uint64_t gid; };
    std::unordered_map<uint32_t, DepotSeen> g_depotsSeen;
    std::mutex g_depotsSeenMutex;

    // (depotId, gid) pairs already warned about this session. A stuck
    // download re-requests every ~30 s; the user only needs one warning.
    std::set<std::pair<uint32_t, uint64_t>> g_notifiedMissing;
    std::mutex g_missingMutex;

    void RecordDepots(const CUtlVector<DepotEntry>* vec) {
        if (!vec || !vec->m_Size || !vec->m_Memory.m_pMemory) return;
        uint32_t n = vec->m_Size;
        if (n > kDepotHardCap) n = kDepotHardCap;
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        for (uint32_t i = 0; i < n; ++i) {
            const DepotEntry& e = vec->m_Memory.m_pMemory[i];
            if (!e.DepotId || !e.ManifestGid) continue;
            g_depotsSeen[e.DepotId] = DepotSeen{e.AppId, e.ManifestGid};
        }
    }

    // safe window over CUtlVector<DepotEntry> — Steam's internal layout
    class DepotBank {
        CUtlVector<DepotEntry>* m_store = nullptr;
        uint32_t m_items = 0;

    public:
        explicit DepotBank(CUtlVector<DepotEntry>* store) : m_store(store) {
            if (!m_store || !m_store->m_Size) return;
            m_items = m_store->m_Size;
            if (m_items > kDepotHardCap) {
                LOG_MANBND_WARN("BuildDepotDependency: clipping count {} to {}", m_items, kDepotHardCap);
                m_items = kDepotHardCap;
            }
            if (!m_store->m_Memory.m_pMemory) {
                LOG_MANBND_ERROR("BuildDepotDependency: backing memory is null");
                m_items = 0;
            }
        }

        bool HasEntries() const { return m_items > 0; }
        uint32_t Len() const { return m_items; }
        const DepotEntry& Get(uint32_t ix) const { return m_store->m_Memory.m_pMemory[ix]; }
        DepotEntry& Mut(uint32_t ix) { return m_store->m_Memory.m_pMemory[ix]; }

        std::string DumpEntry(uint32_t ix) const {
            const auto& e = Get(ix);
            return std::format("[DepotId={} | AppId={} | Gid={} | Size={} | Dlc={} | Lcs={} | Carry={} | Shared={}]",
                e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
                (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
        }
    };

    // walk the depot list and slap in any overrides from lua config
    static void SlapManifestOverrides(DepotBank& bank) {
        if (!bank.HasEntries()) return;
        const auto& overrides = LuaLoader::GetManifestOverrides();
        if (overrides.empty()) return;

        static std::once_flag s_once;
        std::call_once(s_once, [&]() {
            LOG_MANBND_INFO("manifest-map-dump size={}", static_cast<uint32_t>(overrides.size()));
            for (const auto& [k, v] : overrides)
                LOG_MANBND_INFO("manifest-map-entry depot={} gid={}", k, v.gid);
        });

        uint32_t idx = 0;
        while (idx < bank.Len()) {
            uint64_t key = static_cast<uint64_t>(bank.Get(idx).DepotId);
            auto it = overrides.find(key);
            if (it != overrides.end()) {
                uint64_t newSz = it->second.size ? it->second.size : bank.Get(idx).ManifestSize;
                LOG_MANBND_INFO("manifest-override depot={} gid={}->{} size={}->{}",
                    bank.Get(idx).DepotId, bank.Get(idx).ManifestGid, it->second.gid,
                    bank.Get(idx).ManifestSize, newSz);
                bank.Mut(idx).ManifestGid  = it->second.gid;
                bank.Mut(idx).ManifestSize = newSz;
            } else {
                LOG_MANBND_INFO("manifest-scan depot={} gid={} appid={} size={} mapSize={}",
                    bank.Get(idx).DepotId, bank.Get(idx).ManifestGid,
                    bank.Get(idx).AppId, bank.Get(idx).ManifestSize,
                    static_cast<uint32_t>(overrides.size()));
            }
            ++idx;
        }
    }

} // namespace ManifestBind::Internal

namespace {
    using ManifestBind::Internal::DepotBank;
    using ManifestBind::Internal::RecordDepots;
    using ManifestBind::Internal::SlapManifestOverrides;
    using ManifestBind::Internal::kPreseedFetchTimeoutMs;

    LM_HOOK(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32_t* pBuildId, bool* pbBetaFallback)
    {
        bool ok = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        if (pDepotInfo) {
            DepotBank db(pDepotInfo);
            LOG_MANBND_TRACE("BuildDepotDependency appid={} depots={} ok={}", AppId, db.Len(), ok);
            if (!ok) return ok;

            // Steam reports zero depots for an app during the window where its
            // appinfo is being swapped in by a PICS refresh — the old depot set
            // is dropped before the new one lands. The caller takes whatever
            // comes back and stores it, so acting on the transient writes an
            // EMPTY depot config and the install silently "completes" having
            // downloaded nothing. Returning false makes that caller bail on its
            // clean failure path and keep the previous depot config, so the
            // next pass (once appinfo has landed) builds it properly. Scoped
            // to lua-managed apps; an app that genuinely has none is untouched.
            //
            // Only pDepotInfo is tested. pSharedDepotInfo holds depots belonging
            // to OTHER apps (sharedinstall / depotfromapp redirects), so it says
            // nothing about whether this app's own config is sound.
            if (AppId != 0 && pDepotInfo->m_Size == 0 && LuaLoader::HasDepot(AppId)) {
                LOG_MANBND_WARN("BuildDepotDependency: app {} returned 0 depots (appinfo mid-refresh) — failing the call so Steam keeps its existing depot config instead of storing an empty one", AppId);
                return false;
            }

            // Before the override pass, so what is cached is Steam's gid.
            RecordDepots(pDepotInfo);
            RecordDepots(pSharedDepotInfo);
            SlapManifestOverrides(db);
        }
        return ok;
    }

    // CDepotDownloadMgr's per-manifest acquire. Fires at the per-manifest
    // chokepoint immediately before the original's own disk check, so a
    // bounded blocking pre-seed here converts into a first-attempt success
    // with no request code. Unlike BuildDepotDependency this fires for every
    // manifest acquisition, including workshop items (depot == appid), which
    // never pass through BuildDepotDependency. Pinned depots are covered too:
    // the pinned gid was patched into the depot config above, so that is the
    // gid Steam goes on to request.
    //
    // Calling convention verified against the BetterSteamTools reference
    // (Hooks_Manifest.cpp:224, __fastcall): LumaCore's LM_HOOK is __fastcall,
    // so the signature carries over unchanged.
    LM_HOOK(YldLoadDepotManifest, int64_t, void* a1, void* a2, int appId,
              uint32_t depotId, uint64_t gid, const char* branch)
    {
        // Only depots Lua unlocks and does not own. A miss just returns and
        // the original takes its normal request-code path.
        if (depotId && gid && LuaLoader::HasDepot(depotId)) {
            LOG_MANBND_DEBUG("YldLoadDepotManifest: pre-seed app={} depot={} gid={} branch={}",
                appId, depotId, gid, branch ? branch : "");
            bool notArchived = false;
            ManifestCache::EnsureCached(static_cast<uint32_t>(appId), depotId, gid,
                kPreseedFetchTimeoutMs, &notArchived, false);
            if (notArchived)
                ManifestBind::ReportMissingManifest(depotId, gid);
        }
        return oYldLoadDepotManifest(a1, a2, appId, depotId, gid, branch);
    }

} // anonymous namespace

namespace ManifestBind {

    void Install() {
        LM_TX_BEGIN();
        LM_INSTALL(BuildDepotDependency);
        LM_INSTALL(YldLoadDepotManifest);
        LM_TX_COMMIT();
    }

    void Uninstall() {
        LM_TX_BEGIN();
        LM_REMOVE(BuildDepotDependency);
        LM_REMOVE(YldLoadDepotManifest);
        LM_TX_COMMIT();
    }

    bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid) {
        std::lock_guard<std::mutex> lock(Internal::g_depotsSeenMutex);
        auto it = Internal::g_depotsSeen.find(depotId);
        if (it == Internal::g_depotsSeen.end()) return false;
        outAppId = it->second.appId;
        outGid   = it->second.gid;
        return true;
    }

    void ReportMissingManifest(uint32_t depotId, uint64_t gid) {
        {
            std::lock_guard<std::mutex> lock(Internal::g_missingMutex);
            if (!Internal::g_notifiedMissing.emplace(depotId, gid).second) return;
        }
        LOG_MANBND_WARN("manifest depot={} gid={} not archived yet (404) — queued, retry the download in a little while",
            depotId, gid);
    }
}
