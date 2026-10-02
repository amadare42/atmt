// saves.cpp - finding the saves in the game's save folder.
//
// The folder holds five kinds of files and only two of them are saves:
//
//   save000.dat .. save129.dat      the manual slots        <- a save
//   autosave00.dat .. autosave07.dat the auto slots         <- a save
//   autosaveNN_t.dat                the auto slot's header  (text: chapter, location, playtime)
//   thumbNNN.bmp                    the slot thumbnails
//   sdslot.dat, steam_autocloud.vdf metadata (sdslot.dat is 64 x 5760 bytes of slot headers)
//
// Picking the wrong one of those is the obvious way for this feature to fail, so the filter is
// exact and the tests feed it all five.
#include "al.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace al {

namespace {

// "save" + exactly three digits, or "autosave" + exactly two digits, then ".dat".
bool DigitsThenDat(const wchar_t* p, int digits) {
    for (int i = 0; i < digits; ++i) {
        if (p[i] < L'0' || p[i] > L'9') return false;
    }
    const wchar_t* tail = p + digits;
    return _wcsicmp(tail, L".dat") == 0;
}

bool StartsWithNoCase(const wchar_t* s, const wchar_t* prefix) {
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

}  // namespace

bool IsSaveFileName(const wchar_t* name) {
    if (name == nullptr) return false;
    if (StartsWithNoCase(name, L"autosave")) return DigitsThenDat(name + 8, 2);
    if (StartsWithNoCase(name, L"save")) {
        if (!DigitsThenDat(name + 4, 3)) return false;
        // Slot 511 is the game's own system file (save511.dat, ~52 bytes: settings and progress
        // flags), not a save the player can load. It sits in the same folder, is often the newest
        // file there, and picking it as "the latest save" is exactly the bug this guards.
        unsigned slot = 0;
        for (int i = 0; i < 3; ++i) {
            slot = slot * 10 + static_cast<unsigned>(name[4 + i] - L'0');
        }
        return slot != 511;
    }
    return false;
}

std::vector<SaveFile> FindSaves(const std::wstring& dir, bool include_autosaves) {
    std::vector<SaveFile> out;
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = dir + L"\\*.dat";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        if (!IsSaveFileName(fd.cFileName)) continue;
        if (!include_autosaves && StartsWithNoCase(fd.cFileName, L"autosave")) continue;
        SaveFile s;
        s.path = dir + L"\\" + fd.cFileName;
        s.time = fd.ftLastWriteTime;
        s.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(s);
    } while (FindNextFileW(h, &fd) != 0);
    FindClose(h);

    // Newest first. Equal times (a copy operation, a slow clock) break by name so that the
    // result is the same on every run - "latest" must not be a coin flip.
    std::sort(out.begin(), out.end(), [](const SaveFile& a, const SaveFile& b) {
        const int cmp = CompareFileTime(&a.time, &b.time);
        if (cmp != 0) return cmp > 0;
        return _wcsicmp(a.path.c_str(), b.path.c_str()) > 0;
    });
    return out;
}

std::wstring DefaultSaveDir() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &path)) && path != nullptr) {
        std::wstring dir = path;
        CoTaskMemFree(path);
        return dir + L"\\Falcom\\ed8";
    }
    // Fallback for the (unlikely) case where the shell does not know the folder.
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    if (profile == nullptr) return std::wstring();
    return std::wstring(profile) + L"\\Saved Games\\Falcom\\ed8";
}

std::string FormatTime(const FILETIME& ft) {
    FILETIME local = ft;
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st;
    if (FileTimeToSystemTime(&local, &st) == 0) return "?";
    char buf[32];
    _snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
              st.wMinute);
    return buf;
}

}  // namespace al
