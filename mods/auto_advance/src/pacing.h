// pacing.h - what auto_advance decides, without the game: how many characters a page shows, and when
// a page that is fully on screen has been up long enough to be advanced.
//
// Header-only and free of Windows and the game, so tests/test_auto_advance.cpp checks it offline.
#pragma once

#include <cstddef>
#include <cstdint>

namespace auto_advance {

// Characters a reader reads on one page of a message run: the bytes from the page's start (the message
// state's +0x334, relative to its run at +0x32C) to how far the game has typed it (+0x330) or, while it
// still types, to the page's end (PageEnd).
//
// The run's encoding (docs/ENGINE_NOTES.md, "Dialog"): UTF-8 text; a byte below 0x20 is control -
// 0x01 a soft line break, 0x02 the end of a page - and a page after a `02 03` break may carry a header
// with binary operands (`11 <u32>` / `12 <u32>` the text opcode, `10 00 00 1a <u16>` a name); inline
// codes are `#` and a few letters and digits, with an optional [argument] (`#E[A]`, `#1P`, `#M0`).
// Spaces are not counted; every other code point is one character, whatever its byte length.
//
// An item shown with its icon (`#<id>I`, then the name in colour - "Obtained #112I#2CU-Material#0C x3.")
// counts kIconNameChars on top of the text: such a line is read slower than its letters say (the icon,
// the colour, a name to take in), and where the game puts the name in by itself it is not in the run.
constexpr unsigned kIconNameChars = 20;

inline unsigned CountPageChars(const uint8_t* p, size_t n) {
    unsigned count = 0;
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i];
        if (b == 0x11 || b == 0x12) {   // a header with a u32 operand: its bytes are not text
            i += 5;
            continue;
        }
        if (b == 0x10 && i + 3 < n && p[i + 1] == 0x00 && p[i + 2] == 0x00 && p[i + 3] == 0x1A) {
            i += 6;
            continue;
        }
        if (b < 0x20) {
            ++i;
            continue;
        }
        if (b == '#') {
            // As the dialog logger strips them (trails_dialog_logger/src/util.cpp, StripCodes):
            //   #<digits><letter>       exactly one letter (#0T, #1P, #5S, #2C) - the text follows
            //   #E / #M + one argument  digits, [..] or a single letter (#E6, #E[1], #EI, #MA)
            //   the rest                letters and digits up to an upper-case letter that starts a word
            // `#<digits>I` is an item's icon: the item's name is counted on top (kIconNameChars).
            const auto upper = [](uint8_t c) { return c >= 'A' && c <= 'Z'; };
            const auto lower = [](uint8_t c) { return c >= 'a' && c <= 'z'; };
            const auto digit = [](uint8_t c) { return c >= '0' && c <= '9'; };
            const auto alpha = [&](uint8_t c) { return upper(c) || lower(c); };
            const auto alnum = [&](uint8_t c) { return alpha(c) || digit(c); };
            size_t j = i + 1;
            if (j < n && digit(p[j])) {
                while (j < n && digit(p[j])) ++j;
                if (j < n && alpha(p[j])) {
                    if (p[j] == 'I') count += kIconNameChars;
                    ++j;
                }
            } else if (j < n && (p[j] == 'E' || p[j] == 'M')) {
                ++j;
                if (j < n && digit(p[j])) {
                    while (j < n && digit(p[j])) ++j;
                } else if (j < n && alpha(p[j])) {
                    ++j;
                }
            } else {
                while (j < n && alnum(p[j])) {
                    if (j > i + 1 && upper(p[j])) {
                        const bool word_start = j + 1 >= n || lower(p[j + 1]) || !alnum(p[j + 1]);
                        if (word_start) break;
                    }
                    ++j;
                }
            }
            if (j > i + 1) {
                if (j < n && p[j] == '[') {   // an argument; may nest (#M[[autoM0]])
                    int depth = 0;
                    while (j < n) {
                        if (p[j] == '[') ++depth;
                        else if (p[j] == ']' && --depth == 0) {
                            ++j;
                            break;
                        }
                        ++j;
                    }
                }
                i = j;
                continue;
            }
            // a '#' that starts no code is a character like any other
        }
        if (b != ' ' && (b & 0xC0) != 0x80) ++count;   // not a space, not a UTF-8 continuation byte
        ++i;
    }
    return count;
}

// Where the page that starts at `p` ends: the offset of its `02` (the end of a page or a message), or
// `n` when none comes within `n` bytes. Headers are stepped over as CountPageChars does, so a `02` in an
// operand (`11 02 00 00 00`) is not taken for the end.
inline size_t PageEnd(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t b = p[i];
        if (b == 0x02) return i;
        if (b == 0x11 || b == 0x12) {
            i += 5;
        } else if (b == 0x10 && i + 3 < n && p[i + 1] == 0x00 && p[i + 2] == 0x00 && p[i + 3] == 0x1A) {
            i += 6;
        } else {
            ++i;
        }
    }
    return n;
}

// Was a key press stamped `pressed_at` (GetTickCount(); 0 = none) at most `window_ms` before `now`?
// The stamp is never 0 (a press at tick 0 is stamped 1), so it can be a tick *ahead* of `now`: the
// difference is taken signed, and "ahead" counts as fresh - an unsigned one wrapped to 4 billion and
// dropped the press whenever the game polled within the same tick.
inline bool PressFresh(uint32_t pressed_at, uint32_t now, uint32_t window_ms) {
    if (pressed_at == 0) return false;
    const int32_t age = static_cast<int32_t>(now - pressed_at);
    return age < static_cast<int32_t>(window_ms);
}

// How long a page of `chars` characters stays up: a base for any page plus a time per character, and
// never less than `min_ms`.
inline double ReadingMs(unsigned chars, double base_ms, double ms_per_char, double min_ms = 0.0) {
    const double ms = base_ms + ms_per_char * static_cast<double>(chars);
    if (ms < min_ms) return min_ms;
    return ms > 0.0 ? ms : 0.0;
}

// One page's countdown. Fed once per poll of the game's "wait for a key" with what the game shows;
// says whether to press, and how much of the wait is left (the pie).
//
//   * The countdown starts when a new `page` key comes: when the page is fully typed, or - when the
//     caller passes pages that still type, with `ready` false - when it starts typing. Either way the
//     page is never turned before it is `ready` (fully shown): a press then would only finish the text.
//   * It ends at the later of the reading time and the end of the voice line plus `after_voice_ms`.
//     While a voice plays, its end is its known length (start + duration), or "not yet" when the
//     length is unknown; a voice still playing past its expected end holds the press either way.
//     `voice_cap_ms` bounds the wait for a voice, so a sound that never reports its end cannot stop
//     the dialog for good.
//   * Time does not pass while the game is not polling (a pause, an unfocused window) or while
//     `paused` (the overlay has the input): the gap is added to the start instead (and, while
//     `paused` or held, to the voice's end as well).
//   * `toggle_hold` (the player's "hold this line" key) holds the page on screen: its countdown stops
//     until the key is pressed again, and the page is never turned while held. It applies to the page
//     in this Input, not to the one seen before - a key pressed while the next box opens holds the new
//     page - and a press while the text still types holds the page about to be shown. A hold ends with
//     its page.
//   * A page that was turned - by this Pacer's press, or by the player's (PlayerTurned) - and comes back
//     with the same key after a poll gap is a new showing of it, not a pause: an NPC's one-line message
//     sits at the same place every time it is opened, so its key is the same each time, and the game
//     does not poll between dialogs. Its countdown starts over instead of going on where it ended.
class Pacer {
public:
    struct Input {
        double now_ms = 0.0;
        uint32_t page = 0;            // which page is up (0 = none)
        bool ready = true;            // it is fully shown (false = it still types)
        double reading_ms = 0.0;      // ReadingMs() for that page
        bool voice_playing = false;   // a voice line of this dialog is playing now
        double voice_end_ms = 0.0;    // its expected end (start + length), 0 = unknown
        double after_voice_ms = 0.0;
        double voice_cap_ms = 60000.0;
        bool paused = false;
        bool toggle_hold = false;     // the hold key was pressed since the last Step
    };
    struct Output {
        bool press = false;           // advance now
        bool counting = false;        // a page is waiting: draw the pie
        float left = 0.0f;            // 1 = the whole wait ahead, 0 = due
        bool waiting_for_voice = false;   // the reading time is up; the voice is what holds it
        bool held = false;            // the player holds this page (toggle_hold)
        bool hold_pending = false;    // ...or the page about to be shown
        bool hold_dropped = false;    // a held page went away (diagnostics)
    };

    // A poll gap longer than this is a pause, not a frame.
    static constexpr double kGapMs = 250.0;
    // After a press, the same page is pressed again only after this long (should the game not take it).
    static constexpr double kRetryMs = 1000.0;

    Output Step(const Input& in) {
        Output out;
        double gap = last_poll_ >= 0.0 ? in.now_ms - last_poll_ : 0.0;
        last_poll_ = in.now_ms;
        if (in.page == 0) {
            // Between dialogs the game does not poll: a hold asked for then is not for the next one.
            if (page_ != 0 || gap > kGapMs) hold_next_ = false;
            out.hold_dropped = hold_;
            page_ = 0;
            hold_ = false;
            if (in.toggle_hold) hold_next_ = !hold_next_;
            out.hold_pending = hold_next_;
            return out;
        }
        if (in.page != page_ || (turned_ && gap > kGapMs)) {
            out.hold_dropped = hold_;
            turned_ = false;
            page_ = in.page;
            hold_ = hold_next_;
            hold_next_ = false;
            start_ = in.now_ms;
            voice_end_ = 0.0;
            pressed_at_ = -1.0;
            gap = 0.0;
        }
        if (in.toggle_hold) hold_ = !hold_;
        if (gap > kGapMs) start_ += gap;
        if ((in.paused || hold_) && gap <= kGapMs) {
            // The whole countdown stands still, the voice's part of it too: a voice that ends while
            // held still leaves its pause after it, and the pie does not run down toward its end.
            start_ += gap;
            if (voice_end_ > 0.0) voice_end_ += gap;
        }
        if (in.voice_playing) {
            const double expected = in.voice_end_ms > in.now_ms ? in.voice_end_ms : in.now_ms;
            if (expected > voice_end_) voice_end_ = expected;
        }

        double end = start_ + in.reading_ms;
        const double voice_done = voice_end_ > 0.0 ? voice_end_ + in.after_voice_ms : 0.0;
        const double cap = start_ + (in.reading_ms > in.voice_cap_ms ? in.reading_ms : in.voice_cap_ms);
        bool voice_holds = false;
        if (voice_done > end) {
            end = voice_done < cap ? voice_done : cap;
            voice_holds = true;
        }
        const bool capped = in.now_ms >= cap;
        const bool held_by_voice = in.voice_playing && !capped;
        if (held_by_voice && end <= in.now_ms) end = in.now_ms + in.after_voice_ms;

        out.counting = true;
        out.held = hold_;
        out.waiting_for_voice = voice_holds && in.now_ms >= start_ + in.reading_ms;
        const double total = end - start_;
        out.left = total > 0.0 ? static_cast<float>((end - in.now_ms) / total) : 0.0f;
        if (out.left < 0.0f) out.left = 0.0f;
        if (out.left > 1.0f) out.left = 1.0f;

        if (!in.ready || in.paused || hold_ || held_by_voice || in.now_ms < end) return out;
        if (pressed_at_ >= 0.0 && in.now_ms - pressed_at_ < kRetryMs) return out;
        pressed_at_ = in.now_ms;
        turned_ = true;
        out.press = true;
        out.left = 0.0f;
        return out;
    }

    // The player pressed confirm on `page`. On a page fully shown that turns it (on one still typing
    // it only finishes the text, and the countdown goes on).
    void PlayerTurned(uint32_t page, bool ready) {
        if (page != 0 && page == page_ && ready) turned_ = true;
    }

private:
    uint32_t page_ = 0;
    bool turned_ = false;        // the page was turned: seen again after a gap, it is shown anew
    bool hold_ = false;          // the page on screen is held
    bool hold_next_ = false;     // the page about to be shown will be
    double start_ = 0.0;
    double last_poll_ = -1.0;
    double voice_end_ = 0.0;     // the latest expected end of a voice heard on this page
    double pressed_at_ = -1.0;
};

}  // namespace auto_advance
