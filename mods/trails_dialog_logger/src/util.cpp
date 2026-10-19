// util.cpp - the small pieces the logger needs from the text and the process.
#include "atmt.h"

#include <cstring>
#include <string>

namespace atmt {

namespace {
void* g_self_module = nullptr;
}

void SetSelfModule(void* module) {
    g_self_module = module;
}

void* SelfModuleHandle() {
    return g_self_module;
}

// Remove the inline `#`-codes and flatten newlines: what the player reads. A code is `#`
// plus letters/digits (optionally with a [..] argument). Two shapes have an exact length,
// measured over every dat_us script, and are not left to the word guard:
//   #<digits><letter>          #0T, #1P, #5S, #003c - the text starts right after the letter
//                              ("#1P*yawn*", "#5SAAAAHHH!", "#003caccidents")
//   #E / #M + one argument     digits, [..], or a single letter: #E6, #E[1], #EI, #MA, #MB
//                              ("#EI#M[[autoM4]]Anyway" left a stray "I", "#EEI could" an "E")
std::string StripCodes(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = in[i];
        if (c == '\x01') {
            out.push_back(' ');   // the game's own break inside one message
            continue;
        }
        if (c == '#') {
            size_t j = i + 1;
            auto is_digit = [&](size_t k) {
                return k < n && isdigit(static_cast<unsigned char>(in[k])) != 0;
            };
            auto is_alpha = [&](size_t k) {
                return k < n && isalpha(static_cast<unsigned char>(in[k])) != 0;
            };
            if (is_digit(j)) {   // #<digits><letter>
                while (is_digit(j)) ++j;
                if (is_alpha(j)) ++j;
            } else if (j < n && (in[j] == 'E' || in[j] == 'M')) {   // #E / #M + one argument
                ++j;
                if (is_digit(j)) {
                    while (is_digit(j)) ++j;
                } else if (is_alpha(j)) {
                    ++j;
                }
            } else {   // the rest (#K, #F, ...): letters/digits up to the word guard
                while (j < n && isalnum(static_cast<unsigned char>(in[j])) != 0) {
                    if (j > i + 1 && isupper(static_cast<unsigned char>(in[j])) != 0) {
                        const bool word_start =
                            (j + 1 >= n)
                            || islower(static_cast<unsigned char>(in[j + 1])) != 0
                            || isalnum(static_cast<unsigned char>(in[j + 1])) == 0;
                        if (word_start) break;
                    }
                    ++j;
                }
            }
            if (j < n && in[j] == '[') {   // optional argument, e.g. #M[[autoM0]]
                int depth = 0;
                while (j < n) {
                    if (in[j] == '[') {
                        ++depth;
                    } else if (in[j] == ']') {
                        --depth;
                        if (depth == 0) {
                            ++j;
                            break;
                        }
                    }
                    ++j;
                }
            }
            i = j - 1;   // the loop's ++i moves to the first byte after the code
            continue;
        }
        out.push_back(c);
    }
    // collapse whitespace runs and trim, because a code often leaves one behind
    std::string flat;
    flat.reserve(out.size());
    bool last_space = true;
    for (char c : out) {
        const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (space) {
            if (!last_space) flat.push_back(' ');
            last_space = true;
        } else {
            flat.push_back(c);
            last_space = false;
        }
    }
    while (!flat.empty() && flat.back() == ' ') flat.pop_back();
    return flat;
}

bool IsExecutableCode(const void* p) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Type != MEM_IMAGE) return false;   // code of a loaded module
    const DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                              | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kExecutable) != 0;
}

}  // namespace atmt
