// test_load_last_save.cpp - everything about the "-al" mod that can be checked offline.
//
// No game, no loader, milliseconds. What is checked here is everything that decides *what* the
// "-al" mod does: which command lines count, which files in the save folder are saves, and
// which one is the newest. The one step that needs the game (asking it to load) is not touched.
//
// usage: atmt_al_test <scratch dir for the fixtures>
#include "../mods/load_last_save/src/al.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool cond, const char* what) {
    std::printf("%s %s\n", cond ? "[ ok ]" : "[FAIL]", what);
    if (!cond) ++g_failures;
}

// Creates a file with a fixed modification time, so "newest" is exact and nothing has to sleep.
// The wanted time is local (that is what the log prints); SetFileTime takes UTC, so the
// conversion happens here - without it every fixture time came out shifted by the time zone and
// a "newest" assertion could pass for the wrong reason.
static void MakeFile(const std::wstring& dir, const wchar_t* name, SYSTEMTIME local,
                     unsigned size = 64) {
    const std::wstring path = dir + L"\\" + name;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<char> buf(size, 'x');
    DWORD written = 0;
    WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr);
    SYSTEMTIME utc;
    FILETIME ft;
    if (TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) != 0
        && SystemTimeToFileTime(&utc, &ft) != 0) {
        SetFileTime(h, nullptr, nullptr, &ft);
    }
    CloseHandle(h);
}

static SYSTEMTIME Time(int year, int month, int day, int hour, int minute) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    return st;
}

static std::wstring Leaf(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring root = L"dist";
    if (argc > 1) {
        std::string s(argv[1]);
        root.assign(s.begin(), s.end());
    }

    // --- which command lines count ----------------------------------------------------
    // The switch is read off the process command line, so the parsing has to survive quoting
    // and must not fire on a longer token that merely starts with it.
    const std::wstring exe = L"\"U:\\Program Files (x86)\\Steam\\steamapps\\common\\Trails of "
                             L"Cold Steel\\ed8.exe\"";
    Check(al::HasSwitch(exe + L" -al", L"-al"), "-al at the end of the command line counts");
    Check(al::HasSwitch(exe + L" -al -window", L"-al"), "-al before another switch counts");
    Check(al::HasSwitch(L"ed8.exe -window -al", L"-al"), "-al after another switch counts");
    Check(al::HasSwitch(L"ed8.exe -AL", L"-al"), "the switch is case-insensitive");
    Check(!al::HasSwitch(exe, L"-al"), "no switch, no action");
    Check(!al::HasSwitch(L"ed8.exe -alan", L"-al"), "-alan is not -al");
    Check(!al::HasSwitch(L"ed8.exe --al", L"-al"), "--al is not -al");
    Check(!al::HasSwitch(L"ed8.exe", L"-al"), "a bare executable path is not a switch");
    Check(!al::HasSwitch(L"", L"-al"), "an empty command line is handled");
    Check(al::HasSwitch(L"ed8.exe \"-al\"", L"-al"), "a quoted switch still counts");

    // --- Autoload: when a start loads -------------------------------------------------
    {
        al::Options o;
        Check(o.autoload == al::Autoload::kAlways, "Autoload defaults to always");
        Check(al::WantsLoad(o, L"ed8.exe"), "the default loads without -al");
        o.autoload = al::Autoload::kParameter;
        Check(al::WantsLoad(o, L"ed8.exe -al") && !al::WantsLoad(o, L"ed8.exe"),
              "parameter: loads with -al, not without");
        o.autoload = al::Autoload::kAlways;
        Check(al::WantsLoad(o, L"ed8.exe"), "always: loads without -al");
        o.autoload = al::Autoload::kDisabled;
        Check(!al::WantsLoad(o, L"ed8.exe -al"), "disabled: never loads, -al or not");
        o.autoload = al::Autoload::kAlways;
        o.enabled = false;
        Check(!al::WantsLoad(o, L"ed8.exe -al"), "Enabled=false wins over Autoload=always");
    }

    // --- which files in the save folder are saves -------------------------------------
    // The folder mixes saves with their headers, thumbnails and metadata; only two name shapes
    // are saves. Getting this wrong is the most likely way for the feature to pick a bad file.
    Check(al::IsSaveFileName(L"save000.dat"), "save000.dat is a save");
    Check(al::IsSaveFileName(L"save129.dat"), "save129.dat is a save");
    Check(al::IsSaveFileName(L"SAVE001.DAT"), "the name check ignores case");
    Check(al::IsSaveFileName(L"autosave00.dat"), "autosave00.dat is a save");
    Check(!al::IsSaveFileName(L"save000_t.dat"), "a _t header is not a save");
    Check(!al::IsSaveFileName(L"autosave03_t.dat"), "an autosave header is not a save");
    Check(!al::IsSaveFileName(L"thumb000.bmp"), "a thumbnail is not a save");
    Check(!al::IsSaveFileName(L"sdslot.dat"), "the slot metadata is not a save");
    Check(!al::IsSaveFileName(L"steam_autocloud.vdf"), "steam's file is not a save");
    Check(!al::IsSaveFileName(L"save00.dat"), "the game uses three digits, not two");
    Check(!al::IsSaveFileName(L"autosave1.dat"), "the game uses two digits for autosaves");
    Check(!al::IsSaveFileName(L"saved_game.dat"), "a name that merely starts with save is not one");

    // --- which save is the newest -----------------------------------------------------
    {
        const std::wstring dir = root + L"\\al_test_saves";
        CreateDirectoryW(dir.c_str(), nullptr);
        // a fixture folder shaped like the game's, decoys included
        MakeFile(dir, L"save000.dat", Time(2026, 3, 24, 5, 27));
        MakeFile(dir, L"save001.dat", Time(2026, 4, 14, 4, 28));
        MakeFile(dir, L"save002.dat", Time(2026, 4, 14, 4, 30));
        MakeFile(dir, L"autosave00.dat", Time(2026, 9, 21, 6, 56));
        MakeFile(dir, L"autosave01.dat", Time(2026, 9, 21, 7, 25));      // newest of all
        MakeFile(dir, L"autosave02.dat", Time(2026, 9, 20, 22, 50));
        MakeFile(dir, L"autosave01_t.dat", Time(2026, 9, 21, 7, 25), 1536);   // decoys
        MakeFile(dir, L"save001_t.dat", Time(2026, 9, 21, 8, 0), 1536);
        MakeFile(dir, L"thumb000.bmp", Time(2026, 9, 21, 9, 0), 4096);
        MakeFile(dir, L"sdslot.dat", Time(2026, 9, 21, 9, 30), 368640);
        MakeFile(dir, L"steam_autocloud.vdf", Time(2026, 9, 21, 10, 0), 51);

        const std::vector<al::SaveFile> all = al::FindSaves(dir, true);
        Check(all.size() == 6, "exactly the six save files are found, no decoy");
        // The whole order, so a wrong comparison cannot be hidden by a lucky subset: the
        // autosaves are newest here, then the April manual saves, then the March one.
        std::wstring order;
        for (const al::SaveFile& s : all) {
            if (!order.empty()) order += L", ";
            order += Leaf(s.path);
        }
        Check(order == L"autosave01.dat, autosave00.dat, autosave02.dat, save002.dat, "
                        L"save001.dat, save000.dat",
              "the saves come out in exactly the expected order (newest first)");
        Check(!all.empty() && Leaf(all.front().path) == L"autosave01.dat",
              "the newest save wins (the newest autosave here)");

        const std::vector<al::SaveFile> manual = al::FindSaves(dir, false);
        Check(!manual.empty() && Leaf(manual.front().path) == L"save002.dat",
              "with autosaves excluded the newest manual save wins");
        bool any_auto = false;
        for (const al::SaveFile& s : manual) {
            if (_wcsnicmp(Leaf(s.path).c_str(), L"autosave", 8) == 0) any_auto = true;
        }
        Check(!any_auto, "no autosave survives IncludeAutosaves=false");

        // Equal times must not be a coin flip: the order has to be the same on every run.
        const std::wstring tie = root + L"\\al_test_tie";
        CreateDirectoryW(tie.c_str(), nullptr);
        MakeFile(tie, L"save005.dat", Time(2026, 5, 1, 12, 0));
        MakeFile(tie, L"save007.dat", Time(2026, 5, 1, 12, 0));
        const std::vector<al::SaveFile> tied = al::FindSaves(tie, true);
        Check(tied.size() == 2 && Leaf(tied[0].path) == L"save007.dat",
              "equal times break by name, deterministically");

        // An empty or missing folder is a report, not a crash.
        const std::wstring empty = root + L"\\al_test_empty";
        CreateDirectoryW(empty.c_str(), nullptr);
        Check(al::FindSaves(empty, true).empty(), "an empty save folder yields no saves");
        Check(al::FindSaves(L"Z:\\no_such_folder_at_all", true).empty(),
              "a missing save folder yields no saves");
    }

    // --- the settings file ------------------------------------------------------------
    {
        const std::wstring ini = root + L"\\_al_test.ini";
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            std::fprintf(f, "[General]\nEnabled=true\nAutoload=always\nIncludeAutosaves=false\n"
                            "Diagnostics=true   ; list every candidate\n"
                            "ProbeFileApis=true\n"
                            "ProbeReader=true\n"
                            "ProbeWriters=true\n"
                            "ProbeFade=true\n"
                            "PadScript=pad_script.txt\n"
                            "SaveDir=C:\\atmt_test\\saves\n");
            std::fclose(f);
        }
        const al::Options o = al::LoadOptions(ini);
        Check(o.enabled, "Enabled is read");
        Check(o.autoload == al::Autoload::kAlways, "Autoload is read");
        Check(!o.include_autosaves, "IncludeAutosaves is read");
        Check(o.diagnostics, "Diagnostics is read, inline comment and all");
        Check(o.probe_file_apis, "the file-call probe switch is read");
        Check(o.probe_reader, "the save-reader probe switch is read");
        Check(o.probe_writers, "the writer-probe switch is read");
        Check(o.probe_fade, "the fade-probe switch is read");
        Check(o.pad_script == "pad_script.txt", "the pad script name is read");
        Check(o.save_dir == L"C:\\atmt_test\\saves",
              "SaveDir is read (this is what keeps a test run out of the real save folder)");
        const al::Options missing = al::LoadOptions(root + L"\\_no_such_al.ini");
        Check(missing.enabled && missing.include_autosaves && missing.save_dir.empty(),
              "a missing settings file keeps the defaults");
        _wremove(ini.c_str());
    }

    // The regression test for the crash: a snapshot whose stack ends right at the edge of a mapped
    // page. The scan must stop there instead of reading into the unmapped page (offset 0x6c04 in
    // the old build was exactly that read).
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const size_t page = si.dwPageSize;
        unsigned char* region = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (region != nullptr) {
            DWORD old = 0;
            VirtualProtect(region + page, page, PAGE_NOACCESS, &old);
            uintptr_t* frame = reinterpret_cast<uintptr_t*>(region + page - 8);   // last 8 bytes
            frame[0] = 0;   // "saved EBP": stops the chain immediately
            frame[1] = 0;   // return address
            uintptr_t snap[12] = {0};
            snap[1 + 2] = reinterpret_cast<uintptr_t>(frame);        // saved EBP
            snap[1 + 3] = reinterpret_cast<uintptr_t>(frame) - 4;    // saved ESP (entry-4)
            const std::string frames = al::HookStackFrames(snap, 6);
            Check(frames.empty(), "the frame scan stops at the end of the mapped stack");
            VirtualFree(region, 0, MEM_RELEASE);
        }
    }

        // --- the pad script parser (test-only input; see src/pad_inject.cpp) -----------------
        // The game ignores injected keystrokes, so reaching a menu screen without a human needs input
        // at the API level; the script is what says which button to hold and when.
        {
            unsigned at = 0;
            unsigned button = 0;
            bool is_exit = false;
            Check(al::PadParseEntry("3000 A", &at, &button, &is_exit) && at == 3000 && button == 0x1000,
                  "a pad script line with a space is parsed");
            Check(al::PadParseEntry("4500,DOWN", &at, &button, &is_exit) && at == 4500
                      && button == 0x0002,
                  "a pad script line with a comma is parsed");
            Check(al::PadParseEntry("9000 EXIT", &at, &button, &is_exit) && is_exit,
                  "the EXIT entry is recognised");
            Check(!al::PadParseEntry("; a comment", &at, &button, &is_exit),
                  "a comment line is not a pad entry");
            Check(!al::PadParseEntry("3000 NOPE", &at, &button, &is_exit),
                  "an unknown button name is rejected");
            Check(al::PadParseEntry("3000 b", &at, &button, &is_exit) && button == 0x2000,
                  "a button name is case-insensitive");
            // A leading space used to split the line at index 0: the time became 0 and the button name
            // "5000 A", so a hand-written file whose only line was indented looked empty (2026-09-22).
            Check(al::PadParseEntry(" 5000 A", &at, &button, &is_exit) && at == 5000
                      && button == 0x1000,
                  "a leading space does not stop a pad entry from parsing");
            Check(al::PadParseEntry("\t6000\tdown", &at, &button, &is_exit) && at == 6000
                      && button == 0x0002,
                  "a leading tab does not stop a pad entry from parsing");
        }

    // --- the slot number the game's own format produces, and the system file -----------------
    // The game builds the save's file name as "save%03d.dat" from the manager's slot field. Two bugs
    // lived here: a length guard that rejected every real name ("save063.dat" is 11 characters), and
    // save511.dat - the game's own ~52-byte system file - being treated as a loadable save.
    Check(al::SaveSlotNumber("save063.dat") == 63, "save063.dat is slot 63");
    Check(al::SaveSlotNumber("save000.dat") == 0, "save000.dat is slot 0");
    Check(al::SaveSlotNumber("save511.dat") == 511, "save511.dat still parses as slot 511");
    Check(al::SaveSlotNumber("autosave01.dat") == al::kNoSlot,
          "an autosave is not a slot the game's format produces");
    Check(al::SaveSlotNumber("save.dat") == al::kNoSlot, "a name without digits is not a slot");
    Check(al::SaveSlotNumber("save06x.dat") == al::kNoSlot, "a non-digit in the number is rejected");
    Check(al::SaveSlotNumber("save00.dat") == 0, "a shorter digit group still parses");
    // What the title route tells the load menu (ParseSaveTarget): manual slot or autosave tab + number.
    {
        const al::SaveTarget manual = al::ParseSaveTarget("save063.dat");
        Check(manual.slot == 63 && !manual.autosave, "save063.dat -> manual slot 63");
        const al::SaveTarget autosave = al::ParseSaveTarget("autosave03.dat");
        Check(autosave.slot == 3 && autosave.autosave, "autosave03.dat -> autosave slot 3");
        const al::SaveTarget upper = al::ParseSaveTarget("AUTOSAVE07.DAT");
        Check(upper.slot == 7 && upper.autosave, "autosave names are case-insensitive");
        Check(al::ParseSaveTarget("autosave03_t.dat").slot == al::kNoSlot, "an autosave header is not a target");
        Check(al::ParseSaveTarget("save511.dat").slot == al::kNoSlot, "the system file is not a target");
        Check(al::ParseSaveTarget("autosave.dat").slot == al::kNoSlot, "autosave without a number is not a target");
        Check(al::ParseSaveTarget("thumb003.bmp").slot == al::kNoSlot, "a thumbnail is not a target");
    }
    Check(!al::IsSaveFileName(L"save511.dat"), "the game's system file is not a save to load");
    Check(al::IsSaveFileName(L"save063.dat"), "a normal save is a save");
    Check(!al::IsSaveFileName(L"save511_t.dat"), "the system file's thumbnail is not a save");


        std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                    g_failures);
        return g_failures == 0 ? 0 : 1;
    }

