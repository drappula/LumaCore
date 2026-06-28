// LumaCore — Steam client hook layer for SteaMidra.
// Copyright (c) 2025-2026 Midrag (https://github.com/Midrags).
// Distributed under the GNU General Public License v3 or later.
// See <https://www.gnu.org/licenses/> for the full license text.

#include "hooks/client/OnlineFixInject.h"
#include "hooks/Macros.h"
#include "config/Settings.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace {

    std::mutex g_queueLock;
    std::unordered_map<std::wstring, AppId_t> g_queue;

    std::wstring LowerBasename(LPCWSTR path) {
        if (!path || !*path) return {};
        std::wstring name = std::filesystem::path(path).filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(),
            [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
        return name;
    }

    std::wstring ExeFromCmd(LPCWSTR cmd) {
        if (!cmd) return {};
        while (*cmd == L' ' || *cmd == L'\t') ++cmd;
        std::wstring out;
        if (*cmd == L'"') {
            for (++cmd; *cmd && *cmd != L'"'; ++cmd) out.push_back(*cmd);
        } else {
            for (; *cmd && *cmd != L' ' && *cmd != L'\t'; ++cmd) out.push_back(*cmd);
        }
        return out;
    }

    AppId_t ClaimPending(LPCWSTR app, LPCWSTR cmd) {
        std::wstring key = LowerBasename(app);
        if (key.empty()) key = LowerBasename(ExeFromCmd(cmd).c_str());
        if (key.empty()) return 0;

        std::lock_guard lk(g_queueLock);
        auto it = g_queue.find(key);
        if (it == g_queue.end()) return 0;
        AppId_t id = it->second;
        g_queue.erase(it);
        return id;
    }

    using CreateProcessW_t = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
        LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    using CreateProcessAsUserW_t = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
        LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

    CreateProcessW_t       oCreateProcessW       = nullptr;
    CreateProcessAsUserW_t oCreateProcessAsUserW = nullptr;

    // Inline injection matching RemoteInject::LoadDll — VirtualAllocEx +
    // CreateRemoteThread(LoadLibraryW). Uses the process HANDLE from
    // CreateProcess directly (no extra OpenProcess).
    static bool InjectPayload(HANDLE hProcess, LPCWSTR dllPath) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (!k32) return false;
        auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(k32, "LoadLibraryW"));
        if (!loadLib) return false;

        const SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
        void* mem = VirtualAllocEx(hProcess, nullptr, bytes,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) return false;

        bool ok = false;
        if (WriteProcessMemory(hProcess, mem, dllPath, bytes, nullptr)) {
            HANDLE t = CreateRemoteThread(hProcess, nullptr, 0, loadLib, mem, 0, nullptr);
            if (t) {
                ok = (WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
                CloseHandle(t);
            }
        }
        VirtualFreeEx(hProcess, mem, 0, MEM_RELEASE);
        return ok;
    }

    BOOL LaunchSuspended(HANDLE token, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
               LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
               LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
    {
        auto fwd = [&](DWORD f) {
            return token
                ? oCreateProcessAsUserW(token, app, cmd, pa, ta, inherit, f, env, cwd, si, pi)
                : oCreateProcessW(app, cmd, pa, ta, inherit, f, env, cwd, si, pi);
        };

        AppId_t appId = ClaimPending(app, cmd);
        if (!appId || PayloadPath[0] == 0) return fwd(flags);

        BOOL ok = fwd(flags | CREATE_SUSPENDED);
        if (!ok) {
            LOG_ONLINEFIX_WARN("appid={} spawn failed err={}", appId, GetLastError());
            return ok;
        }

        wchar_t wPayload[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, PayloadPath, -1, wPayload, MAX_PATH);
        bool injected = InjectPayload(pi->hProcess, wPayload);
        LOG_ONLINEFIX_INFO("appid={} pid={} payload {}", appId, pi->dwProcessId,
                           injected ? "loaded" : "FAILED");

        if (!(flags & CREATE_SUSPENDED)) ResumeThread(pi->hThread);
        return ok;
    }

    BOOL WINAPI hkCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
        LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
        LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
    {
        return LaunchSuspended(nullptr, app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    }

    BOOL WINAPI hkCreateProcessAsUserW(HANDLE token, LPCWSTR app, LPWSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
        LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
    {
        return LaunchSuspended(token, app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    }

}

namespace {

    void ResetPayloadLogs() {
        namespace fs = std::filesystem;
        fs::path dir = fs::path(Settings::logDir) / "payload";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }

}

namespace OnlineFixInject {

    void Install() {
        if (PayloadPath[0] == 0) {
            LOG_ONLINEFIX_WARN("payload path not set; injection disabled");
            return;
        }
        if (!Settings::onlineFixInjectEnabled) {
            LOG_ONLINEFIX_INFO("online-fix injection disabled by config");
            return;
        }
        if (GetFileAttributesA(PayloadPath) == INVALID_FILE_ATTRIBUTES) {
            LOG_ONLINEFIX_WARN("payload DLL not found at \"{}\"; injection disabled", PayloadPath);
            return;
        }
        ResetPayloadLogs();
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (!k32) return;
        oCreateProcessW       = reinterpret_cast<CreateProcessW_t>      (GetProcAddress(k32, "CreateProcessW"));
        oCreateProcessAsUserW = reinterpret_cast<CreateProcessAsUserW_t>(GetProcAddress(k32, "CreateProcessAsUserW"));

        LM_TX_BEGIN();
        if (oCreateProcessW)
            DetourAttach(reinterpret_cast<PVOID*>(&oCreateProcessW),
                         reinterpret_cast<PVOID>(hkCreateProcessW));
        if (oCreateProcessAsUserW)
            DetourAttach(reinterpret_cast<PVOID*>(&oCreateProcessAsUserW),
                         reinterpret_cast<PVOID>(hkCreateProcessAsUserW));
        LM_TX_COMMIT();
        LOG_ONLINEFIX_INFO("spawn hooks installed dll=\"{}\"", PayloadPath);
    }

    void Uninstall() {
        LM_TX_BEGIN();
        if (oCreateProcessW) {
            DetourDetach(reinterpret_cast<PVOID*>(&oCreateProcessW),
                         reinterpret_cast<PVOID>(hkCreateProcessW));
            oCreateProcessW = nullptr;
        }
        if (oCreateProcessAsUserW) {
            DetourDetach(reinterpret_cast<PVOID*>(&oCreateProcessAsUserW),
                         reinterpret_cast<PVOID>(hkCreateProcessAsUserW));
            oCreateProcessAsUserW = nullptr;
        }
        LM_TX_COMMIT();

        std::lock_guard lk(g_queueLock);
        g_queue.clear();
    }

    void QueueInjection(const char* exePath, AppId_t realAppId) {
        if (!realAppId || !exePath || !*exePath) return;

        wchar_t wexe[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, exePath, -1, wexe, MAX_PATH);
        std::wstring key = LowerBasename(wexe);
        if (key.empty()) return;

        std::lock_guard lk(g_queueLock);
        g_queue[key] = realAppId;
    }

}
