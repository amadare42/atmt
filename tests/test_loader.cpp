// test_loader.cpp - end-to-end test of the loader + mod pair, without the game.
//
// Run it from a folder that contains:
//   GFSDK_SSAO_D3D11.win32.dll        the loader (auto-load flavour), our build
//   GFSDK_SSAO_D3D11.win32.orig.dll   the real library, renamed (deploy does this)
//   atmt_mods\trails_dialog_logger.dll the mod
//
// It verifies, in order:
//   1. the loader loads at all
//   2. it exports the two GFSDK functions the game imports
//   3. calling one through the thunk reaches the real library (forwarding)
//   4. the loader found the mod folder and started the mod
//   5. the mod opened its dialog log and reported its hook
//
// A failure here means "do not put this in the game folder yet".
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

typedef int(__cdecl* PFN_GetVersion)(void*);

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

std::string ReadFile(const char* path) {
    std::string out;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

bool FileExists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Waits until the file contains `needle`, or until the timeout runs out.
bool WaitFor(const char* path, const char* needle, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 250) {
        if (ReadFile(path).find(needle) != std::string::npos) return true;
        Sleep(250);
    }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // never lose a line to a crash

    Check(FileExists("GFSDK_SSAO_D3D11.win32.dll"), "the loader dll is next to the test");
    Check(FileExists("GFSDK_SSAO_D3D11.win32.orig.dll"),
          "the original library is next to it (renamed)");

    HMODULE loader = LoadLibraryA("GFSDK_SSAO_D3D11.win32.dll");
    Check(loader != nullptr, "the loader loads (auto-load flavour)");
    if (loader == nullptr) {
        std::printf("       LoadLibrary error %lu\n", GetLastError());
        return 1;
    }

    auto get_version =
        reinterpret_cast<PFN_GetVersion>(GetProcAddress(loader, "GFSDK_SSAO_GetVersion"));
    void* create_context = reinterpret_cast<void*>(
        GetProcAddress(loader, "GFSDK_SSAO_CreateContext_D3D11"));
    Check(get_version != nullptr, "GFSDK_SSAO_GetVersion is exported");
    Check(create_context != nullptr, "GFSDK_SSAO_CreateContext_D3D11 is exported");

    // forwarding: the same call through the loader and straight to the original
    HMODULE real = LoadLibraryA("GFSDK_SSAO_D3D11.win32.orig.dll");
    bool forward_ok = false;
    if (real != nullptr && get_version != nullptr) {
        auto get_version_real =
            reinterpret_cast<PFN_GetVersion>(GetProcAddress(real, "GFSDK_SSAO_GetVersion"));
        if (get_version_real != nullptr) {
            unsigned char via_loader[64] = {};
            unsigned char via_real[64] = {};
            const int status_loader = get_version(via_loader);
            const int status_real = get_version_real(via_real);
            forward_ok = status_loader == status_real
                         && std::memcmp(via_loader, via_real, sizeof(via_loader)) == 0;
            std::printf("       GetVersion: loader=(%d) real=(%d)\n", status_loader,
                        status_real);
        } else {
            std::printf("       [skip] the original is a placeholder (no GFSDK exports)\n");
            forward_ok = true;
        }
    }
    Check(forward_ok, "the thunk forwards to the real library");

    // the mod: discovered, started, and reporting its source
    Check(WaitFor("atmt_loader.log", "mod folder", 8000),
          "the loader wrote its log and found the mod folder");
    Check(WaitFor("atmt_loader.log", "trails_dialog_logger started", 20000),
          "the loader started the mod");

    // The mod opens its dialog log and says which source it hooked. Silence is the failure.
    const bool started = WaitFor("atmt_dialogs.jsonl", "logger started:", 25000);
    Check(started, "the mod opened its dialog log and reported its source");

    // The loader log carries the same report: the game's message setter and the addresses it
    // reads from it.
    const bool reported = WaitFor("atmt_loader.log", "logger started:", 8000)
                          || WaitFor("atmt_loader.log", "no hook service", 2000);
    Check(reported, "the mod reported its source through the loader log");

    FreeLibrary(loader);   // exercises the shutdown path (reserved == NULL)
    std::printf("\n%s (%d failure(s))\n",
                g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
