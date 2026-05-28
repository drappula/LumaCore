// LumaCore - Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

// KeyValues::ReadAsBinary + KeyValues::FindOrCreateKey - the parser entry
// points Steam uses for KV trees. Manifest depot patching lives in
// ManifestBind::BuildDepotDependency, so these hooks are observation-only.
//
// Per-call logging is intentionally OFF: FindOrCreateKey fires hundreds of
// times per second once Steam loads its app list, and even at TRACE level
// the disk traffic is enough to drown out everything else in the log
// pipeline. If you ever need to triage a KV regression, flip the Install
// log line below or temporarily wrap a TRACE around oReadAsBinary inside
// the hook body.
//
// Address resolution flows through ByteSearch and the per-build TOML.
// Published TOMLs may carry either the bare hook id (`ReadAsBinary`,
// `FindOrCreateKey`) or the legacy namespaced form
// (`KeyValues_ReadAsBinary`, `KeyValues_FindOrCreateKey`); ByteSearch's
// alias retry handles the fallback so this file does not need a candidate
// array.

#include "KeyValues.h"
#include "Macros.h"
#include "entry.h"
#include "steam/Structs.h"

namespace {

    LC_HOOK_DEF(ReadAsBinary, bool,
                KeyValues* root, void* buf, int depth,
                bool textMode, void* symTable)
    {
        return oReadAsBinary(root, buf, depth, textMode, symTable);
    }

    LC_HOOK_DEF(FindOrCreateKey, KeyValues*,
                KeyValues* parent, const char* keyName,
                bool createMissing, KeyValues** outChild)
    {
        return oFindOrCreateKey(parent, keyName, createMissing, outChild);
    }

}

namespace KVHooks {

    void Install() {
        LC_TX_OPEN();
        LC_ATTACH_D(ReadAsBinary);
        LC_ATTACH_D(FindOrCreateKey);
        LC_TX_COMMIT();
        LOG_KEYVALUECH_INFO("KVHooks::Install: ReadAsBinary {} | FindOrCreateKey {}",
                            oReadAsBinary    ? "attached" : "pattern miss",
                            oFindOrCreateKey ? "attached" : "pattern miss");
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(FindOrCreateKey);
        LC_DETACH(ReadAsBinary);
        LC_TX_COMMIT();
        LOG_KEYVALUECH_INFO("KVHooks::Uninstall: ReadAsBinary + FindOrCreateKey detached");
    }

}
