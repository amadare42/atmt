// history.cpp - the ring of lines the panel shows (see history.h).
#include "history.h"

namespace atmt {

std::string LogLine::Display() const {
    if (speaker.empty()) return text;
    return speaker + ": " + text;
}

History::History() {
    InitializeCriticalSection(&lock_);
}

History::~History() {
    DeleteCriticalSection(&lock_);
}

void History::set_capacity(size_t lines) {
    // A zero cap would keep nothing, which is never what a settings file means: treat it as the
    // smallest useful ring instead (the panel reads the same number).
    if (lines < 1) lines = 1;
    EnterCriticalSection(&lock_);
    capacity_ = lines;
    TrimLocked();
    LeaveCriticalSection(&lock_);
}

size_t History::capacity() const {
    EnterCriticalSection(&lock_);
    const size_t out = capacity_;
    LeaveCriticalSection(&lock_);
    return out;
}

void History::Push(const LogLine& line) {
    EnterCriticalSection(&lock_);
    lines_.push_back(line);
    if (line.seq > newest_) newest_ = line.seq;
    TrimLocked();
    LeaveCriticalSection(&lock_);
}

void History::TrimLocked() {
    while (lines_.size() > capacity_) lines_.pop_front();
}

void History::Snapshot(std::vector<LogLine>& out) const {
    EnterCriticalSection(&lock_);
    out.assign(lines_.begin(), lines_.end());
    LeaveCriticalSection(&lock_);
}

size_t History::size() const {
    EnterCriticalSection(&lock_);
    const size_t out = lines_.size();
    LeaveCriticalSection(&lock_);
    return out;
}

uint64_t History::newest_seq() const {
    EnterCriticalSection(&lock_);
    const uint64_t out = newest_;
    LeaveCriticalSection(&lock_);
    return out;
}

void History::Clear() {
    EnterCriticalSection(&lock_);
    lines_.clear();
    LeaveCriticalSection(&lock_);
}

}  // namespace atmt
