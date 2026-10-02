// test_logger.cpp - offline checks for what the logger still has.
//
// The logger has one source (the game's message setter) and one job (write the line), so the
// checks here are about the parts that can be wrong without the game: the ini reader, the
// text helpers, and the log sink - including the one case that waits, a line whose name
// plate the game writes a moment later.
//
// usage: atmt_selftest <dir for the log files>
#include "../src/atmt.h"

#include "../src/itf_font.h"
#include "../src/pad.h"   // PadButtonFromName/PadButtonName: the pad's names, without the game

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace atmt;

// The detour's entry point, so the test can drive the probe with a fabricated frame.
extern "C" void atmt_probe_message(void* snapshot);

static int g_failures = 0;

static void Check(bool cond, const char* what) {
    printf("%s %s\n", cond ? "[ ok ]" : "[FAIL]", what);
    if (!cond) ++g_failures;
}

// Scribbles on the stack, so a test can prove that a value survives the frame that made it
// (a dangling pointer into a dead frame is a real bug this project hit).
static void ClobberStack(size_t bytes) {
    std::vector<unsigned char> buf(bytes, 0xAB);
    volatile unsigned char sink = 0;
    for (size_t i = 0; i < buf.size(); i += 4096) sink = static_cast<unsigned char>(sink + buf[i]);
    (void)sink;
}

// A whole log file, so a test can look at what was written.
static std::string ReadAll(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (f == nullptr) return std::string();
    std::string all;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);
    return all;
}

// The last line of a log file.
static std::string LastLogLine(const std::wstring& path) {
    const std::string all = ReadAll(path);
    const size_t end = all.find_last_not_of("\r\n");
    if (end == std::string::npos) return std::string();
    size_t start = all.find_last_of('\n', end);
    start = (start == std::string::npos) ? 0 : start + 1;
    return all.substr(start, end - start + 1);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring dir = L"dist";
    if (argc > 1) {
        std::string s(argv[1]);
        dir.assign(s.begin(), s.end());
    }

    // --- code stripping: what the player reads ---------------------------------------
    // `#<digits><Letter>` is one code (#0T, #1P, #5S); without that rule "#1P*yawn*" came
    // out as "P*yawn*" and the yawn line looked wrong.
    Check(StripCodes("#KHello there,\r\nfriend!") == "Hello there, friend!",
          "inline codes and newlines are stripped");
    Check(StripCodes("#E6#MAI'm still here.") == "I'm still here.", "multiple codes stripped");
    Check(StripCodes("#MIAStill here.") == "Still here.", "longer code with word guard");
    Check(StripCodes("#M[[autoM0]]A-Alisa?!") == "A-Alisa?!", "bracketed code argument");
    Check(StripCodes("#0TSeems like everyone") == "Seems like everyone", "digit code + word");
    Check(StripCodes("#1P*yawn*") == "*yawn*", "a digit code keeps its letter");
    Check(StripCodes("#5SDad?!") == "Dad?!", "the letter after a digit code is not text");
    Check(StripCodes("heading\x01out, too?") == "heading out, too?",
          "the game's line break becomes a space (raw keeps it)");

    // --- executable-code guard ------------------------------------------------------
    // Hooks are only installed on committed executable memory; a stale or mistyped
    // MessageHook must not be able to patch data.
    Check(IsExecutableCode(reinterpret_cast<const void*>(&StripCodes)),
          "this module's code counts as executable");
    std::vector<unsigned char> heap(64, 0);
    Check(!IsExecutableCode(heap.data()), "heap memory is not executable code");
    Check(!IsExecutableCode(nullptr), "a null address is not executable code");

    // --- the ini reader -------------------------------------------------------------
    {
        const std::wstring ini = dir + L"\\_atmt_test_config.ini";
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            std::fprintf(f, "; test config\n[General]\nEnabled=false\n"
                            "LogToFile=true\n"
                            "MessageHook=0x701E50\nMessagePlateOffset=0x358\n"
                            "MessageTextSkip=5\nPendingSpeakerMs=250\n"
                            "Diagnostics=true\n"
                            "SomeOldKey=1\n");
            std::fclose(f);
        }
        const Config loaded = LoadConfig(ini);
        Check(!loaded.enabled, "Enabled is read");
        Check(loaded.log_to_file, "LogToFile is read");
        Check(!Config().log_to_file, "the default is to write no files");
        Check(loaded.diagnostics, "the diagnostics switch is read");
        Check(loaded.unknown_keys == "SomeOldKey",
              "a key the mod does not know is reported instead of being ignored silently");
        Check(loaded.message_hook_address == 0x701E50, "the hook address is read");
        Check(loaded.message_plate_offset == 0x358, "the plate offset is read");
        Check(loaded.message_text_skip == 5, "the text skip is read");
        Check(loaded.pending_speaker_ms == 250, "the wait for the plate is read");
        // The shipped ini documents each setting with an inline comment; the parser must stop
        // the value at the semicolon instead of handing "5       ; why" to the callers.
        const std::wstring ini2 = dir + L"\\_atmt_test_config2.ini";
        if (FILE* f = _wfopen(ini2.c_str(), L"wb")) {
            std::fprintf(f, "[General]\n"
                            "MessageTextSkip=7      ; bytes after the `11` opcode\n"
                            "Enabled=true ; master switch\n"
                            "MessageTextSkip2=9\n"
                            "OverlayLines=99\n"
                            "[Overlay]\n"
                            "Diagnostics=true\n");
            std::fclose(f);
        }
        const Config inline_comments = LoadConfig(ini2);
        Check(inline_comments.message_text_skip == 7, "an inline comment after a number is ignored");
        Check(inline_comments.overlay_lines == Config().overlay_lines && !inline_comments.diagnostics
                  && inline_comments.misplaced_keys
                         == "OverlayLines (in [General], belongs in [Overlay]), Diagnostics (in [Overlay], belongs in [General])",
              "a key outside its section is not read, and named (the settings registry would not see it either)");
        _wremove(ini2.c_str());
        const Config missing = LoadConfig(dir + L"\\_no_such_config.ini");
        Check(missing.message_hook_address == 0x701E50,
              "a missing config file keeps the built-in default (the shipped address)");
        _wremove(ini.c_str());
    }

    // --- the overlay's settings -------------------------------------------------------
    // Every key the panel reads, in one file: a setting that is silently ignored is this project's
    // recurring failure mode (MessageTextSkip cost an evening once).
    {
        const std::wstring ini = dir + L"\\_atmt_test_overlay.ini";
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            std::fprintf(f, "[Overlay]\nOverlay=true\nOverlayKey=page up\nOverlayLines=250\n"
                            "OverlayCapture=false\nOverlayTimestamps=false\nOverlayMouse=false\n"
                            "OverlayAnchor=TopRight\nOverlayWidthPct=33\nOverlayHeightPct=120\n"
                            "OverlayOpacityPct=80\nOverlayDimPct=0\nOverlayFontSize=22\n"
                            "OverlayNewestFirst=true\n"
                            "OverlayFontPath=C:\\fonts\\jpn.ttf\nOverlayPadClose=back\n"
                            "OverlayPadToggle=Y\nDevReload=true\nNoSuchSetting=1\n"
                            "SettingsKey=F10\nSettingsPadToggle=back+start\n");
            std::fclose(f);
        }

        const Config c = LoadConfig(ini);
        Check(c.overlay, "Overlay is read");
        Check(c.overlay_key == "page up" && c.overlay_key_chord == 0x21 && c.overlay_key_known,
              "OverlayKey resolves a name (spelling and case do not matter)");
        Check(c.overlay_lines == 250, "OverlayLines is read");
        Check(c.overlay_newest_first, "OverlayNewestFirst is read");
        Check(!Config().overlay_newest_first,
              "the default order is the chronicle: oldest first, the newest line at the bottom");
        Check(!c.overlay_capture, "OverlayCapture is read");
        Check(!c.overlay_timestamps, "OverlayTimestamps is read");
        Check(c.overlay_anchor == "TopRight", "OverlayAnchor is read");
        Check(c.overlay_width_pct == 33, "OverlayWidthPct is read");
        Check(c.overlay_height_pct == 100, "a percentage above 100 is clamped, not believed");
        Check(c.overlay_opacity_pct == 80 && c.overlay_dim_pct == 0, "the alpha percentages are read");
        Check(c.overlay_font_size == 22, "OverlayFontSize is read");
        Check(c.overlay_pad_close == "back" && c.overlay_pad_toggle == "Y", "the pad buttons are read");
        Check(c.dev_reload, "DevReload is read");
        Check(c.unknown_keys == "NoSuchSetting", "an unknown overlay key is reported like any other");
        Check(c.moved_keys == "OverlayMouse, OverlayFontPath, SettingsKey, SettingsPadToggle",
              "the keys that moved to atmt_overlay.ini are named as moved, not as unknown");
        const Config defaults = Config();
        Check(defaults.overlay && defaults.overlay_key_chord == 0x72 && !defaults.dev_reload,
              "the defaults are: panel on, F3, no dev reload");
        Check(defaults.overlay_font_size == 20,
              "the default text size is a readable one (it is what the panel shows before the ini)");
        _wremove(ini.c_str());

        // A text size out of range is clamped, like every other percentage here: a panel drawn at
        // 400 px, or at 2 px, is not a setting anyone meant to write.
        const std::wstring big = dir + L"\\_atmt_test_overlay_font.ini";
        if (FILE* f = _wfopen(big.c_str(), L"wb")) {
            std::fprintf(f, "[Overlay]\nOverlayFontSize=400\n");
            std::fclose(f);
        }
        Check(LoadConfig(big).overlay_font_size == kOverlayFontSizeMax, "a text size above the range is clamped");
        if (FILE* f = _wfopen(big.c_str(), L"wb")) {
            std::fprintf(f, "[Overlay]\nOverlayFontSize=2\n");
            std::fclose(f);
        }
        Check(LoadConfig(big).overlay_font_size == 8, "a text size below the range is clamped");
        _wremove(big.c_str());

        // A key name nobody knows must not leave a panel that can never be opened: F3 is used and
        // the name is reported (the overlay writes it into the log).
        const std::wstring bad = dir + L"\\_atmt_test_overlay_bad.ini";
        if (FILE* f = _wfopen(bad.c_str(), L"wb")) {
            std::fprintf(f, "[Overlay]\nOverlayKey=nosuchkey\n");
            std::fclose(f);
        }
        const Config fallback = LoadConfig(bad);
        Check(!fallback.overlay_key_known && fallback.overlay_key_chord == 0x72,
              "an unknown OverlayKey falls back to F3 instead of never opening");
        _wremove(bad.c_str());

        const std::wstring chord = dir + L"\\_atmt_test_overlay_chord.ini";
        if (FILE* f = _wfopen(chord.c_str(), L"wb")) {
            std::fprintf(f, "[Overlay]\nOverlayKey=Ctrl+Shift+F3   ; a chord\n");
            std::fclose(f);
        }
        const Config with_chord = LoadConfig(chord);
        Check(with_chord.overlay_key_known && with_chord.overlay_key_chord == (kChordCtrl | kChordShift | 0x72),
              "OverlayKey takes a chord");
        _wremove(chord.c_str());
    }

    // --- key names ---------------------------------------------------------------------
    Check(KeyFromName("F3") == 0x72 && KeyFromName("f3") == 0x72, "F3 is F3, whatever the case");
    Check(KeyFromName("F1") == 0x70 && KeyFromName("F12") == 0x7B, "the F-keys are in range");
    Check(KeyFromName("page up") == 0x21 && KeyFromName("PageDown") == 0x22, "named keys");
    Check(KeyFromName("`") == 0xC0 && KeyFromName("GRAVE") == 0xC0, "the key above tab");
    Check(KeyFromName("A") == 'A' && KeyFromName("7") == '7', "letters and digits are their own key");
    Check(KeyFromName("F25") == 0 && KeyFromName("nosuchkey") == 0 && KeyFromName("") == 0,
          "a name that is not a key is refused, so the caller can report it");

    // --- key chords: modifiers + one key, "+" between them, any order and case
    Check(KeyChordFromName("F3") == 0x72, "a plain key is a chord without modifiers");
    Check(KeyChordFromName("Ctrl+F3") == (kChordCtrl | 0x72), "Ctrl+F3");
    Check(KeyChordFromName("alt + shift + l") == (kChordShift | kChordAlt | 'L'),
          "modifiers in any order, spaces and case do not matter");
    Check(KeyChordFromName("Control+Win+page up") == (kChordCtrl | kChordWin | 0x21), "modifier aliases");
    Check(KeyChordFromName("Ctrl") == 0 && KeyChordFromName("Ctrl+") == 0 && KeyChordFromName("F3+F4") == 0
              && KeyChordFromName("Ctrl+nosuchkey") == 0 && KeyChordFromName("") == 0,
          "a modifier alone, two keys, or an unknown part is no chord");
    Check(KeyChordName(kChordAlt | kChordCtrl | 0x72) == "Ctrl+Alt+F3" && KeyChordName(0x21) == "PageUp"
              && KeyChordName(0).empty(),
          "chord names list the modifiers in a fixed order");
    {
        bool round_trip = true;
        for (unsigned mods = 0; mods <= kChordModifiers; mods += kChordCtrl) {
            for (unsigned vk : {0x72u, unsigned('A'), 0x21u, 0xC0u, 0x60u}) {
                if (KeyChordFromName(KeyChordName(mods | vk)) != (mods | vk)) round_trip = false;
            }
        }
        Check(round_trip, "every chord name reads back as the same chord");
    }
    Check(IsModifierKey(0x11) && IsModifierKey(0xA5) && IsModifierKey(0x5B) && !IsModifierKey(0x72),
          "the modifiers (either side, and Win) are never a chord's key");

    // --- the pad's button names --------------------------------------------------------
    Check(PadButtonFromName("B") == kPadB && PadButtonFromName("b") == kPadB, "B is B");
    Check(PadButtonFromName("d-pad down") == kPadDown, "a d-pad direction, spelled out");
    Check(PadButtonFromName("BACK") == kPadBack && PadButtonFromName("select") == kPadBack,
          "back and select are the same button");
    Check(PadButtonFromName("LB") == kPadLB && PadButtonFromName("r1") == kPadRB, "the shoulders");
    Check(PadButtonFromName("L3") == kPadL3 && PadButtonFromName("l stick") == kPadL3
              && PadButtonFromName("Right Stick") == kPadR3,
          "the stick clicks, spelled out or abbreviated");
    Check(PadButtonFromName("") == 0 && PadButtonFromName("turbo") == 0,
          "a button name that is not one is refused");
    Check(std::string(PadButtonName(kPadB)) == "B" && std::string(PadButtonName(kPadUp)) == "UP"
              && std::string(PadButtonName(kPadL3)) == "L3",
          "the button names are for the log as well as for the ini");
    Check(PadChordFromName("L3+R3") == (kPadL3 | kPadR3), "a chord is its buttons together");
    Check(PadChordFromName("back + start") == (kPadBack | kPadStart), "spaces in a chord are fine");
    Check(PadChordFromName("B") == kPadB, "a single button is a chord of one");
    Check(PadChordFromName("") == 0 && PadChordFromName("L3+") == 0
              && PadChordFromName("L3+turbo") == 0,
          "an empty or half-known chord is refused");
    Check(PadChordName(kPadR3 | kPadL3) == "L3+R3" && PadChordName(0).empty(),
          "a chord is named in a fixed order");
    Check(PadChordFromName(PadChordName(kPadA | kPadLB | kPadStart)) == (kPadA | kPadLB | kPadStart),
          "a chord's name reads back as the same chord");

    // --- key names for the settings bar: every name it saves must read back as the same key
    {
        bool round_trip = true;
        int named = 0;
        for (unsigned vk = 0x08; vk <= 0xFE; ++vk) {
            const std::string name = KeyName(vk);
            if (name.empty()) continue;
            ++named;
            if (KeyFromName(name) != vk) {
                std::printf("       KeyName(0x%02x) = %s reads back as 0x%02x\n", vk, name.c_str(),
                            KeyFromName(name));
                round_trip = false;
            }
        }
        Check(round_trip && named > 70, "every key name reads back as the same key");
        Check(KeyName(0x72) == "F3" && KeyName(0x21) == "PageUp" && KeyName(0x10).empty(),
              "key names: F3, PageUp, and none for the generic Shift");
    }

    // --- the panel's reading of the pad (the overlay hands it the raw state) ------------
    {
        AtmtOverlayInput pad;
        memset(&pad, 0, sizeof(pad));
        pad.have_pad = 1;
        SetPadButtons("B", "L3+R3");
        DecodePanelPad(pad);   // nothing held: sets the edges
        pad.pad_buttons = kPadL3;
        Check(!DecodePanelPad(pad).toggle, "half a chord does not toggle the panel");
        pad.pad_buttons = kPadL3 | kPadR3;
        Check(DecodePanelPad(pad).toggle, "the whole chord toggles it");
        Check(!DecodePanelPad(pad).toggle, "holding the chord does not toggle it again");
        pad.pad_buttons = kPadB;
        Check(DecodePanelPad(pad).close, "B closes it");
        pad.pad_buttons = kPadDown;
        Check(DecodePanelPad(pad).lines == 1, "d-pad down scrolls a line");
        SetPadButtons("turbo", "");
        pad.pad_buttons = 0;
        DecodePanelPad(pad);
        pad.pad_buttons = kPadB;
        Check(DecodePanelPad(pad).close, "an unknown close button falls back to B");
    }

    // --- LogToFile=false: the lines still exist, the disk stays untouched ----------------
    // What the setting has to guarantee is both halves: no file, and the panel still filled. The
    // early-outs used to be "the jsonl handle is null", so not opening the file would have blinded the
    // panel as well - which is why this checks the history too.
    {
        const std::wstring quiet_dir = dir + L"\\_atmt_quiet";
        CreateDirectoryW(quiet_dir.c_str(), nullptr);   // "already there" is fine
        const std::wstring quiet_jsonl = quiet_dir + L"\\atmt_dialogs.jsonl";
        const std::wstring quiet_latest = quiet_dir + L"\\atmt_latest.txt";
        const std::wstring quiet_diag = quiet_dir + L"\\atmt_diagnostics.log";
        DeleteFileW(quiet_jsonl.c_str());
        DeleteFileW(quiet_latest.c_str());
        DeleteFileW(quiet_diag.c_str());

        LogSink sink;
        sink.set_write_files(false);
        Check(sink.Open(quiet_dir), "a sink told not to write files opens anyway (in memory)");
        History history;
        history.set_capacity(10);
        sink.set_history(&history);
        CatalogEntry e;
        e.raw = "#KGood morning.";
        e.text = "Good morning.";
        sink.Line(e, 0x1000, "Rean", "test");
        Check(sink.count() == 1, "the line is still counted with LogToFile=false");
        std::vector<LogLine> quiet_lines;
        history.Snapshot(quiet_lines);
        Check(quiet_lines.size() == 1 && quiet_lines[0].speaker == "Rean"
                  && quiet_lines[0].text == "Good morning.",
              "the line still reaches the history the panel reads");
        Check(_wfopen(quiet_jsonl.c_str(), L"rb") == nullptr, "no atmt_dialogs.jsonl is created");
        Check(_wfopen(quiet_latest.c_str(), L"rb") == nullptr, "no atmt_latest.txt is created");
        sink.Note("a note that has nowhere to go");
        sink.Diagnostic("a diagnostic that has nowhere to go");
        Check(_wfopen(quiet_diag.c_str(), L"rb") == nullptr,
              "no atmt_diagnostics.log is created either");
        sink.Close();
    }

    // --- the log sink, and the line that waits for its plate ------------------------
    {
        LogSink sink;
        Check(sink.Open(dir), "the log sink opens");
        sink.set_pending_ms(200);

        CatalogEntry e;
        e.raw = "#KGood morning.";
        e.text = "Good morning.";
        sink.Line(e, 0x1000, "Rean", "test");
        Check(sink.count() == 1, "a line with a known speaker is written at once");
        Check(LastLogLine(dir + L"\\atmt_dialogs.jsonl").find("\"speaker\":\"Rean\"")
                  != std::string::npos,
              "the speaker it was given is written");

        // The game's own object: the plate field is empty at first (the game writes it a
        // moment after the line), so the line waits and the flush reads it again.
        std::vector<unsigned char> obj(0x400, 0);
        const uintptr_t obj_addr = reinterpret_cast<uintptr_t>(obj.data());
        CatalogEntry e2;
        e2.raw = "Erm... Good morning, everyone.";
        e2.text = "Erm... Good morning, everyone.";
        const size_t before = sink.count();
        sink.LineFromObject(e2, obj_addr, 0x358, 0x2000, "message");
        Check(sink.count() == before, "a line whose plate is not written yet waits");
        std::memcpy(obj.data() + 0x358, "Girl's Voice", 13);
        Sleep(300);
        sink.FlushPending();
        Check(sink.count() == before + 1, "the waiting line is written once its wait is over");
        Check(LastLogLine(dir + L"\\atmt_dialogs.jsonl").find("\"speaker\":\"Girl's Voice\"")
                  != std::string::npos,
              "the plate the game wrote is used as the speaker");

        sink.Close();
    }

    // --- the ring the panel shows ------------------------------------------------------
    {
        History history;
        history.set_capacity(3);
        LogLine line;
        for (int i = 1; i <= 5; ++i) {
            line.seq = static_cast<uint64_t>(i);
            line.time = std::string("12:00:0") + std::to_string(i);
            line.text = std::string("line ") + std::to_string(i);
            history.Push(line);
        }
        std::vector<LogLine> lines;
        history.Snapshot(lines);
        Check(lines.size() == 3, "the ring keeps at most what it was sized for");
        Check(lines.front().seq == 3 && lines.back().seq == 5, "the newest lines are the ones kept");
        Check(lines[0].text == "line 3" && lines[0].time == "12:00:03",
              "the lines are in order and keep their clock");
        Check(history.newest_seq() == 5 && history.size() == 3, "the ring knows where it stands");
        Check(lines[1].Display() == "line 4", "a line without a speaker is just its text");
        line.seq = 6;
        line.speaker = "Rean";
        line.text = "Good morning.";
        history.Push(line);
        history.Snapshot(lines);
        Check(lines.back().Display() == "Rean: Good morning.",
              "with a speaker the panel shows the same line atmt_latest.txt does");
        history.set_capacity(60);
        Check(history.capacity() == 60 && history.size() == 3, "growing the cap keeps what is there");
        history.Clear();
        Check(history.size() == 0, "clearing empties it");
    }

    // --- what the sink records for the panel -------------------------------------------
    // The panel shows exactly what was logged - the same speaker, the same text, the same clock -
    // because it is recorded where the jsonl line is written, not by a second code path.
    {
        History history;
        history.set_capacity(10);
        LogSink sink;
        Check(sink.Open(dir), "the sink opens for the history check");
        sink.set_history(&history);
        CatalogEntry e;
        e.text = "Hello there.";
        sink.Line(e, 0x3000, "Rean", "test");
        std::vector<LogLine> lines;
        history.Snapshot(lines);
        Check(lines.size() == 1 && lines[0].speaker == "Rean" && lines[0].text == "Hello there.",
              "a line the sink writes is in the ring the panel reads");
        Check(lines[0].seq == sink.count() && !lines[0].time.empty(),
              "it carries the sequence number and a clock the panel can show");
        sink.Close();
    }

    // --- the hook service contract ---------------------------------------------------
    // The service returns the target address as its handle (null means failure). Treating a
    // non-zero return as an error silently left the hook created but never enabled, so the
    // game ran and logged nothing. These checks pin the convention.
    {
        static int enable_calls = 0;
        HookApi fake;
        fake.create = [](void* target, void*, void** trampoline) -> void* {
            if (trampoline != nullptr) *trampoline = target;   // stand-in trampoline
            return target;                                    // the handle, as the loader does
        };
        fake.enable = [](void* handle) -> int {
            (void)handle;
            ++enable_calls;
            return 0;
        };
        SetHookApi(fake);

        LogSink sink;
        Check(sink.Open(dir), "the log sink opens for the hook check");
        {
            // The caller's settings live in a scope of their own: the hook must keep its own
            // copy, or the values dangle the moment that scope ends. That was a real crash -
            // the first message after startup faulted on a config pointer into a dead thread's
            // stack, so the check clobbers the stack in between.
            Config c;
            c.message_hook_address = reinterpret_cast<uintptr_t>(&StripCodes);   // real code
            c.message_text_skip = 1234;
            c.message_plate_offset = 0x777;
            const bool installed = InstallAddressHooks(c, sink);
            Check(installed, "a hook the service accepts counts as installed");
            Check(enable_calls == 1, "the hook is enabled once it is created");
        }
        ClobberStack(256 * 1024);
        Check(HookConfig().message_text_skip == 1234
                  && HookConfig().message_plate_offset == 0x777,
              "the hook keeps its own copy of the settings (survives its caller's frame)");
        Check(HookConfig().message_hook_address == reinterpret_cast<uintptr_t>(&StripCodes),
              "the hook uses the address it was given, even after the caller is gone");
        sink.Close();

        Config bad;
        bad.message_hook_address = reinterpret_cast<uintptr_t>(heap.data());   // data, not code
        LogSink sink2;
        sink2.Open(dir);
        Check(!InstallAddressHooks(bad, sink2),
              "a hook on memory that is not executable code is refused");
        sink2.Close();
        SetHookApi(HookApi{});
    }

    // --- the hook's own entry point, end to end --------------------------------------
    // This is the code that crashed the game: the detour's entry point, the register
    // snapshot, the message state and the run. Everything here is a stand-in for the game's
    // frame, so a fault in the probe shows up as a failed test instead of a dead game.
    {
        static int enable_calls2 = 0;
        HookApi fake;
        fake.create = [](void* target, void*, void** trampoline) -> void* {
            if (trampoline != nullptr) *trampoline = target;
            return target;
        };
        fake.enable = [](void*) -> int {
            ++enable_calls2;
            return 0;
        };
        SetHookApi(fake);

        LogSink sink;
        sink.Open(dir);
        sink.set_pending_ms(0);
        {
            // settings in a scope of their own, and the stack clobbered before the first
            // message: exactly the sequence that made the game crash on its first dialog
            Config c;
            c.message_hook_address = reinterpret_cast<uintptr_t>(&StripCodes);
            c.message_text_skip = 5;
            c.message_plate_offset = 0x358;
            InstallAddressHooks(c, sink);
        }
        ClobberStack(256 * 1024);

        // the message state the game keeps: the name plate the game will draw, and the run
        std::vector<unsigned char> obj(0x400, 0);
        std::memcpy(obj.data() + 0x358, "Rean", 5);
        // the run as the game stores it: `11 <u32>` then the bytes
        std::string run("\x11\x00\x00\x00\x00Oh, good morning!", 25);
        // The stack frame the hooked function had at its entry: [return address][argument].
        uint32_t fake_frame[2] = {0xDEADBEEF,
                                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(run.data()))};
        // The register snapshot the detour builds: [index][edi esi ebp esp ebx edx ecx eax]
        // [eflags]. `esp` in there is the value *after* the detour's pushfl (entry-4), which
        // is the frame the probe adds 8 to in order to reach the argument.
        uintptr_t snap[12] = {0};
        snap[1 + 3] = reinterpret_cast<uintptr_t>(&fake_frame[0]) - 4;
        snap[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());

        const size_t before = sink.count();
        atmt_probe_message(snap);
        Check(sink.count() == before + 1, "a message through the hook's entry point is logged");
        const std::string line = LastLogLine(dir + L"\\atmt_dialogs.jsonl");
        Check(line.find("\"speaker\":\"Rean\"") != std::string::npos,
              "the speaker is read from the message state's name plate");
        Check(line.find("Oh, good morning!") != std::string::npos,
              "the text is read from the run the game passed");

        // Bug: the game reuses run buffers across different lines (measured live: the
        // busiest addresses hosted 8-9 different texts - reuse, not a fixed message window). A dedupe keyed on the pointer alone silently ate the second
        // line whenever it landed on a recently used address, which is how lines went
        // intermittently missing from an otherwise working log. Redrawing the same text on the
        // same buffer must still be deduped; different text on the same buffer must not be.
        {
            std::vector<char> buf(64, 0);
            auto write_run = [&](const char* text) {
                std::memset(buf.data(), 0, buf.size());
                buf[0] = '\x11';
                std::memcpy(buf.data() + 5, text, std::strlen(text));
            };

            write_run("First line in this box.");
            uint32_t frame_1[2] = {0xAAAA,
                                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf.data()))};
            uintptr_t snap_1[12] = {0};
            snap_1[1 + 3] = reinterpret_cast<uintptr_t>(&frame_1[0]) - 4;
            snap_1[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            const size_t r1 = sink.count();
            atmt_probe_message(snap_1);
            Check(sink.count() == r1 + 1, "the first line on a fresh buffer is logged");

            // Same buffer, same text: still a redraw of the line already logged.
            uint32_t frame_1b[2] = {
                0xAAAA, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf.data()))};
            uintptr_t snap_1b[12] = {0};
            snap_1b[1 + 3] = reinterpret_cast<uintptr_t>(&frame_1b[0]) - 4;
            snap_1b[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            atmt_probe_message(snap_1b);
            Check(sink.count() == r1 + 1, "redrawing the same text on the same buffer is still deduped");

            // Same buffer, different text: the game moved on to a new line without moving the
            // buffer - this must not be swallowed by the dedupe.
            write_run("A different line, same address.");
            uint32_t frame_2[2] = {0xAAAA,
                                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(buf.data()))};
            uintptr_t snap_2[12] = {0};
            snap_2[1 + 3] = reinterpret_cast<uintptr_t>(&frame_2[0]) - 4;
            snap_2[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            atmt_probe_message(snap_2);
            Check(sink.count() == r1 + 2,
                  "a different line reusing the same buffer address is still logged");
            Check(LastLogLine(dir + L"\\atmt_dialogs.jsonl")
                          .find("\"text\":\"A different line, same address.\"") != std::string::npos,
                  "the reused-buffer line's own text reaches the log");
        }

        // The game hands the run over in two shapes, and getting this wrong cut the first
        // characters off real lines ("Oh, darling, you forgot something..." arrived as
        // "arling, you forgot something..."). Both shapes are checked here.
        {
            // (a) the text opcode: `11 <u32>` then the bytes
            std::string opcode_run("\x11\x18\x11\x00\x00Morning. You two are up early.", 38);
            uint32_t frame_a[2] = {0xDEADBEEF, static_cast<uint32_t>(
                                                  reinterpret_cast<uintptr_t>(opcode_run.data()))};
            uintptr_t snap_a[12] = {0};
            snap_a[1 + 3] = reinterpret_cast<uintptr_t>(&frame_a[0]) - 4;
            snap_a[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            const size_t n1 = sink.count();
            atmt_probe_message(snap_a);
            Check(sink.count() == n1 + 1 && LastLogLine(dir + L"\\atmt_dialogs.jsonl").find(
                      "\"text\":\"Morning. You two are up early.\"") != std::string::npos,
                  "a run handed over at its `11 <u32>` opcode keeps the whole line");

            // (b) a run the game has already advanced into: the bytes start at the pointer
            std::string plain_run("Oh, darling, you forgot something...", 37);
            uint32_t frame_b[2] = {0xBADC0DE, static_cast<uint32_t>(
                                                 reinterpret_cast<uintptr_t>(plain_run.data()))};
            uintptr_t snap_b[12] = {0};
            snap_b[1 + 3] = reinterpret_cast<uintptr_t>(&frame_b[0]) - 4;
            snap_b[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            const size_t n2 = sink.count();
            atmt_probe_message(snap_b);
            Check(sink.count() == n2 + 1 && LastLogLine(dir + L"\\atmt_dialogs.jsonl").find(
                      "\"text\":\"Oh, darling, you forgot something...\"") != std::string::npos,
                  "a run handed over after its opcode keeps the whole line");

            // (c) defensive: the name header signature, in case a caller passes one
            std::string header_run("\x10\x00\x00\x1a\x03\x04I'm off, then. Take care!", 38);
            uint32_t frame_c[2] = {0xF00DF00D, static_cast<uint32_t>(
                                                   reinterpret_cast<uintptr_t>(header_run.data()))};
            uintptr_t snap_c[12] = {0};
            snap_c[1 + 3] = reinterpret_cast<uintptr_t>(&frame_c[0]) - 4;
            snap_c[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            const size_t n3 = sink.count();
            atmt_probe_message(snap_c);
            Check(sink.count() == n3 + 1 && LastLogLine(dir + L"\\atmt_dialogs.jsonl").find(
                      "\"text\":\"I'm off, then. Take care!\"") != std::string::npos,
                  "a name header before the run is stepped over");
        }

        // Bug: the setter is only called for a message's first page - the game walks the pages
        // after a `02 03` break itself, so those pages never reached the log (measured in
        // t1010.dat: "We may be in the same group...", "Well, let's go." were always missing
        // while the page before them was logged). The bytes below are the game's own.
        {
            const char msg_bytes[] =
                "\x11\xfe\x20\x00\x00...You are my classmate. You are not\x01my friend."
                "\x02\x03\x11\xff\x20\x00\x00We may be in the same group."
                "\x02\x03\x0bLast page."
                "\x02\x00\x1c\x3a\x00\x00";
            std::string message(msg_bytes, sizeof(msg_bytes) - 1);
            uint32_t frame_m[2] = {0x3333, static_cast<uint32_t>(
                                               reinterpret_cast<uintptr_t>(message.data()))};
            uintptr_t snap_m[12] = {0};
            snap_m[1 + 3] = reinterpret_cast<uintptr_t>(&frame_m[0]) - 4;
            snap_m[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            // The game's message state, as the setter and the renderer leave it (addr_hook.cpp,
            // HeldPage): +0x32C the run, +0x330 how far it is typed, +0x334 where the page starts.
            auto set_u32 = [&](size_t offset, uint32_t value) {
                std::memcpy(obj.data() + offset, &value, sizeof(value));
            };
            const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(message.data()));
            const uint32_t page2 = static_cast<uint32_t>(message.find("We may be"));
            const uint32_t page3 = static_cast<uint32_t>(message.find("Last page."));
            const size_t m0 = sink.count();
            atmt_probe_message(snap_m);
            set_u32(0x32C, base);   // what the game's setter does right after the probe
            set_u32(0x330, 0);
            set_u32(0x334, 0);
            atmt::ReleaseHeldPages(false);
            Check(sink.count() == m0 + 1,
                  "only the first page is logged while the game shows the first page");
            set_u32(0x330, page2 - 3);   // typed up to the `02 03`, waiting for the player
            atmt::ReleaseHeldPages(false);
            Check(sink.count() == m0 + 1, "a finished page waiting for input does not release the next");
            set_u32(0x334, page2 - 5);   // the player advanced: the page starts after the break
            atmt::ReleaseHeldPages(false);
            Check(sink.count() == m0 + 2, "the second page is logged once the game shows it");
            set_u32(0x330, page3 + 2);   // typing into the third page releases it as well
            atmt::ReleaseHeldPages(false);
            Check(sink.count() == m0 + 3, "every page of a message is logged, not only the first");
            const std::string all = ReadAll(dir + L"\\atmt_dialogs.jsonl");
            const size_t p1 = all.rfind("\"text\":\"...You are my classmate. You are not my friend.\"");
            const size_t p2 = all.rfind("\"text\":\"We may be in the same group.\"");
            const size_t p3 = all.rfind("\"text\":\"Last page.\"");
            Check(p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos
                      && p1 < p2 && p2 < p3,
                  "the pages after a `02 03` break are logged in order, their headers stepped over");

            // Should the game hand one of those pages to the setter later, it is not logged twice.
            uint32_t frame_p2[2] = {0x4444, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                                                message.data() + message.find("We may be")))};
            uintptr_t snap_p2[12] = {0};
            snap_p2[1 + 3] = reinterpret_cast<uintptr_t>(&frame_p2[0]) - 4;
            snap_p2[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            atmt_probe_message(snap_p2);
            Check(sink.count() == m0 + 3, "a page already logged with its message is not logged again");

            // The player moves on before a later page was reached (the next message is set on the
            // same box): what is still held goes out first, in order, then the new line.
            std::string two_pages("\x11\x01\x02\x00\x00" "First of two." "\x02\x03" "Second of two."
                                  "\x02\x00", 36);
            uint32_t frame_t[2] = {0x6666, static_cast<uint32_t>(
                                               reinterpret_cast<uintptr_t>(two_pages.data()))};
            uintptr_t snap_t[12] = {0};
            snap_t[1 + 3] = reinterpret_cast<uintptr_t>(&frame_t[0]) - 4;
            snap_t[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            const size_t q0 = sink.count();
            atmt_probe_message(snap_t);
            set_u32(0x32C, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(two_pages.data())));
            set_u32(0x330, 0);
            set_u32(0x334, 0);
            atmt::ReleaseHeldPages(false);
            Check(sink.count() == q0 + 1, "a two-page message shows its first page only");
            std::string next_run("Next message.", 13);
            uint32_t frame_n[2] = {0x5555, static_cast<uint32_t>(
                                               reinterpret_cast<uintptr_t>(next_run.data()))};
            uintptr_t snap_n[12] = {0};
            snap_n[1 + 3] = reinterpret_cast<uintptr_t>(&frame_n[0]) - 4;
            snap_n[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            atmt_probe_message(snap_n);
            const std::string after = ReadAll(dir + L"\\atmt_dialogs.jsonl");
            Check(sink.count() == q0 + 3
                      && after.rfind("\"text\":\"Second of two.\"") < after.rfind("\"text\":\"Next message.\""),
                  "the held pages of a message go out, in order, when the next message is set");
        }

        // The diagnostic that matters: when the hook runs but the bytes are not a readable run,
        // that must be written down - a line that silently does not appear is the worst outcome.
        {
            // 0x00 ends a run immediately, so there is genuinely nothing to read here (0x01 would
            // not: that is the game's in-message line break and the reader would walk past it)
            std::vector<unsigned char> junk_off(64, 0x00);
            std::vector<unsigned char> junk_on(64, 0x00);    // a different run, so the dedupe in the
                                                             // hook does not swallow the second call
            const std::wstring diag_path = dir + L"\\atmt_diagnostics.log";
            _wremove(diag_path.c_str());

            // off: the hook's own copy of the settings has the switch off, so nothing is written
            uint32_t frame_off[2] = {0x1111, static_cast<uint32_t>(
                                                 reinterpret_cast<uintptr_t>(junk_off.data()))};
            uintptr_t snap_off[12] = {0};
            snap_off[1 + 3] = reinterpret_cast<uintptr_t>(&frame_off[0]) - 4;
            snap_off[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            atmt_probe_message(snap_off);
            Check(_wfopen(diag_path.c_str(), L"rb") == nullptr,
                  "with diagnostics off, an unreadable run writes nothing");

            // on: the same case, with the switch on in the settings the hook was installed with
            uint32_t frame_on[2] = {0x2222, static_cast<uint32_t>(
                                                reinterpret_cast<uintptr_t>(junk_on.data()))};
            uintptr_t snap_on[12] = {0};
            snap_on[1 + 3] = reinterpret_cast<uintptr_t>(&frame_on[0]) - 4;
            snap_on[1 + 6] = reinterpret_cast<uintptr_t>(obj.data());
            {
                Config diag;
                diag.message_hook_address = reinterpret_cast<uintptr_t>(&StripCodes);
                diag.message_text_skip = 5;
                diag.message_plate_offset = 0x358;
                diag.diagnostics = true;
                InstallAddressHooks(diag, sink);   // takes its own copy
            }
            atmt_probe_message(snap_on);
            const std::string reported = LastLogLine(diag_path);
            Check(reported.find("nothing readable") != std::string::npos,
                  "with diagnostics on, a run that cannot be read is reported");
            Check(reported.find("00 00 00") != std::string::npos,
                  "the report shows the bytes that were there");
        }

        sink.Close();
        SetHookApi(HookApi{});
    }

    // The game's font (itf_font.h): a hand-made .itf with the layout measured on font_us_hd.itf -
    // the table at 0x40, a 12-byte glyph header, 4-bit pixels without row padding, low nibble first.
    {
        std::vector<uint8_t> itf(0x40 + 2 * 8, 0);
        itf[0] = 1;
        itf[1] = 1;
        itf[2] = 100;   // cell size
        auto put16 = [&](size_t at, uint32_t v) {
            itf[at] = static_cast<uint8_t>(v);
            itf[at + 1] = static_cast<uint8_t>(v >> 8);
        };
        auto put32 = [&](size_t at, uint32_t v) {
            put16(at, v & 0xFFFF);
            put16(at + 2, v >> 16);
        };
        put32(4, 2);   // two glyphs: 'A' and U+3231 (the game's heart)
        auto add_glyph = [&](size_t slot, uint32_t cp, int w, int h, int top, int left, int adv) {
            const size_t at = itf.size();
            put32(0x40 + slot * 8, cp);
            put32(0x40 + slot * 8 + 4, static_cast<uint32_t>(at));
            itf.resize(at + 12 + (static_cast<size_t>(w) * h + 1) / 2, 0);
            put16(at, w);
            put16(at + 2, h);
            put16(at + 4, top);
            put16(at + 6, left);
            put16(at + 8, adv);
            return at + 12;
        };
        const size_t a_px = add_glyph(0, 'A', 3, 2, 18, 1, 41);
        itf[a_px] = 0xF1;      // (0,0) = 1, (1,0) = 15
        itf[a_px + 1] = 0x07;  // (2,0) = 7
        add_glyph(1, 0x3231, 2, 2, 22, 1, 74);
        ItfFont font;
        Check(font.Load(itf.data(), itf.size()), "a well-formed .itf loads");
        ItfGlyph g;
        Check(font.cell_size() == 100 && font.glyph_count() == 2, "the cell size and the table are read");
        Check(font.Find('A', &g) && g.w == 3 && g.h == 2 && g.top == 18 && g.left == 1 && g.advance == 41,
              "a glyph's metrics are read from its header");
        Check(ItfFont::Pixel(g, 0, 0) == 1 && ItfFont::Pixel(g, 1, 0) == 15 && ItfFont::Pixel(g, 2, 0) == 7,
              "pixels are 4-bit, low nibble first, rows not padded");
        Check(font.Find(0x3231, &g) && g.advance == 74, "the game's own symbols are found by code point");
        Check(!font.Find('B', &g), "a glyph the font lacks is reported missing");
        std::vector<uint8_t> broken = itf;
        broken.resize(broken.size() - 1);   // the last glyph's pixels run past the end
        ItfFont rejected;
        Check(!rejected.Load(broken.data(), broken.size()) && rejected.empty(),
              "a truncated .itf is refused");
    }

    printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
           g_failures);
    return g_failures == 0 ? 0 : 1;
}

