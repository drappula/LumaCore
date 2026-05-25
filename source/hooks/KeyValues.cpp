// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

// Hook KeyValues::ReadAsBinary — the parser entry point Steam uses for KV
// trees. Manifest depot patching lives in ManifestBind::BuildDepotDependency,
// so this hook is currently observation-only: each fire feeds the keyvalue
// category so triage on future KV regressions has a non-empty log to read.

#include "KeyValues.h"
#include "Macros.h"
#include "entry.h"

namespace {

    LC_HOOK_DEF(ReadAsBinary, bool,
                KeyValues* root, void* buf, int depth,
                bool textMode, void* symTable)
    {
        LOG_KEYVALUECH_INFO("KeyValues::ReadAsBinary fire (root=0x{:X}, depth={}, textMode={})",
                            reinterpret_cast<uintptr_t>(root), depth, textMode);
        return oReadAsBinary(root, buf, depth, textMode, symTable);
    }

}

namespace KVHooks {

    void Install() {
        LC_TX_OPEN();
        LC_ATTACH_EX_D(ReadAsBinary, KeyValues_ReadAsBinarySigs);
        LC_TX_COMMIT();
        LOG_KEYVALUECH_INFO("KVHooks::Install: ReadAsBinary {}",
                            oReadAsBinary ? "attached" : "pattern miss");
    }

    void Uninstall() {
        LC_TX_OPEN();
        LC_DETACH(ReadAsBinary);
        LC_TX_COMMIT();
        LOG_KEYVALUECH_INFO("KVHooks::Uninstall: ReadAsBinary detached");
    }

}
