// test_settings.cpp - the settings registry (loader/src/loader_settings.cpp) and the
// in-place ini writer it saves through (shared/ini.h), without the game or a mod dll.
//
// Usage: atmt_settings_test <scratch dir>   (the ini files it writes land there)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "ini.h"
#include "loader.h"
#include "mod_api.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failures;
}

std::wstring g_dir;

std::wstring PathOf(const wchar_t* leaf) { return g_dir + L"\\" + leaf; }

void WriteFile(const std::wstring& path, const char* content) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    std::fwrite(content, 1, std::strlen(content), f);
    std::fclose(f);
}

std::string ReadFile(const std::wstring& path) {
    std::string out;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------- shared/ini.h
void TestIniReadWrite() {
    const std::wstring path = PathOf(L"ini_rw.ini");
    WriteFile(path,
              "; header comment\r\n"
              "Top=1\r\n"
              "[General]\r\n"
              "; speed, documented\r\n"
              "Speed=2.0   ; inline comment\r\n"
              "Name=a;b\r\n"
              "\r\n"
              "[Other]\r\n"
              "Speed=9\r\n");

    char buf[64];
    atmt_ini::Read(path.c_str(), "General", "Speed", buf, sizeof(buf), nullptr);
    Check(std::strcmp(buf, "2.0") == 0, "ini: Read stops at an inline comment");
    atmt_ini::Read(path.c_str(), "General", "Name", buf, sizeof(buf), nullptr);
    Check(std::strcmp(buf, "a;b") == 0, "ini: Read keeps a ';' that is not a comment");
    Check(atmt_ini::ReadInt(path.c_str(), nullptr, "Top", 0) == 1,
          "ini: a NULL section reads the keys before the first header");
    Check(atmt_ini::ReadInt(path.c_str(), "Other", "Speed", 0) == 9, "ini: sections stay apart");

    const atmt_ini::Entry general[] = {
        {"speed", "3", "never used: the key exists"},
        {"Added", "yes", "what Added does\nsecond line"},
    };
    Check(atmt_ini::WriteValues(path.c_str(), "General", general, 2), "ini: WriteValues succeeds");
    const atmt_ini::Entry fresh[] = {{"NewKey", "5", ""}};
    atmt_ini::WriteValues(path.c_str(), "Fresh", fresh, 1);
    const atmt_ini::Entry top[] = {{"Top", "7", ""}, {"TopNew", "8", ""}};
    atmt_ini::WriteValues(path.c_str(), nullptr, top, 2);

    const std::string after = ReadFile(path);
    Check(Contains(after, "Speed=3   ; inline comment\r\n"),
          "ini: a value is replaced in place, its inline comment kept");
    Check(Contains(after, "; speed, documented\r\n"), "ini: comment lines survive");
    Check(Contains(after, "Name=a;b\r\n; what Added does\r\n; second line\r\nAdded=yes\r\n\r\n[Other]"),
          "ini: a missing key goes at the end of its section, with its comment above");
    Check(Contains(after, "[Other]\r\nSpeed=9\r\n"), "ini: another section's same key is untouched");
    Check(Contains(after, "\r\n[Fresh]\r\nNewKey=5\r\n"), "ini: a missing section is appended");
    Check(Contains(after, "; header comment\r\nTop=7\r\nTopNew=8\r\n[General]"),
          "ini: the section-less part is written before the first header");

    // A comment block that introduces the next section stays with it; the comment right under the
    // last key stays with that key.
    const std::wstring intro = PathOf(L"ini_intro.ini");
    WriteFile(intro,
              "[General]\r\n"
              "A=1\r\n"
              "; about A\r\n"
              "\r\n"
              "; ---- the next section ----\r\n"
              "[Next]\r\n"
              "B=2\r\n");
    const atmt_ini::Entry c[] = {{"C", "3", "about C"}};
    atmt_ini::WriteValues(intro.c_str(), "General", c, 1);
    Check(ReadFile(intro) == "[General]\r\nA=1\r\n; about A\r\n; about C\r\nC=3\r\n\r\n"
                             "; ---- the next section ----\r\n[Next]\r\nB=2\r\n",
          "ini: a new key goes after the last key and its own comment, not the next section's");

    const std::wstring lf = PathOf(L"ini_lf.ini");
    WriteFile(lf, "[General]\nA=1");   // LF only, and no line ending at the end
    const atmt_ini::Entry b[] = {{"B", "2", ""}};
    atmt_ini::WriteValues(lf.c_str(), "General", b, 1);
    Check(ReadFile(lf) == "[General]\nA=1\nB=2\n", "ini: an LF file stays LF, the last line is closed");

    const std::wstring none = PathOf(L"ini_new.ini");
    DeleteFileW(none.c_str());
    atmt_ini::WriteValues(none.c_str(), "General", b, 1);
    Check(ReadFile(none) == "[General]\r\nB=2\r\n", "ini: a missing file is created");
}

// ---------------------------------------------------------------- the registry
// A mod's storage, as a mod would keep it: plain globals the hooks read.
int32_t g_enabled = 1;
int32_t g_count = 10;
float g_speed = 2.0f;
int32_t g_mode = 2;
char g_name[16] = "default";
int g_changes = 0;
int g_actions = 0;
const char* const kModes[] = {"game", "off", "modern"};

void __cdecl OnChange(const AtmtSetting*, void* user) { ++*static_cast<int*>(user); }

void MakeTable(AtmtSetting* t) {
    std::memset(t, 0, sizeof(AtmtSetting) * 7);
    t[0] = {ATMT_SETTING_BOOL, ATMT_SETTING_RESTART, nullptr, "Enabled", "Enabled", "on/off", &g_enabled};
    t[1] = {ATMT_SETTING_INT, ATMT_SETTING_LIVE, nullptr, "Count", nullptr, "how many", &g_count, 0, 1, 100, 1};
    t[2] = {ATMT_SETTING_FLOAT, ATMT_SETTING_LIVE, nullptr, "Speed", "Speed", "how fast", &g_speed, 0, 0.5f, 4.0f, 0.1f};
    t[3] = {ATMT_SETTING_ENUM, ATMT_SETTING_LIVE, "Camera", "Mode", "Mode", "what it does", &g_mode, 0, 0, 0, 0, kModes, 3};
    t[4] = {ATMT_SETTING_STRING, 0, nullptr, "Name", "Name", nullptr, g_name, sizeof(g_name)};
    t[5] = {ATMT_SETTING_ACTION, 0, nullptr, nullptr, "Reset"};
    t[6] = {ATMT_SETTING_LABEL, 0, nullptr, nullptr, nullptr};
    t[1].on_change = &OnChange;
    t[1].user = &g_changes;
    t[5].on_change = &OnChange;
    t[5].user = &g_actions;
}

void TestRegistry() {
    using namespace atmt_loader;
    const std::wstring path = PathOf(L"mod.ini");
    WriteFile(path,
              "[General]\r\n"
              "Enabled=false\r\n"
              "Count=500   ; clamped to 100\r\n"
              "Speed=fast\r\n"
              "Name=a much too long name for the buffer\r\n");

    AtmtSetting table[7];
    MakeTable(table);
    const int owner = 0;   // any unique address will do as the mod's identity

    uint32_t count = 0, gen0 = 0, gen1 = 0;
    SettingsAcquire(&count, &gen0);
    SettingsRelease();

    Check(SettingsRegister(&owner, "fake_mod", path, "Fake", "Fakes things.", table, 7) == 7, "registry: register");
    Check(g_enabled == 0, "registry: a bool is loaded from the ini");
    Check(g_count == 100, "registry: an int from the ini is clamped");
    Check(g_speed == 2.0f, "registry: an invalid float keeps the default");
    Check(g_mode == 2, "registry: a missing key keeps the default");
    Check(std::strcmp(g_name, "a much too long") == 0, "registry: a string is truncated to its buffer");

    const std::string ini = ReadFile(path);
    Check(Contains(ini, "\r\n[Camera]\r\n; what it does\r\nMode=modern\r\n"),
          "registry: a missing key is added to the ini with its help above it");
    Check(Contains(ini, "Count=500   ; clamped to 100"), "registry: registering does not rewrite present keys");

    const AtmtSettingsGroup* groups = SettingsAcquire(&count, &gen1);
    Check(count == 1 && groups != nullptr && std::strcmp(groups[0].title, "Fake") == 0
              && std::strcmp(groups[0].mod_name, "fake_mod") == 0 && groups[0].count == 7
              && std::strcmp(groups[0].description, "Fakes things.") == 0,
          "registry: acquire lists the table");
    Check(gen1 != gen0, "registry: the generation moves on register");
    const AtmtSetting* copy = groups[0].settings;

    // set through the host's copy (from within an acquire: the lock is recursive)
    const float faster = 9.0f;
    Check(SettingsSet(&copy[2], &faster) == 1 && g_speed == 4.0f, "registry: set clamps a float");
    SettingsRelease();
    const int32_t same = 100;
    Check(SettingsSet(&copy[1], &same) == 0 && g_changes == 0,
          "registry: setting the current value changes nothing and calls nothing");
    const int32_t fifty = 50;
    // through the mod's own table: matched by its value pointer
    Check(SettingsSet(&table[1], &fifty) == 1 && g_count == 50 && g_changes == 1,
          "registry: set through the mod's own entry, on_change called");
    const int32_t bad_mode = 7;
    Check(SettingsSet(&copy[3], &bad_mode) == 0 && g_mode == 2, "registry: an out-of-range enum is refused");
    Check(SettingsSet(&copy[5], nullptr) == 0 && g_actions == 1, "registry: an action calls on_change");
    Check(SettingsSet(&copy[6], nullptr) == 0, "registry: a label does nothing");
    AtmtSetting stranger = {};
    int32_t nowhere = 0;
    stranger.value = &nowhere;
    Check(SettingsSet(&stranger, &fifty) == -1, "registry: an unknown setting is refused");

    Check(SettingsCommit() == 2, "registry: commit writes the two changed values");
    const std::string saved = ReadFile(path);
    Check(Contains(saved, "Count=50   ; clamped to 100\r\n") && Contains(saved, "Speed=4\r\n"),
          "registry: commit replaced the values in place");
    Check(SettingsCommit() == 0, "registry: a second commit has nothing to write");

    // re-register (a dev reload): replaces rather than adds
    MakeTable(table);
    SettingsRegister(&owner, "fake_mod", path, "Fake again", nullptr, table, 7);
    groups = SettingsAcquire(&count, nullptr);
    Check(count == 1 && std::strcmp(groups[0].title, "Fake again") == 0 && groups[0].description[0] == 0,
          "registry: registering again replaces the table");
    SettingsRelease();
    Check(g_speed == 4.0f && g_count == 50, "registry: re-registering reads the saved values back");

    // unregister saves what is still unsaved
    const int32_t off = 0;
    SettingsSet(&table[3], &off);
    SettingsUnregister(&owner);
    groups = SettingsAcquire(&count, nullptr);
    SettingsRelease();
    Check(count == 0, "registry: unregister drops the table");
    Check(Contains(ReadFile(path), "Mode=game\r\n"), "registry: unregister commits unsaved changes");

    // one invalid entry refuses the whole table
    AtmtSetting broken[2];
    MakeTable(table);
    broken[0] = table[0];
    broken[1] = table[3];
    broken[1].choices = nullptr;
    Check(SettingsRegister(&owner, "fake_mod", path, "Broken", nullptr, broken, 2) == -1,
          "registry: an enum without choices refuses the table");
    SettingsAcquire(&count, nullptr);
    SettingsRelease();
    Check(count == 0, "registry: nothing of a refused table is registered");
}

}  // namespace

int main(int argc, char** argv) {
    wchar_t dir[MAX_PATH] = {};
    if (argc > 1) {
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH);
    } else {
        GetTempPathW(MAX_PATH, dir);
        wcscat(dir, L"atmt_settings_test");
    }
    CreateDirectoryW(dir, nullptr);
    g_dir = dir;

    TestIniReadWrite();
    TestRegistry();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
