// loader_paths.cpp - where the loader and the game are, and this dll's own handle.
//
// Two distinct notions, kept apart on purpose:
//   * the game folder: what the executable runs from. Anything the user sees
//     (logs, screenshots, inis) belongs there.
//   * this dll's folder: where the loader sits and where its mod folder is. With
//     the proxy deployment these are the same; when the loader is injected from
//     outside they are not.
#include "loader.h"

#include <cstdio>

namespace atmt_loader {

HMODULE g_self_module = nullptr;

namespace {

std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring ModulePathOf(HMODULE module) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(module, buf, MAX_PATH * 2);
    return n > 0 ? std::wstring(buf, n) : std::wstring();
}

}  // namespace

const std::wstring& SelfPath() {
    static const std::wstring cached = ModulePathOf(g_self_module);
    return cached;
}

const std::wstring& SelfDir() {
    static const std::wstring cached = DirOf(SelfPath());
    return cached;
}

const std::wstring& GameModulePath() {
    static const std::wstring cached = ModulePathOf(GetModuleHandleW(nullptr));
    return cached;
}

const std::wstring& GameDir() {
    static const std::wstring cached = DirOf(GameModulePath());
    return cached;
}

}  // namespace atmt_loader
