// list_modules.cpp - print the modules loaded in a running process.
//
//   atmt_modules --pid 1234           list module paths
//   atmt_modules --pid 1234 --imports libpng.dll
//                                    print which loaded modules import a DLL
//
// Why: a proxy dll is only safe if nothing else in the process imports the dll it
// replaces. The winmm proxy crashed the game because nvwgf2um.dll (the NVIDIA
// driver, loaded into the game) imports functions the proxy did not export, so
// this lists exactly what is in the process.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

namespace {

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Which dlls does this module import? Returns lowercase names.
std::vector<std::string> ImportsOf(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 200 * 1024 * 1024) {
        std::fclose(f);
        return out;
    }
    std::vector<unsigned char> data(static_cast<size_t>(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return out;
    }
    std::fclose(f);

    auto u16 = [&](size_t off) -> unsigned {
        return data[off] | (static_cast<unsigned>(data[off + 1]) << 8);
    };
    auto u32 = [&](size_t off) -> unsigned long {
        return data[off] | (static_cast<unsigned>(data[off + 1]) << 8)
               | (static_cast<unsigned>(data[off + 2]) << 16)
               | (static_cast<unsigned long>(data[off + 3]) << 24);
    };
    if (data.size() < 0x40 || u16(0) != 0x5a4d) return out;
    const size_t nt = u32(0x3c);
    if (nt + 0x78 >= data.size() || u32(nt) != 0x00004550) return out;
    const size_t opt = nt + 0x18;
    const unsigned magic = u16(opt);
    const bool pe32plus = (magic == 0x20b);
    const size_t dirOff = opt + (pe32plus ? 0x70 : 0x60);
    const unsigned long importRva = u32(dirOff + 8);   // entry 1 = imports
    if (importRva == 0) return out;
    const size_t numSections = u16(nt + 6);
    const size_t sectionTable = opt + u16(nt + 0x14);
    const size_t sizeOfHeaders = u32(opt + 0x3c);
    auto rvaToOffset = [&](unsigned long rva) -> size_t {
        if (rva < sizeOfHeaders) return rva;
        for (size_t i = 0; i < numSections; ++i) {
            const size_t s = sectionTable + i * 40;
            if (s + 40 > data.size()) break;
            const unsigned long va = u32(s + 12);
            const unsigned long rawSize = u32(s + 16);
            const unsigned long rawPtr = u32(s + 20);
            if (rva >= va && rva < va + rawSize) return rawPtr + (rva - va);
        }
        return 0;
    };
    size_t off = rvaToOffset(importRva);
    if (off == 0 || off + 20 > data.size()) return out;
    for (;;) {
        if (off + 20 > data.size()) break;
        const unsigned long nameRva = u32(off + 12);
        if (nameRva == 0) break;
        const size_t nameOff = rvaToOffset(nameRva);
        if (nameOff == 0 || nameOff >= data.size()) break;
        std::string name(reinterpret_cast<const char*>(data.data() + nameOff));
        if (!name.empty()) out.push_back(Lower(name));
        off += 20;
    }
    return out;
}

}   // namespace

int main(int argc, char** argv) {
    DWORD pid = 0;
    std::string want;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pid" && i + 1 < argc) {
            pid = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--imports" && i + 1 < argc) {
            want = Lower(argv[++i]);
        }
    }
    if (pid == 0) {
        std::printf("usage: atmt_modules --pid <pid> [--imports <dllname>]\n");
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        std::printf("OpenProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
        std::printf("EnumProcessModulesEx failed (%lu)\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameExA(process, modules[i], path, MAX_PATH) == 0) continue;
        const std::string full(path);
        if (want.empty()) {
            std::printf("%s\n", full.c_str());
            continue;
        }
        for (const std::string& imported : ImportsOf(full)) {
            if (imported == want) {
                std::printf("imports %s: %s\n", want.c_str(), BaseName(full).c_str());
                break;
            }
        }
    }
    CloseHandle(process);
    return 0;
}
