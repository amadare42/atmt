// loader_hooks.cpp - the hook service mods use (MinHook, single instance).
//
// Keeping hooking in the loader means: one MinHook instance per process however
// many mods are installed, one place that knows the rules (an address must be
// committed executable code, or patching it would crash the game), and one place
// to report failures into the shared log.
#include "loader.h"

#include <MinHook.h>

namespace atmt_loader {

namespace {
CRITICAL_SECTION g_lock;
INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
volatile bool g_initialized = false;
volatile bool g_init_failed = false;
}  // namespace

namespace {

BOOL CALLBACK InitLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

// Mods may create hooks from threads of their own, so the first call can come from two at once.
void EnsureLock() { InitOnceExecuteOnce(&g_lock_once, &InitLock, nullptr, nullptr); }

bool EnsureInitialized() {
    if (g_initialized) return true;
    if (g_init_failed) return false;
    // MinHook is not thread safe while initializing; mods load from our own thread
    // but a mod may create its hooks from a thread of its own later on.
    EnterCriticalSection(&g_lock);
    if (!g_initialized && !g_init_failed) {
        const MH_STATUS status = MH_Initialize();
        if (status == MH_OK) {
            g_initialized = true;
            Log("hooks: MinHook initialized");
        } else {
            g_init_failed = true;
            LogError("hooks: MH_Initialize failed (%d)", static_cast<int>(status));
        }
    }
    const bool ok = g_initialized;
    LeaveCriticalSection(&g_lock);
    return ok;
}

// A hook target has to be code: a stale or mistyped address would otherwise be
// handed to MinHook, which rewrites the bytes at that address - the crash class
// that cost this project a game session.
bool IsExecutable(const void* address) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                           | PAGE_EXECUTE_WRITECOPY)) != 0;
}

}  // namespace

void* HookCreate(void* target, void* detour, void** trampoline) {
    EnsureLock();
    if (target == nullptr || detour == nullptr) return nullptr;
    if (!IsExecutable(target)) {
        LogError("hooks: refused 0x%08x - not executable code in this process",
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)));
        return nullptr;
    }
    if (!EnsureInitialized()) return nullptr;

    void* original = nullptr;
    const MH_STATUS status =
        MH_CreateHook(target, detour, trampoline != nullptr ? trampoline : &original);
    if (status != MH_OK) {
        LogError("hooks: MH_CreateHook(0x%08x) failed (%d)",
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)),
            static_cast<int>(status));
        return nullptr;
    }
    Log("hooks: created at 0x%08x",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)));
    return target;   // the target address doubles as the handle
}

int HookEnable(void* handle) {
    EnsureLock();
    if (!EnsureInitialized()) return -1;
    // NULL means "all hooks", which is how mods enable everything they created
    const MH_STATUS status = MH_EnableHook(handle);
    if (status != MH_OK) {
        LogError("hooks: MH_EnableHook failed (%d)", static_cast<int>(status));
        return -1;
    }
    return 0;
}

int HookDisable(void* handle) {
    EnsureLock();
    if (!EnsureInitialized()) return -1;
    return MH_DisableHook(handle) == MH_OK ? 0 : -1;
}

int HookRemove(void* handle) {
    EnsureLock();
    if (!EnsureInitialized()) return -1;
    return MH_RemoveHook(handle) == MH_OK ? 0 : -1;
}

void HookShutdown() {
    EnsureLock();
    if (g_initialized) {
        MH_Uninitialize();
        g_initialized = false;
    }
}

}  // namespace atmt_loader
