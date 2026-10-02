// dummy_orig.cpp - a stand-in for GFSDK_SSAO_D3D11.win32.dll.
//
// The loader test needs a library to forward to. With the real game installed that is
// the renamed original, but on a build machine there may be none - and using our own
// loader as the "original" is wrong: loading it starts a second loader (and a second
// copy of every mod), which made the test see duplicated log lines.
//
// So this stub exports the same two names and answers recognisably, which also lets
// the test verify that the thunk really forwards (the values come back from here).
#include <windows.h>

#include <cstring>

extern "C" {

__declspec(dllexport) int __cdecl GFSDK_SSAO_GetVersion(void* out) {
    if (out != nullptr) std::memset(out, 0x5A, 8);   // a pattern the test looks for
    return 0x1234;
}

__declspec(dllexport) void* __cdecl GFSDK_SSAO_CreateContext_D3D11(void* device,
                                                                  void* context) {
    (void)device;
    (void)context;
    return nullptr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;   // nothing to do: this is only a forwarding target
}

}  // extern "C"
