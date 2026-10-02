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
// plus letters/digits (optionally with a [..] argument), and `#<digits><Letter>` is one code
// (#0T, #1P, #5S) - without that rule "#1P*yawn*" became "P*yawn*".
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
            const bool digits = j < n && isdigit(static_cast<unsigned char>(in[j])) != 0;
            while (j < n && isalnum(static_cast<unsigned char>(in[j])) != 0) {
                if (j > i + 1 && isupper(static_cast<unsigned char>(in[j])) != 0) {
                    const bool word_start =
                        (j + 1 >= n)
                        || islower(static_cast<unsigned char>(in[j + 1])) != 0
                        || isalnum(static_cast<unsigned char>(in[j + 1])) == 0;
                    if (word_start && !(digits && j == i + 2)) break;
                }
                ++j;
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
