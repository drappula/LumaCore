// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#pragma once

#include <cstdint>
#include "Steam/Types.h"

namespace CmdUser {
    void Register();

    // eticket async-call map for GetAPICallResult(154).
    // LookupEticketAsyncCall returns the AppId if recorded, 0 otherwise.
    AppId_t LookupEticketAsyncCall(uint64 hAsyncCall);
    void EraseEticketAsyncCall(uint64 hAsyncCall);
}
