// atmt_input.h - the names of keys and pad buttons, and the edges and repeats the overlay reads them
// with. Header-only and shared: the overlay (mods/overlay) and every mod that draws in it (the dialog
// log's panel) parse the same ini names and feel the same when a direction is held.
//
// The game is a DirectInput game: it ignores the Windows message queue entirely (measured, see
// docs/ENGINE_NOTES.md, "Input"), so a hotkey cannot be a RegisterHotKey or a window hook - the async key
// state is what is left, and it needs no hook at all. The pad is read by the overlay (through its
// own XInputGetState hook, which is also what takes it away from the game) and handed to every
// window as a raw snapshot each frame (AtmtOverlayInput).
#ifndef ATMT_INPUT_H
#define ATMT_INPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace atmt {

// ---------------------------------------------------------------- keys
constexpr unsigned kDefaultOverlayKey = 0x72;   // VK_F3: the dialog log
constexpr unsigned kDefaultSettingsKey = 0x71;  // VK_F2: the settings bar

// Is the key down right now?
inline bool KeyDown(unsigned vk) {
    if (vk == 0) return false;
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

// "F3" -> 0x72, "PAGEUP" -> VK_PRIOR, "`" -> VK_OEM_3, "A" -> 'A'. Case, spaces and separators are
// ignored, so "page up", "PageUp" and "pageup" are the same key. 0 for a name nobody knows - the
// caller reports it, because a silently ignored setting is the kind of thing that costs an evening.
inline unsigned KeyFromName(const std::string& name) {
    std::string up;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '-' || c == '_') continue;
        up.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    }
    if (up.empty()) return 0;

    struct Entry {
        const char* name;
        unsigned vk;
    };
    static const Entry kNames[] = {
        {"TAB", 0x09},        {"BACKSPACE", 0x08},   {"ENTER", 0x0D},
        {"RETURN", 0x0D},     {"ESC", 0x1B},         {"ESCAPE", 0x1B},
        {"SPACE", 0x20},      {"PAGEUP", 0x21},      {"PGUP", 0x21},
        {"PAGEDOWN", 0x22},   {"PGDN", 0x22},        {"END", 0x23},
        {"HOME", 0x24},       {"LEFT", 0x25},        {"UP", 0x26},
        {"RIGHT", 0x27},      {"DOWN", 0x28},        {"INSERT", 0x2D},
        {"INS", 0x2D},        {"DELETE", 0x2E},      {"DEL", 0x2E},
        {"GRAVE", 0xC0},      {"TILDE", 0xC0},       {"BACKTICK", 0xC0},
        {"OEM3", 0xC0},       {"MINUS", 0xBD},       {"EQUALS", 0xBB},
        {"PLUS", 0xBB},       {"LBRACKET", 0xDB},    {"RBRACKET", 0xDD},
        {"BACKSLASH", 0xDC},  {"SEMICOLON", 0xBA},   {"QUOTE", 0xDE},
        {"COMMA", 0xBC},      {"PERIOD", 0xBE},      {"SLASH", 0xBF},
        {"NUMPAD0", 0x60},    {"NUMPAD1", 0x61},     {"NUMPAD2", 0x62},
        {"NUMPAD3", 0x63},    {"NUMPAD4", 0x64},     {"NUMPAD5", 0x65},
        {"NUMPAD6", 0x66},    {"NUMPAD7", 0x67},     {"NUMPAD8", 0x68},
        {"NUMPAD9", 0x69},    {"MULTIPLY", 0x6A},    {"ADD", 0x6B},
        {"SUBTRACT", 0x6D},   {"DECIMAL", 0x6E},     {"DIVIDE", 0x6F},
    };
    for (const Entry& e : kNames) {
        if (up == e.name) return e.vk;
    }

    // F1..F24: "F" followed by a number, so the single letter F stays a letter.
    if (up[0] == 'F' && up.size() >= 2 && up.size() <= 3) {
        bool digits = true;
        for (size_t i = 1; i < up.size(); ++i) {
            if (isdigit(static_cast<unsigned char>(up[i])) == 0) digits = false;
        }
        if (digits) {
            const int number = atoi(up.c_str() + 1);
            if (number >= 1 && number <= 24) return 0x70u + static_cast<unsigned>(number - 1);
        }
    }

    // A single character: its own virtual key. Punctuation can be named literally - "`" is the one
    // people use for an overlay, and a settings file is a natural place to write it that way.
    if (up.size() == 1) {
        const char c = up[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<unsigned>(c);
        switch (c) {
            case '`': return 0xC0;
            case '=': return 0xBB;
            case '[': return 0xDB;
            case ']': return 0xDD;
            case '\\': return 0xDC;
            case ';': return 0xBA;
            case '\'': return 0xDE;
            case ',': return 0xBC;
            case '.': return 0xBE;
            case '/': return 0xBF;
            default: break;
        }
    }
    return 0;
}

// The other way round, for showing and saving a key picked in the settings bar: 0x72 -> "F3".
// Every name it returns reads back through KeyFromName to the same key; empty for a key without one.
inline std::string KeyName(unsigned vk) {
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= 0x60 && vk <= 0x69) return "Numpad" + std::to_string(vk - 0x60);
    struct Entry {
        unsigned vk;
        const char* name;
    };
    static const Entry kNames[] = {
        {0x08, "Backspace"}, {0x09, "Tab"},       {0x0D, "Enter"},    {0x1B, "Esc"},
        {0x20, "Space"},     {0x21, "PageUp"},    {0x22, "PageDown"}, {0x23, "End"},
        {0x24, "Home"},      {0x25, "Left"},      {0x26, "Up"},       {0x27, "Right"},
        {0x28, "Down"},      {0x2D, "Insert"},    {0x2E, "Delete"},   {0xC0, "Grave"},
        {0xBD, "Minus"},     {0xBB, "Equals"},    {0xDB, "LBracket"}, {0xDD, "RBracket"},
        {0xDC, "Backslash"}, {0xBA, "Semicolon"}, {0xDE, "Quote"},    {0xBC, "Comma"},
        {0xBE, "Period"},    {0xBF, "Slash"},     {0x6A, "Multiply"}, {0x6B, "Add"},
        {0x6D, "Subtract"},  {0x6E, "Decimal"},   {0x6F, "Divide"},
    };
    for (const Entry& e : kNames) {
        if (e.vk == vk) return e.name;
    }
    return std::string();
}

// ---------------------------------------------------------------- key chords
// A KEY setting is a chord: up to four modifiers and one other key, "Ctrl+F3", "Shift+Alt+L", or a
// plain "F3" (a chord without modifiers). It is packed in one unsigned: the virtual key in the low
// byte, the modifiers above it - so a plain key's chord is its virtual key, and 0 is "no key".
// The modifiers are the generic ones: Ctrl is either Ctrl, and so on.
constexpr unsigned kChordCtrl = 0x100;
constexpr unsigned kChordShift = 0x200;
constexpr unsigned kChordAlt = 0x400;
constexpr unsigned kChordWin = 0x800;
constexpr unsigned kChordModifiers = kChordCtrl | kChordShift | kChordAlt | kChordWin;

inline unsigned ChordKey(unsigned chord) { return chord & 0xFFu; }
inline unsigned ChordModifiers(unsigned chord) { return chord & kChordModifiers; }

// The modifiers in the order a chord's name lists them; the schema dumps these names for the manager.
struct ChordModifierName {
    unsigned bit;
    const char* name;
};
inline const ChordModifierName* ChordModifierNames(size_t* count) {
    static const ChordModifierName kNames[] = {
        {kChordCtrl, "Ctrl"}, {kChordShift, "Shift"}, {kChordAlt, "Alt"}, {kChordWin, "Win"}};
    *count = sizeof(kNames) / sizeof(kNames[0]);
    return kNames;
}

// Is this virtual key one of the modifiers (any side)? They are never the chord's key.
inline bool IsModifierKey(unsigned vk) {
    switch (vk) {
        case 0x10: case 0x11: case 0x12:         // VK_SHIFT, VK_CONTROL, VK_MENU
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:   // their left/right
        case 0x5B: case 0x5C:                    // VK_LWIN, VK_RWIN
            return true;
        default: return false;
    }
}

// The modifiers held right now.
inline unsigned ModifiersDown() {
    unsigned mods = 0;
    if (KeyDown(0x11)) mods |= kChordCtrl;
    if (KeyDown(0x10)) mods |= kChordShift;
    if (KeyDown(0x12)) mods |= kChordAlt;
    if (KeyDown(0x5B) || KeyDown(0x5C)) mods |= kChordWin;
    return mods;
}

// A modifier's bit for its name ("ctrl", "Control", "ALT", "win"...), 0 when it is not one.
inline unsigned ChordModifierFromName(const std::string& name) {
    std::string up;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '-' || c == '_') continue;
        up.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    }
    if (up == "CTRL" || up == "CONTROL") return kChordCtrl;
    if (up == "SHIFT") return kChordShift;
    if (up == "ALT") return kChordAlt;
    if (up == "WIN" || up == "WINDOWS") return kChordWin;
    return 0;
}

// "Ctrl+F3" -> kChordCtrl | 0x72, "shift + alt + l" -> kChordShift | kChordAlt | 'L', "F3" -> 0x72.
// The parts are split on '+' and may come in any order; exactly one of them is a key KeyFromName
// knows and the rest are modifiers. 0 for anything else ("Ctrl" alone, "Ctrl+F3+F4", "Ctrl+").
inline unsigned KeyChordFromName(const std::string& name) {
    unsigned mods = 0, vk = 0;
    size_t start = 0;
    while (start <= name.size()) {
        size_t plus = name.find('+', start);
        if (plus == std::string::npos) plus = name.size();
        const std::string part = name.substr(start, plus - start);
        if (const unsigned mod = ChordModifierFromName(part)) {
            mods |= mod;
        } else {
            const unsigned key = KeyFromName(part);
            if (key == 0 || vk != 0) return 0;   // unknown, or a second key
            vk = key;
        }
        start = plus + 1;
    }
    return vk == 0 ? 0 : (vk | mods);
}

// kChordCtrl | 0x72 -> "Ctrl+F3": the modifiers in a fixed order, then the key. Reads back through
// KeyChordFromName to the same chord; empty for 0 or a key without a name.
inline std::string KeyChordName(unsigned chord) {
    const std::string key = KeyName(ChordKey(chord));
    if (key.empty()) return std::string();
    std::string out;
    size_t count = 0;
    const ChordModifierName* mods = ChordModifierNames(&count);
    for (size_t i = 0; i < count; ++i) {
        if ((chord & mods[i].bit) != 0) out += std::string(mods[i].name) + "+";
    }
    return out + key;
}

// One chord, sampled once per frame. It fires once per press of its key, on the first frame the key
// is down with exactly the chord's modifiers - no fewer and no more, so with both F3 and Ctrl+F3
// bound, Ctrl+F3 is only the one. A modifier pressed a moment after the key still completes the
// chord (frames are sampled, and fingers are not exact), but once the key has been seen down with a
// modifier the chord does not have, that press of it is spoiled: releasing Ctrl while F3 is still
// held does not fire a plain F3.
class KeyEdge {
public:
    void set_key(unsigned chord) { chord_ = chord; }
    unsigned key() const { return chord_; }
    bool down() const { return key_down_; }
    bool PressedOnce() {
        const unsigned vk = ChordKey(chord_);
        if (vk == 0) return false;
        const bool now = KeyDown(vk);
        if (!now) {
            key_down_ = false;
            spent_ = false;
            return false;
        }
        key_down_ = true;
        if (spent_) return false;
        const unsigned want = ChordModifiers(chord_);
        const unsigned mods = ModifiersDown();
        if ((mods & ~want) != 0) {   // an extra modifier: some other chord's press
            spent_ = true;
            return false;
        }
        if (mods != want) return false;   // not complete yet
        spent_ = true;
        return true;
    }
    // Forget the current state: a key already down at this moment is not a press (until it is
    // released and pressed again).
    void Reset() {
        key_down_ = KeyDown(ChordKey(chord_));
        spent_ = key_down_;
    }

private:
    unsigned chord_ = 0;
    bool key_down_ = false;
    bool spent_ = false;   // this press of the key has fired, or cannot any more
};

// ---------------------------------------------------------------- pad buttons
// XINPUT_GAMEPAD's button bits, named the way the ini names them.
constexpr unsigned short kPadUp = 0x0001;
constexpr unsigned short kPadDown = 0x0002;
constexpr unsigned short kPadLeft = 0x0004;
constexpr unsigned short kPadRight = 0x0008;
constexpr unsigned short kPadStart = 0x0010;
constexpr unsigned short kPadBack = 0x0020;
constexpr unsigned short kPadL3 = 0x0040;   // left stick pressed in
constexpr unsigned short kPadR3 = 0x0080;   // right stick pressed in
constexpr unsigned short kPadLB = 0x0100;
constexpr unsigned short kPadRB = 0x0200;
constexpr unsigned short kPadA = 0x1000;
constexpr unsigned short kPadB = 0x2000;
constexpr unsigned short kPadX = 0x4000;
constexpr unsigned short kPadY = 0x8000;

constexpr short kStickDeadzone = 16000;   // a stick counts as a direction past this

// "B" -> kPadB, "d-pad up" -> kPadUp, "back" -> kPadBack. 0 for a name this does not know (the
// caller reports it); the analog triggers are deliberately not part of this.
inline unsigned short PadButtonFromName(const std::string& name) {
    struct Entry {
        const char* name;
        unsigned short bit;
    };
    static const Entry kNames[] = {
        {"UP", kPadUp},        {"DPADUP", kPadUp},       {"DOWN", kPadDown},
        {"DPADDOWN", kPadDown}, {"LEFT", kPadLeft},      {"DPADLEFT", kPadLeft},
        {"RIGHT", kPadRight},  {"DPADRIGHT", kPadRight}, {"START", kPadStart},
        {"BACK", kPadBack},    {"SELECT", kPadBack},     {"LB", kPadLB},
        {"L1", kPadLB},        {"RB", kPadRB},           {"R1", kPadRB},
        {"A", kPadA},          {"B", kPadB},             {"X", kPadX},
        {"Y", kPadY},          {"L3", kPadL3},           {"LSTICK", kPadL3},
        {"LEFTSTICK", kPadL3}, {"R3", kPadR3},           {"RSTICK", kPadR3},
        {"RIGHTSTICK", kPadR3},
    };
    std::string up;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '-' || c == '_') continue;
        up.push_back(static_cast<char>(toupper(static_cast<unsigned char>(c))));
    }
    if (up.empty()) return 0;
    for (const Entry& e : kNames) {
        if (up == e.name) return e.bit;
    }
    return 0;
}

inline const char* PadButtonName(unsigned short button) {
    switch (button) {
        case kPadUp: return "UP";
        case kPadDown: return "DOWN";
        case kPadLeft: return "LEFT";
        case kPadRight: return "RIGHT";
        case kPadStart: return "START";
        case kPadBack: return "BACK";
        case kPadL3: return "L3";
        case kPadR3: return "R3";
        case kPadLB: return "LB";
        case kPadRB: return "RB";
        case kPadA: return "A";
        case kPadB: return "B";
        case kPadX: return "X";
        case kPadY: return "Y";
        default: return "(none)";
    }
}

// A chord: "L3+R3" -> kPadL3 | kPadR3 (a single name is a chord of one). 0 when the name is empty or
// any part of it is unknown. A chord counts as pressed when all of its buttons are down.
inline unsigned short PadChordFromName(const std::string& name) {
    unsigned short chord = 0;
    size_t start = 0;
    while (start <= name.size()) {
        size_t plus = name.find('+', start);
        if (plus == std::string::npos) plus = name.size();
        const unsigned short bit = PadButtonFromName(name.substr(start, plus - start));
        if (bit == 0) return 0;   // "L3+" or "L3+turbo": not a chord anyone knows
        chord = static_cast<unsigned short>(chord | bit);
        start = plus + 1;
    }
    return chord;
}

// kPadL3 | kPadR3 -> "L3+R3", in a fixed order; empty for 0.
inline std::string PadChordName(unsigned short chord) {
    static const unsigned short kOrder[] = {kPadA,  kPadB,    kPadX,     kPadY,   kPadLB,
                                            kPadRB, kPadL3,   kPadR3,    kPadBack, kPadStart,
                                            kPadUp, kPadDown, kPadLeft,  kPadRight};
    std::string out;
    for (unsigned short bit : kOrder) {
        if ((chord & bit) == 0) continue;
        if (!out.empty()) out += "+";
        out += PadButtonName(bit);
    }
    return out;
}

// ---------------------------------------------------------------- held directions
// Held longer, both the tick rate and the step size ramp up, in two plateaus at 900 ms and 2500 ms:
// a tap still moves one step, a log read by holding DOWN does not take all evening.
constexpr DWORD kRepeatDelayMs = 320;
constexpr DWORD kRepeatRateMs = 90;

inline DWORD RepeatIntervalMs(DWORD held_ms) {
    if (held_ms >= 2500) return 35;
    if (held_ms >= 900) return 60;
    return kRepeatRateMs;
}
inline int RepeatStep(DWORD held_ms) {
    if (held_ms >= 2500) return 3;
    if (held_ms >= 900) return 2;
    return 1;
}

// One input (a key, a button, a stick direction), sampled once per frame.
struct Hold {
    bool down = false;
    DWORD next = 0;
    DWORD start = 0;   // the tick this hold began, for RepeatIntervalMs/RepeatStep
};

// True on the press, and - when `repeat` - again at the repeat rate while it is held.
inline bool Pressed(Hold& hold, bool now, bool repeat) {
    const DWORD tick = GetTickCount();
    if (!now) {
        hold.down = false;
        return false;
    }
    if (!hold.down) {
        hold.down = true;
        hold.start = tick;
        hold.next = tick + (repeat ? kRepeatDelayMs : 0);
        return true;
    }
    if (repeat && tick >= hold.next) {
        hold.next = tick + RepeatIntervalMs(tick - hold.start);
        return true;
    }
    return false;
}

// How far a repeated press moves: the accelerated step for how long it has been held.
inline int HeldStep(const Hold& hold) { return RepeatStep(GetTickCount() - hold.start); }

}  // namespace atmt

#endif  // ATMT_INPUT_H
