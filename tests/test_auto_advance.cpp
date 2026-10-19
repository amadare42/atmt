// test_auto_advance.cpp - what the auto_advance mod decides, offline (mods/auto_advance/src/pacing.h,
// wav.h).
//
// No game, no loader: how many characters a page counts (codes, headers and spaces left out), how
// long a voice line is from its WAV header, and when the countdown presses - after the reading time,
// never while the voice plays, not while paused, and not twice for one page.
//
// usage: atmt_auto_advance_test
#include "pacing.h"
#include "wav.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool cond, const char* what) {
    std::printf("%s %s\n", cond ? "[ ok ]" : "[FAIL]", what);
    if (!cond) ++g_failures;
}

static unsigned Count(const std::string& s) {
    return auto_advance::CountPageChars(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static void Put32(std::vector<uint8_t>* v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v->push_back(static_cast<uint8_t>(x >> (8 * i)));
}

static std::vector<uint8_t> Wav(uint32_t byte_rate, uint32_t data_size, bool extra_chunk) {
    std::vector<uint8_t> v;
    const char* riff = "RIFF\0\0\0\0WAVEfmt ";
    v.insert(v.end(), riff, riff + 16);
    Put32(&v, 16);
    const uint8_t fmt[8] = {1, 0, 1, 0, 0x80, 0xbb, 0, 0};   // PCM, mono, 48000 Hz
    v.insert(v.end(), fmt, fmt + 8);
    Put32(&v, byte_rate);
    v.push_back(2), v.push_back(0), v.push_back(16), v.push_back(0);
    if (extra_chunk) {
        const char* list = "LIST";
        v.insert(v.end(), list, list + 4);
        Put32(&v, 3);   // odd: padded to 4
        v.push_back('a'), v.push_back('b'), v.push_back('c'), v.push_back(0);
    }
    const char* data = "data";
    v.insert(v.end(), data, data + 4);
    Put32(&v, data_size);
    return v;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // --- characters on a page -------------------------------------------------------------
    Check(Count("Hello there.") == 11, "spaces are not counted, punctuation is");
    Check(Count("Hi\x01there") == 7, "a soft line break is not a character");
    Check(Count("#E[A]#M0Well, let's go.") == 13, "inline codes with and without arguments are left out");
    Check(Count("#1P*yawn*") == 6, "a code with digits ends at its letter");
    Check(Count("#M[[autoM0]]Ok") == 2, "a nested argument is skipped whole");
    Check(Count(std::string("\x11\xff\x20\x41\x00Yes", 8)) == 3, "a text opcode's operand bytes are not text");
    Check(Count(std::string("\x10\x00\x00\x1a\x41\x42No", 8)) == 2, "a name header is not text");
    Check(Count("Rean\xE3\x83\xBB\xE2\x80\xA6") == 6, "a multi-byte UTF-8 character counts once");
    Check(Count("# 1") == 2, "a lone '#' is a character");
    Check(Count("") == 0, "an empty page has no characters");
    Check(Count("Obtained #112I#2CU-Material#0C x3.") == 21 + auto_advance::kIconNameChars,
          "an item's icon counts its name on top of the text");
    Check(Count("Received #2C2000 mira#0C.") == 17, "the amount after a colour code is text");
    Check(Count("#2CARCUS#0C") == 5, "an all-capitals name after a colour code is text");
    Check(Count("#EI#M[[autoM4]]Anyway") == 6, "#E takes one letter argument");

    // --- reading time ---------------------------------------------------------------------
    {
        const std::string run("First page.\x02\x03\x11\x02\x00\x00\x00Second\x02\x00", 26);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(run.data());
        Check(auto_advance::PageEnd(p, run.size()) == 11, "a page ends at its 02");
        Check(auto_advance::PageEnd(p + 13, run.size() - 13) == 11, "a 02 inside a header's operand is not the end");
        Check(auto_advance::PageEnd(p, 5) == 5, "no 02 in reach: the end of what was given");
    }
    Check(auto_advance::PressFresh(1001, 1000, 1000), "a press stamped a tick ahead of the reader is fresh");
    Check(auto_advance::PressFresh(1000, 1999, 1000), "a press within the window is fresh");
    Check(!auto_advance::PressFresh(1000, 2000, 1000), "a press older than the window is not");
    Check(!auto_advance::PressFresh(0, 5, 1000), "no stamp, no press");
    Check(auto_advance::PressFresh(0xFFFFFFF0u, 0x10u, 1000), "fresh across GetTickCount's wrap");
    Check(auto_advance::ReadingMs(20, 1000.0, 50.0) == 2000.0, "base + characters x time per character");
    Check(auto_advance::ReadingMs(0, 0.0, 50.0) == 0.0, "nothing to read, nothing to wait");
    Check(auto_advance::ReadingMs(4, 250.0, 20.0, 1500.0) == 1500.0, "a short page stays up the minimum");
    Check(auto_advance::ReadingMs(100, 250.0, 20.0, 1500.0) == 2250.0, "a long page is not cut to the minimum");

    // --- voice length ---------------------------------------------------------------------
    std::vector<uint8_t> wav = Wav(96000, 48000, false);
    Check(auto_advance::WavDurationMs(wav.data(), wav.size()) == 500.0, "48 kHz mono 16-bit: bytes / byte rate");
    wav = Wav(96000, 192000, true);
    Check(auto_advance::WavDurationMs(wav.data(), wav.size()) == 2000.0, "a chunk before the data is skipped");
    Check(auto_advance::WavDurationMs(wav.data(), 20) == 0.0, "a header cut short is unknown");
    const uint8_t not_wav[16] = {'O', 'g', 'g', 'S'};
    Check(auto_advance::WavDurationMs(not_wav, sizeof(not_wav)) == 0.0, "not a WAVE file is unknown");

    // --- the countdown --------------------------------------------------------------------
    using auto_advance::Pacer;
    {
        Pacer p;
        Pacer::Input in;
        in.page = 7;
        in.reading_ms = 1000.0;
        bool pressed = false;
        float last_left = 2.0f;
        bool monotonic = true;
        for (double t = 0.0; t < 990.0; t += 16.0) {
            in.now_ms = t;
            const Pacer::Output out = p.Step(in);
            pressed = pressed || out.press;
            if (out.left > last_left) monotonic = false;
            last_left = out.left;
        }
        Check(!pressed, "no press before the reading time is up");
        Check(monotonic && last_left < 0.05f, "the pie empties steadily");
        in.now_ms = 1001.0;
        Check(p.Step(in).press, "a press once the reading time is up");
        in.now_ms = 1017.0;
        Check(!p.Step(in).press, "not twice for the same page");
        bool retried = false;   // the game keeps polling the page it did not turn
        for (double t = 1033.0; t <= 2100.0 && !retried; t += 16.0) {
            in.now_ms = t;
            retried = p.Step(in).press && t >= 2001.0;
        }
        Check(retried, "again after a while if the game did not take the press");
        in.page = 8;
        in.now_ms = 2116.0;
        const Pacer::Output fresh = p.Step(in);
        Check(!fresh.press && fresh.left == 1.0f, "a new page starts a full countdown");
        in.page = 0;
        in.now_ms = 2132.0;
        Check(!p.Step(in).counting, "no page shown: no pie");
    }
    {
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 500.0;
        in.after_voice_ms = 300.0;
        in.voice_playing = true;
        in.voice_end_ms = 2000.0;
        bool pressed = false;
        for (double t = 0.0; t <= 2000.0; t += 16.0) {
            in.now_ms = t;
            const Pacer::Output out = p.Step(in);
            pressed = pressed || out.press;
            if (t > 600.0 && t < 1900.0 && !out.waiting_for_voice) pressed = true;   // must say why it waits
        }
        Check(!pressed, "the voice holds the page past its reading time");
        in.voice_playing = false;
        in.now_ms = 2200.0;
        Check(!p.Step(in).press, "and for the pause after it");
        in.now_ms = 2320.0;
        Check(p.Step(in).press, "then turns it");
    }
    {
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 500.0;
        in.voice_playing = true;
        in.voice_end_ms = 300.0;   // the header said 300 ms, the game says it still plays
        for (double t = 0.0; t < 916.0; t += 16.0) {
            in.now_ms = t;
            p.Step(in);
        }
        in.now_ms = 916.0;
        Check(!p.Step(in).press, "a voice the game still plays holds the page whatever its header said");
        in.voice_playing = false;
        in.now_ms = 932.0;
        Check(p.Step(in).press, "and lets go once it stops");
    }
    {
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 500.0;
        in.voice_playing = true;   // a length nobody knows, and it never ends
        in.voice_cap_ms = 5000.0;
        bool early = false;
        for (double t = 0.0; t < 4990.0; t += 16.0) {
            in.now_ms = t;
            early = early || p.Step(in).press;
        }
        in.now_ms = 5001.0;
        Check(!early && p.Step(in).press, "a voice that never ends stops holding the page at the cap");
    }
    {
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 1000.0;
        in.now_ms = 0.0;
        p.Step(in);
        in.now_ms = 5000.0;   // the game did not poll for 5 s: paused, not read
        Check(!p.Step(in).press, "a long gap between polls is a pause");
        in.paused = true;
        bool pressed = false;
        for (double t = 5016.0; t < 9000.0; t += 16.0) {
            in.now_ms = t;
            pressed = pressed || p.Step(in).press;
        }
        Check(!pressed, "no press while the overlay has the input");
        in.paused = false;
        in.now_ms = 9016.0;
        const Pacer::Output out = p.Step(in);
        Check(!out.press && out.left > 0.9f, "the countdown resumes where it was");
    }
    {
        // An NPC's one-line message: opened again, it has the same key - and the game did not poll between.
        Pacer p;
        Pacer::Input in;
        in.page = 7;
        in.reading_ms = 1000.0;
        bool pressed = false;
        for (double t = 0.0; t <= 1100.0 && !pressed; t += 16.0) {
            in.now_ms = t;
            pressed = p.Step(in).press;
        }
        Check(pressed, "the one-line message is turned");
        in.now_ms = 6000.0;
        const Pacer::Output again = p.Step(in);
        Check(!again.press && again.left == 1.0f, "opened again, the same line starts a full countdown");
        in.now_ms = 6500.0;
        Check(!p.Step(in).press, "and is not turned before its time");
    }
    {
        // The same, with the player turning the line halfway through its countdown.
        Pacer p;
        Pacer::Input in;
        in.page = 7;
        in.reading_ms = 1000.0;
        for (double t = 0.0; t <= 600.0; t += 16.0) {
            in.now_ms = t;
            p.Step(in);
        }
        p.PlayerTurned(7, true);
        in.now_ms = 6000.0;
        Check(p.Step(in).left == 1.0f, "a line the player turned starts over when it is opened again");
    }
    {
        // A press while the text still types only finishes it: the countdown is not reset by it.
        Pacer p;
        Pacer::Input in;
        in.page = 7;
        in.ready = false;
        in.reading_ms = 1000.0;
        for (double t = 0.0; t <= 600.0; t += 16.0) {
            in.now_ms = t;
            p.Step(in);
        }
        p.PlayerTurned(7, false);
        in.now_ms = 5000.0;
        Check(p.Step(in).left < 0.5f, "a press on a typing page is not a turn");
    }

    {
        // Hold this line: the countdown stops, nothing presses, a second press resumes where it was.
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 1000.0;
        for (double t = 0.0; t <= 400.0; t += 16.0) {
            in.now_ms = t;
            p.Step(in);
        }
        in.toggle_hold = true;
        in.now_ms = 416.0;
        Check(p.Step(in).held, "a shown page can be held");
        in.toggle_hold = false;
        bool pressed = false;
        Pacer::Output out;
        for (double t = 432.0; t < 5000.0; t += 16.0) {
            in.now_ms = t;
            out = p.Step(in);
            pressed = pressed || out.press;
        }
        Check(!pressed && out.held, "a held page is never turned");
        Check(out.left > 0.5f && out.left < 0.65f, "and its countdown stands still");
        in.toggle_hold = true;
        in.now_ms = 5000.0;
        Check(!p.Step(in).held, "a second press lets it go");
        in.toggle_hold = false;
        for (double t = 5016.0; t < 5560.0; t += 16.0) {
            in.now_ms = t;
            pressed = pressed || p.Step(in).press;
        }
        Check(!pressed, "the countdown resumes where it stopped");
        in.now_ms = 5620.0;
        Check(p.Step(in).press, "and then turns the page");
    }
    {
        // Hold a page whose end the voice sets: the countdown stands still too, voice and all.
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 500.0;
        in.after_voice_ms = 300.0;
        in.voice_playing = true;
        in.voice_end_ms = 2000.0;
        for (double t = 0.0; t <= 1000.0; t += 16.0) {
            in.now_ms = t;
            p.Step(in);
        }
        in.toggle_hold = true;
        in.now_ms = 1008.0;
        const float before = p.Step(in).left;
        in.toggle_hold = false;
        Pacer::Output out;
        for (double t = 1024.0; t < 6000.0; t += 16.0) {
            in.now_ms = t;
            if (t > 2000.0) in.voice_playing = false;   // the voice ends while held
            out = p.Step(in);
        }
        Check(out.held && out.left > before - 0.02f && out.left < before + 0.02f,
              "a held page's voice countdown stands still");
        in.toggle_hold = true;
        in.now_ms = 6000.0;
        p.Step(in);
        in.toggle_hold = false;
        bool pressed = false;
        for (double t = 6016.0; t < 6250.0; t += 16.0) {
            in.now_ms = t;
            pressed = pressed || p.Step(in).press;
        }
        Check(!pressed, "let go, it still waits out the pause after the voice");
    }
    {
        // A hold pressed while the text still types holds the page it shows; it ends with that page.
        Pacer p;
        Pacer::Input in;
        in.reading_ms = 200.0;
        in.page = 0;
        in.now_ms = 0.0;
        p.Step(in);
        in.toggle_hold = true;
        in.now_ms = 16.0;
        Check(p.Step(in).hold_pending, "a hold pressed while typing waits for the page");
        in.toggle_hold = false;
        in.page = 3;
        bool pressed = false;
        Pacer::Output out;
        for (double t = 32.0; t < 2000.0; t += 16.0) {
            in.now_ms = t;
            out = p.Step(in);
            pressed = pressed || out.press;
        }
        Check(!pressed && out.held, "a hold asked for while typing holds the page once shown");
        in.page = 4;
        in.now_ms = 2016.0;
        Check(!p.Step(in).held, "the next page is not held");
        in.page = 0;
        in.now_ms = 2032.0;
        in.toggle_hold = true;
        p.Step(in);
        in.toggle_hold = false;
        in.now_ms = 9000.0;   // no polls for a long while: that dialog is over
        p.Step(in);
        in.page = 5;
        in.now_ms = 9016.0;
        Check(!p.Step(in).held, "a hold asked for between dialogs does not carry to the next one");
    }

    {
        // The key pressed while the next box opens: the game's last poll still showed the old page, the
        // next one shows the new page - the hold is for the new one (it used to be dropped).
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 5000.0;
        in.now_ms = 0.0;
        p.Step(in);
        in.page = 2;
        in.toggle_hold = true;
        in.now_ms = 400.0;   // no polls while the box opened
        const Pacer::Output out = p.Step(in);
        Check(out.held, "a hold pressed during the box's opening holds the new page");
    }

    {
        // StartWhileTyping: the countdown runs while the page types, but the page turns only once shown.
        Pacer p;
        Pacer::Input in;
        in.page = 9;
        in.ready = false;
        in.reading_ms = 500.0;
        bool pressed = false;
        Pacer::Output out;
        for (double t = 0.0; t < 900.0; t += 16.0) {
            in.now_ms = t;
            out = p.Step(in);
            pressed = pressed || out.press;
        }
        Check(!pressed && out.counting && out.left == 0.0f, "the time runs out while typing, no press yet");
        in.ready = true;
        in.now_ms = 916.0;
        Check(p.Step(in).press, "the page turns as soon as it is fully shown");
    }
    {
        Pacer p;
        Pacer::Input in;
        in.page = 9;
        in.ready = false;
        in.reading_ms = 2000.0;
        for (double t = 0.0; t < 500.0; t += 16.0) {
            in.now_ms = t;
            p.Step(in);
        }
        in.ready = true;   // the player pressed to finish the text: the same page, the same countdown
        in.now_ms = 516.0;
        const Pacer::Output out = p.Step(in);
        Check(!out.press && out.left < 0.8f, "finishing the text does not restart the countdown");
    }
    {
        Pacer p;
        Pacer::Input in;
        in.page = 1;
        in.reading_ms = 5000.0;
        in.toggle_hold = true;
        in.now_ms = 0.0;
        p.Step(in);
        in.toggle_hold = false;
        in.page = 2;
        in.now_ms = 16.0;
        Check(p.Step(in).hold_dropped, "a held page going away is reported");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "all passed" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
