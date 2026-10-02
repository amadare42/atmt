// platform.cpp - see platform.h.
#include "platform.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace atmt {

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

bool ProcessRunningW(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

}  // namespace

bool ProcessRunning(const std::string& name) { return ProcessRunningW(Wide(name).c_str()); }
bool GameRunning() { return ProcessRunningW(L"ed8.exe"); }
bool WineserverRunning() { return false; }

fs::path SelfExe() {
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return fs::path(std::wstring(buf, n));
}

fs::path SelfDir() { return SelfExe().parent_path(); }
bool RunningFromAppImage() { return false; }

std::string GetEnv(const char* name) {
    const std::wstring wname = Wide(name);
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, 32768);
    if (n == 0 || n >= 32768) return std::string();
    return Narrow(std::wstring(buf, n));
}

fs::path DataDir() {
    std::string base = GetEnv("LOCALAPPDATA");
    if (base.empty()) base = GetEnv("APPDATA");
    if (base.empty()) return SelfDir() / "atmt_manager_data";
    return Path(base) / "atmt_manager";
}

fs::path HomeDir() { return Path(GetEnv("USERPROFILE")); }

std::string SteamPathFromRegistry() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::string();
    }
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    std::string out;
    if (RegQueryValueExW(key, L"SteamPath", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS
        && type == REG_SZ) {
        out = Narrow(std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t))));
    }
    RegCloseKey(key);
    return out;
}

std::vector<std::pair<std::string, std::string>> GogGamesFromRegistry() {
    std::vector<std::pair<std::string, std::string>> out;
    // GOG's installers are 32-bit: their keys are in the 32-bit view (WOW6432Node)
    for (const REGSAM view : {static_cast<REGSAM>(KEY_WOW64_32KEY), static_cast<REGSAM>(KEY_WOW64_64KEY)}) {
        HKEY games;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | view, &games) != ERROR_SUCCESS) {
            continue;
        }
        for (DWORD i = 0;; ++i) {
            wchar_t name[256];
            DWORD name_len = 256;
            if (RegEnumKeyExW(games, i, name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY game;
            if (RegOpenKeyExW(games, name, 0, KEY_READ | view, &game) != ERROR_SUCCESS) continue;
            wchar_t buf[1024];
            DWORD size = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(game, L"path", nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS
                && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                const std::string id = Narrow(std::wstring(name, name_len));
                const std::string path = Narrow(std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t))));
                bool dup = false;
                for (const auto& e : out) dup = dup || (e.first == id && IEquals(e.second, path));
                if (!dup && !path.empty()) out.emplace_back(id, path);
            }
            RegCloseKey(game);
        }
        RegCloseKey(games);
    }
    return out;
}

bool PickFolder(const std::string& title, const std::string& start, std::string* out) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(Wide(title).c_str());
        if (!start.empty()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(Wide(start).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                dlg->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    *out = Narrow(path);
                    ok = !out->empty();
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

bool OpenUrl(const std::string& url) {
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool Launch(const fs::path& exe, const std::vector<std::string>& args) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const std::string& a : args) cmd += L" \"" + Wide(a) + L"\"";
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool RunAndWait(const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env,
                const fs::path& cwd, const fs::path& log, int* exit_code, std::string* error) {
    if (exit_code != nullptr) *exit_code = -1;
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (args.empty()) return fail("nothing to run");
    // the command line: every argument quoted (CommandLineToArgvW's rules: backslashes before a quote doubled)
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"';
        const std::wstring w = Wide(a);
        size_t slashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') {
                ++slashes;
            } else {
                if (c == L'"') cmd.append(slashes + 1, L'\\');
                slashes = 0;
            }
            cmd += c;
        }
        cmd.append(slashes, L'\\');
        cmd += L'"';
    }
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    // the environment: this process's, `env` replacing (an empty value removing) variables by name
    std::vector<wchar_t> block;
    if (!env.empty()) {
        if (wchar_t* own = GetEnvironmentStringsW()) {
            for (const wchar_t* e = own; *e != L'\0'; e += wcslen(e) + 1) {
                const std::wstring kv = e;
                const size_t eq = kv.find(L'=', 1);   // "=C:=C:\..." entries start with '='
                const std::string key = Narrow(kv.substr(0, eq));
                bool replaced = false;
                for (const auto& set : env) replaced = replaced || _stricmp(set.first.c_str(), key.c_str()) == 0;
                if (!replaced) block.insert(block.end(), kv.c_str(), kv.c_str() + kv.size() + 1);
            }
            FreeEnvironmentStringsW(own);
        }
        for (const auto& set : env) {
            if (set.second.empty()) continue;
            const std::wstring kv = Wide(set.first + "=" + set.second);
            block.insert(block.end(), kv.c_str(), kv.c_str() + kv.size() + 1);
        }
        block.push_back(L'\0');
    }
    HANDLE log_handle = INVALID_HANDLE_VALUE;
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!log.empty()) {
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        log_handle = CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_handle != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = nullptr;
            si.hStdOutput = log_handle;
            si.hStdError = log_handle;
        }
    }
    const std::wstring dir = cwd.empty() ? std::wstring() : cwd.wstring();
    PROCESS_INFORMATION pi;
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, log_handle != INVALID_HANDLE_VALUE,
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block.empty() ? nullptr : block.data(),
                                       dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    const DWORD start_error = GetLastError();
    if (log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
    if (!started) return fail("cannot start " + args[0] + " (error " + std::to_string(start_error) + ")");
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code != nullptr) *exit_code = static_cast<int>(code);
    return true;
}

std::string PlatformName() { return "windows"; }
bool IsSteamGameMode() { return false; }

#else  // Linux

// Does any process look like `name` (its comm, or the file name of its argv[0], which for a
// Proton game is a Windows path)?
bool ProcessRunning(const std::string& name) {
    DIR* proc = opendir("/proc");
    if (proc == nullptr) return false;
    bool found = false;
    const std::string self = std::to_string(getpid());
    while (dirent* e = readdir(proc)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || self == e->d_name) continue;
        const std::string base = std::string("/proc/") + e->d_name;
        std::string comm, cmdline;
        ReadFile(base + "/comm", &comm);
        if (IEquals(Trim(comm), name)) {
            found = true;
            break;
        }
        if (!ReadFile(base + "/cmdline", &cmdline) || cmdline.empty()) continue;
        std::string argv0 = cmdline.substr(0, cmdline.find('\0'));
        for (char& c : argv0) {
            if (c == '\\') c = '/';
        }
        const size_t slash = argv0.find_last_of('/');
        if (IEquals(slash == std::string::npos ? argv0 : argv0.substr(slash + 1), name)) {
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
}

bool GameRunning() { return ProcessRunning("ed8.exe"); }
bool WineserverRunning() { return ProcessRunning("wineserver"); }

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

fs::path SelfExe() {
    const std::string appimage = GetEnv("APPIMAGE");
    if (!appimage.empty()) return Path(appimage);
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
}

fs::path SelfDir() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
}

bool RunningFromAppImage() { return !GetEnv("APPIMAGE").empty(); }

fs::path HomeDir() {
    std::string home = GetEnv("HOME");
    return Path(home.empty() ? "/tmp" : home);
}

fs::path DataDir() {
    const std::string xdg = GetEnv("XDG_DATA_HOME");
    if (!xdg.empty()) return Path(xdg) / "atmt_manager";
    return HomeDir() / ".local" / "share" / "atmt_manager";
}

std::string SteamPathFromRegistry() { return std::string(); }
std::vector<std::pair<std::string, std::string>> GogGamesFromRegistry() { return {}; }

bool PickFolder(const std::string& title, const std::string& start, std::string* out) {
    // whichever dialog tool the desktop has (KDE on the Deck: kdialog); stderr is dropped
    auto quote = [](const std::string& s) {
        std::string q = "'";
        for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
    };
    const std::string dir = start.empty() ? U8(HomeDir()) : start;
    const std::string cmds[] = {
        "kdialog --title " + quote(title) + " --getexistingdirectory " + quote(dir) + " 2>/dev/null",
        "zenity --file-selection --directory --title=" + quote(title) + " --filename=" + quote(dir + "/") + " 2>/dev/null",
    };
    for (const std::string& tool : {std::string("kdialog"), std::string("zenity")}) {
        bool have = false;
        for (const std::string& p : Split(GetEnv("PATH"), ':')) have = have || (!p.empty() && Exists(Path(p) / tool));
        if (!have) continue;
        FILE* f = popen((tool == "kdialog" ? cmds[0] : cmds[1]).c_str(), "r");
        if (f == nullptr) continue;
        std::string got;
        char buf[512];
        while (fgets(buf, sizeof(buf), f) != nullptr) got += buf;
        const int rc = pclose(f);
        got = Trim(got);
        if (rc == 0 && !got.empty()) {
            *out = got;
            return true;
        }
        return false;   // the tool ran: the player cancelled
    }
    return false;
}

bool Launch(const fs::path& exe, const std::vector<std::string>& args) {
    std::vector<std::string> storage;
    storage.push_back(U8(exe));
    for (const std::string& a : args) storage.push_back(a);
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    pid_t pid;
    return posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) == 0;
}

bool RunAndWait(const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env,
                const fs::path& cwd, const fs::path& log, int* exit_code, std::string* error) {
    if (exit_code != nullptr) *exit_code = -1;
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (args.empty()) return fail("nothing to run");
    // everything the child needs is built before fork: after it only async-signal-safe calls
    std::vector<std::string> env_storage;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string kv = *e;
        const std::string key = kv.substr(0, kv.find('='));
        bool replaced = false;
        for (const auto& set : env) replaced = replaced || set.first == key;
        if (!replaced) env_storage.push_back(kv);
    }
    for (const auto& set : env) {
        if (!set.second.empty()) env_storage.push_back(set.first + "=" + set.second);
    }
    std::vector<char*> envp;
    for (std::string& s : env_storage) envp.push_back(&s[0]);
    envp.push_back(nullptr);
    std::vector<std::string> arg_storage = args;
    std::vector<char*> argv;
    for (std::string& s : arg_storage) argv.push_back(&s[0]);
    argv.push_back(nullptr);
    const std::string dir = U8(cwd);
    int log_fd = -1;
    if (!log.empty()) {
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        log_fd = open(U8(log).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    const int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) {
        if (log_fd >= 0) close(log_fd);
        if (null_fd >= 0) close(null_fd);
        return fail("cannot start " + args[0]);
    }
    if (pid == 0) {
        if (null_fd >= 0) dup2(null_fd, 0);
        if (log_fd >= 0) {
            dup2(log_fd, 1);
            dup2(log_fd, 2);
        }
        if (!dir.empty() && chdir(dir.c_str()) != 0) _exit(126);
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    if (log_fd >= 0) close(log_fd);
    if (null_fd >= 0) close(null_fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return fail("lost track of " + args[0]);
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != nullptr) *exit_code = code;
    if (code == 127) return fail("cannot run " + args[0]);
    return true;
}

bool OpenUrl(const std::string& url) { return Launch("/usr/bin/xdg-open", {url}); }

std::string PlatformName() { return "linux"; }

bool IsSteamGameMode() {
    // gamescope sets these for the apps it runs; Desktop Mode (KDE) does not.
    return !GetEnv("GAMESCOPE_WAYLAND_DISPLAY").empty() || GetEnv("XDG_CURRENT_DESKTOP") == "gamescope"
           || GetEnv("SteamGamepadUI") == "1";
}

#endif

}  // namespace atmt
