// injector32.cpp - minimal 32-bit DLL injector (CreateRemoteThread + LoadLibraryA).
//
// Must be a 32-bit binary: the address of LoadLibraryA has to come from the
// 32-bit kernel32, otherwise the injected thread jumps into a 64-bit address
// that does not exist in the target process.
//
// usage: atmt_inject --pid <pid> --dll <path> [--wait-for ed8] [--timeout 120]
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static bool Inject(DWORD pid, const char* dll) {
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (h == nullptr) {
        printf("OpenProcess(%lu) failed (%lu)\n", pid, GetLastError());
        return false;
    }
    const SIZE_T len = strlen(dll) + 1;
    void* mem = VirtualAllocEx(h, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (mem == nullptr) {
        printf("VirtualAllocEx failed (%lu)\n", GetLastError());
        CloseHandle(h);
        return false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(h, mem, dll, len, &written)) {
        printf("WriteProcessMemory failed (%lu)\n", GetLastError());
        CloseHandle(h);
        return false;
    }
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    printf("LoadLibraryA at %p (32-bit)\n", reinterpret_cast<void*>(loadLib));
    HANDLE th = CreateRemoteThread(h, nullptr, 0,
                                   reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLib), mem, 0,
                                   nullptr);
    if (th == nullptr) {
        printf("CreateRemoteThread failed (%lu)\n", GetLastError());
        CloseHandle(h);
        return false;
    }
    WaitForSingleObject(th, 10000);
    DWORD exit_code = 0;
    GetExitCodeThread(th, &exit_code);
    CloseHandle(th);
    CloseHandle(h);
    printf("remote LoadLibraryA returned 0x%08lx (%s)\n", exit_code,
           exit_code == 0 ? "FAILED - dll did not load" : "ok");
    return exit_code != 0;
}

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::string dll;
    std::string wait_for;
    int timeout_s = 120;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--pid") pid = static_cast<DWORD>(strtoul(next().c_str(), nullptr, 10));
        else if (a == "--dll") dll = next();
        else if (a == "--wait-for") wait_for = next();
        else if (a == "--timeout") timeout_s = atoi(next().c_str());
    }
    if (dll.empty()) {
        printf("usage: atmt_inject --pid <pid> --dll <path>   (or --wait-for ed8)\n");
        return 2;
    }
    if (pid == 0 && !wait_for.empty()) {
        printf("waiting for %s.exe ...\n", wait_for.c_str());
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeout_s) * 1000;
        while (GetTickCount() < deadline) {
            std::string cmd = "tasklist /fi \"imagename eq " + wait_for + ".exe\" /fo csv /nh";
            FILE* p = _popen(cmd.c_str(), "r");
            if (p != nullptr) {
                char line[512];
                while (fgets(line, sizeof(line), p) != nullptr) {
                    char* comma = strchr(line, ',');
                    if (comma != nullptr) {
                        char* q1 = strchr(comma + 1, '"');
                        if (q1 != nullptr) {
                            pid = static_cast<DWORD>(strtoul(q1 + 1, nullptr, 10));
                            break;
                        }
                    }
                }
                _pclose(p);
            }
            if (pid != 0) break;
            Sleep(500);
        }
        if (pid == 0) {
            printf("target process not found within %d s\n", timeout_s);
            return 3;
        }
        printf("found %s.exe with pid %lu\n", wait_for.c_str(), pid);
    }
    if (pid == 0) {
        printf("need --pid (or --wait-for)\n");
        return 2;
    }
    char full[MAX_PATH];
    if (GetFullPathNameA(dll.c_str(), MAX_PATH, full, nullptr) == 0) {
        printf("bad dll path\n");
        return 2;
    }
    printf("injecting %s into pid %lu ...\n", full, pid);
    return Inject(pid, full) ? 0 : 1;
}
