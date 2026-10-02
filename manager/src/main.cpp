// main.cpp - atmt_manager: one binary for the window and the command line.
//
//   atmt_manager                 the window (gamepad first; works in Steam's Game Mode)
//   atmt_manager --cli <cmd>     the command line (cli.cpp), also what the developer scripts run
#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"
#include "core/updater.h"
#include "core/util.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef ATMT_MANAGER_GUI
namespace atmt {
int RunGui(const std::vector<std::string>& args);
}
#endif

namespace {

std::vector<std::string> Arguments(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    // the UTF-8 spelling of the real (UTF-16) command line: a game folder may not be ASCII
    (void)argc;
    (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(w);
    SetConsoleOutputCP(CP_UTF8);
#else
    for (int i = 1; i < argc; ++i) out.push_back(argv[i]);
#endif
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = Arguments(argc, argv);
    atmt::CleanupAfterAppUpdate();
#ifdef ATMT_MANAGER_GUI
    if (!atmt::IsCliInvocation(args)) {
#ifdef _WIN32
        // A console subsystem exe (so the CLI prints): started from Explorer it got a console of
        // its own, which the window does not need.
        DWORD procs[2];
        if (GetConsoleProcessList(procs, 2) <= 1) FreeConsole();
#endif
        return atmt::RunGui(args);
    }
#endif
    return atmt::RunCli(args);
}
