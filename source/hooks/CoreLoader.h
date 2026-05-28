// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#ifndef HOOKMANAGER_H
#define HOOKMANAGER_H

#include "entry.h"

namespace SteamUI {
    void CoreHook();
    void CoreUnhook();
}

namespace LumaCore {
    void Attach();
    void Detach();
}


#endif // HOOKMANAGER_H
