# proxy_gfsdk.s - ABI-agnostic forwarding thunks for the GFSDK_SSAO proxy.
#
# GFSDK_SSAO_D3D11.win32.dll exports exactly two undecorated C functions and only
# ed8.exe imports it (checked with atmt_modules against the live process, unlike
# winmm.dll which seven modules import). We install ourselves under that name so
# the game loads the logger without any injection step, and jump straight to the
# real library, which the deploy script keeps next to us as
# GFSDK_SSAO_D3D11.win32.orig.dll.
#
# A jump thunk is used instead of typed C wrappers on purpose: it does not need to
# know the prototype, the calling convention or the argument count, so it cannot
# corrupt the stack. `jmp dword ptr [ptr]` is 6 bytes: ff 25 <address>.

    .text

    .globl _GFSDK_SSAO_CreateContext_D3D11
_GFSDK_SSAO_CreateContext_D3D11:
    pushal
    call _atmt_gfsdk_resolve
    popal
    jmp *_g_real_CreateContext

    .globl _GFSDK_SSAO_GetVersion
_GFSDK_SSAO_GetVersion:
    pushal
    call _atmt_gfsdk_resolve
    popal
    jmp *_g_real_GetVersion
