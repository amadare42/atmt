// bgm_map.h - which song a music file is: FalcomBGMInfo's map, parsed and looked up.
//
// The map is FalcomBGMInfo's own (third_party/FalcomBGMInfo/assets/BgmMap_CS1.yaml), one line per
// track:
//     data\bgm\wav\ed8100.wav: "Suburban Town of Trista|近郊都市トリスタ|1|3|Trails of Cold Steel OST"
// i.e. "<path the game opens>: <title>|<Japanese title>|<disc>|<track>|<album>". Only that flat
// shape is read (no yaml library): a key, a colon, a quoted or plain value; '#' lines are comments.
//
// Lookups run inside the CreateFile hooks, on whatever thread opens a file, so Find allocates
// nothing and the map is never changed after it is loaded.
#ifndef BGM_MAP_H
#define BGM_MAP_H

#include <cstddef>
#include <string>
#include <vector>

namespace bgm {

struct Song {
    std::string path;       // lower case, '\' separators: "data\bgm\wav\ed8100.wav"
    std::string file;       // its last component: "ed8100.wav" (what the map is sorted by)
    std::string title;
    std::string japanese;   // empty when it is the same as the title
    std::string disc;
    std::string track;
    std::string album;
};

class Map {
public:
    // Parses `text` (UTF-8) and replaces what the map held. Returns the number of songs; lines that
    // are not a song are counted in *skipped (when given).
    size_t Load(const char* text, size_t size, size_t* skipped = nullptr);

    // The song whose path is a suffix of `path` (any case, '/' or '\'), or -1. `path` is what the
    // game passed to CreateFile: absolute or relative, it ends with the map's key either way.
    int Find(const char* path) const;
    int Find(const wchar_t* path) const;

    const Song& at(int index) const { return songs_[static_cast<size_t>(index)]; }
    size_t size() const { return songs_.size(); }

private:
    int FindNormalized(const char* path, size_t length) const;
    std::vector<Song> songs_;
};

// A music file: ends with .wav, .ogg or .opus (any case). The cheap test the hooks make first.
bool IsAudioPath(const char* path);
bool IsAudioPath(const wchar_t* path);

}  // namespace bgm

#endif  // BGM_MAP_H
