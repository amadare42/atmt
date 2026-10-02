// history.h - the lines the panel shows, kept where they are written.
//
// The log already has every line the player read: LogSink::Line writes it from the game's own
// thread. The panel needs the same lines in memory - newest last, bounded, readable from the
// render thread - so the sink records them here as it writes and the panel takes a snapshot when
// it draws. Deliberately free of ImGui and of the game, so the offline self test can check the
// order, the cap and the formatting without a GPU.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace atmt {

struct LogLine {
    uint64_t seq = 0;      // the sequence number the jsonl record carries
    std::string time;      // "HH:MM:SS", the same timestamp the log writes
    std::string speaker;   // the game's own name plate; empty when the game drew none
    std::string text;      // what the player read (inline codes already stripped)

    // "Speaker: text" (or just the text). One formatting for atmt_latest.txt and the panel alike.
    std::string Display() const;
};

// A bounded ring of the newest lines. One writer (the game's thread, through the sink), two
// readers (the render thread when the panel is open, the self test). The lock is a critical
// section, like everywhere else in this mod: it is the cheapest thing that cannot deadlock here.
class History {
public:
    History();
    ~History();
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // The cap is both the memory bound and the scrollback: the panel can never show more than
    // this. Shrinking drops the oldest lines.
    void set_capacity(size_t lines);
    size_t capacity() const;
    void Push(const LogLine& line);
    // Copies the lines, oldest first, into `out` (which is cleared first).
    void Snapshot(std::vector<LogLine>& out) const;
    size_t size() const;
    uint64_t newest_seq() const;   // 0 when nothing was logged yet
    void Clear();

private:
    void TrimLocked();
    mutable CRITICAL_SECTION lock_;
    std::deque<LogLine> lines_;
    size_t capacity_ = 500;
    uint64_t newest_ = 0;
};

}  // namespace atmt
