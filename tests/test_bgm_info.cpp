// test_bgm_info.cpp - the bgm_info mod's song map, offline (mods/bgm_info/src/bgm_map.h).
//
// No game, no loader. What decides which title the toast shows: FalcomBGMInfo's map as compiled
// into the mod parses completely, and the paths the game hands CreateFile find their song - in any
// case, with either separator, absolute or relative - while other audio does not.
//
// usage: atmt_bgm_test
#include "bgm_map.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "bgm_map_cs1.inc"

static int g_failures = 0;

static void Check(bool cond, const char* what) {
    std::printf("%s %s\n", cond ? "[ ok ]" : "[FAIL]", what);
    if (!cond) ++g_failures;
}

static std::string TitleOf(const bgm::Map& map, int index) {
    return index >= 0 ? map.at(index).title : std::string("(none)");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // --- the built-in map ---------------------------------------------------------------
    bgm::Map map;
    size_t skipped = 99;
    const size_t count = map.Load(reinterpret_cast<const char*>(kBgmMapCs1), sizeof(kBgmMapCs1), &skipped);
    std::printf("       built-in map: %u songs, %u lines skipped\n", static_cast<unsigned>(count),
                static_cast<unsigned>(skipped));
    Check(count > 80, "the built-in Cold Steel I map has its songs");
    Check(skipped == 0, "every line of the built-in map is a song or a comment");

    const int trista = map.Find("data\\bgm\\wav\\ed8100.wav");
    Check(TitleOf(map, trista) == "Suburban Town of Trista", "a relative path finds its song");
    if (trista >= 0) {
        const bgm::Song& s = map.at(trista);
        Check(s.japanese == "\xE8\xBF\x91\xE9\x83\x8A\xE9\x83\xBD\xE5\xB8\x82\xE3\x83\x88\xE3\x83\xAA"
                            "\xE3\x82\xB9\xE3\x82\xBF",
              "the Japanese title is kept as UTF-8");
        Check(s.disc == "1" && s.track == "3" && s.album == "Trails of Cold Steel OST",
              "disc, track and album are split out");
    }
    Check(map.Find("D:\\Games\\Trails of Cold Steel\\data\\bgm\\wav\\ed8100.wav") == trista,
          "an absolute path finds the same song");
    Check(map.Find("./DATA/BGM/WAV/ED8100.WAV") == trista, "case and '/' separators do not matter");
    Check(map.Find(L"C:\\game\\data\\bgm\\wav\\ed8100.wav") == trista, "a wide path finds it too");
    Check(map.Find("data\\voice\\wav\\ed8100.wav") < 0, "the same file name elsewhere is not the song");
    Check(map.Find("xdata\\bgm\\wav\\ed8100.wav") < 0, "the key must match whole path components");
    Check(map.Find("data\\bgm\\wav\\ed8413.wav") < 0, "a commented-out line is not a song");
    Check(map.Find("data\\bgm\\wav\\ed8530b.wav") >= 0, "a letter-suffixed file name is a song");
    Check(map.Find("data\\bgm\\wav\\ed8402.wav") >= 0
              && map.at(map.Find("data\\bgm\\wav\\ed8402.wav")).japanese.empty(),
          "a Japanese title equal to the English one is dropped");
    Check(TitleOf(map, map.Find("data\\bgm\\wav\\ed8109.wav")) == "Dining Bar 'F'",
          "a title with quotes inside the value survives");

    // --- the cheap test in front of the lookup ------------------------------------------
    Check(bgm::IsAudioPath("data\\bgm\\wav\\ed8100.WAV"), ".WAV is audio");
    Check(bgm::IsAudioPath(L"x.opus") && bgm::IsAudioPath("x.ogg"), ".opus and .ogg are audio");
    Check(!bgm::IsAudioPath("data\\scripts\\scena\\dat_us\\t0000.dat"), "a script is not audio");
    Check(!bgm::IsAudioPath(".wa") && !bgm::IsAudioPath(static_cast<const char*>(nullptr)),
          "short and null paths are not audio");

    // --- a map of one's own (bgm_info_map.yaml) ------------------------------------------
    const char own[] =
        "# comment\r\n"
        "\r\n"
        "data/bgm/wav/a.wav: \"One|One|1|2|OST\"\r\n"
        "'data\\bgm\\wav\\b.ogg': 'It''s|\xE6\x97\xA5|3|4|OST'\n"
        "data\\bgm\\wav\\a.wav: \"One again\"\n"
        "not a song\n";
    const size_t own_count = map.Load(own, sizeof(own) - 1, &skipped);
    Check(own_count == 2 && skipped == 1, "a hand-written map: songs counted, junk skipped");
    Check(TitleOf(map, map.Find("data\\bgm\\wav\\a.wav")) == "One again", "a path listed twice: the last wins");
    Check(TitleOf(map, map.Find("data\\bgm\\wav\\b.ogg")) == "It's", "single-quoted values unescape ''");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
