// payload.h - what the manager installs: the loader, the mods and their data, as components.
//
// Each component has its own version and is released, downloaded and updated on its own
// (updater.h, manifest.json). A component is a folder (bundled with the app, or unpacked from
// <name>-<version>.tar.gz):
//
//   component.json     {"name", "kind": "loader"|"mod"|"data", "version", "min_manager_version",
//                       "title", "default_enabled"}
//   files/...          files for the game folder, by their path there (the loader:
//                      files/GFSDK_SSAO_D3D11.win32.dll; a mod: files/atmt_mods/<name>.dll and its
//                      documented ini template files/atmt_mods/<name>.ini)
//   schema.json        optional: its part of the settings schema ({"input", "mods", "groups"}; the
//                      parts of every component are put together)
//   presets/*.json     the fixed presets          \
//   supported_exe.txt  md5 of the ed8.exe builds   |  any component may have them; the "data"
//   install_rules.json leftovers to remove         |  component has them
//   icon_pack.json     the icon pack's targets    /   (icon_pack.h)
//   manifest.md5       "<md5>  <path>" of every file above, '/' separated
//
// A payload is one component of each name: the newest one this app may use (min_manager_version)
// out of the bundled payload folders (a folder of component folders) and the downloaded ones in
// <DataDir>/components/<name>/<version>/. It needs a loader.
//
// Everything that is about a particular mod lives in these files; the manager only knows the
// loader's layout (the proxy dll, atmt_mods/, atmt_loader.ini) and the toolkit's atmt_ prefix.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "icon_pack.h"
#include "json.h"
#include "util.h"

namespace atmt {

constexpr const char* kProxyDll = "GFSDK_SSAO_D3D11.win32.dll";
constexpr const char* kOrigDll = "GFSDK_SSAO_D3D11.win32.orig.dll";
constexpr const char* kModDir = "atmt_mods";
constexpr const char* kDisabledDir = "atmt_mods/disabled";
// Every file and folder the toolkit puts into the game folder starts with this (a full removal
// takes exactly those).
constexpr const char* kToolkitPrefix = "atmt_";
// The component that is the loader.
constexpr const char* kLoaderComponent = "loader";

const char* AppVersion();

// -1 / 0 / 1 for "1.2.3" style versions; a "-suffix" (pre-release) sorts before the plain version.
int CompareVersions(const std::string& a, const std::string& b);

// A component name: [a-z0-9_]+ (it is a folder name and, for a mod, the dll's stem).
bool ValidComponentName(const std::string& name);

struct Component {
    std::string name;
    std::string kind;                 // "loader", "mod", "data"
    std::string version;
    std::string min_manager_version;
    std::string title;
    bool default_enabled = true;      // a mod: switched on when it is first installed
    fs::path dir;
    std::vector<std::pair<std::string, std::string>> manifest;   // path in dir -> md5

    static bool Load(const fs::path& dir, Component* out, std::string* error = nullptr);
    // Every file against manifest.md5; the first mismatch in `error`.
    bool VerifyFiles(std::string* error = nullptr) const;
    // The app may use it (min_manager_version).
    bool Usable() const;
};

struct ModInfo {
    std::string name;          // the dll's stem, e.g. "my_mod"
    std::string title;
    std::string description;   // the mod's own (its schema.json "mods"), not component.json
    std::string version;
    bool default_enabled = true;
};

struct PresetChange {
    std::string mod;           // a schema group ("atmt_loader" for the loader's ini)
    std::string section;
    std::string key;
    std::string value;
};

struct Preset {
    std::string id;            // file stem
    std::string name;
    std::string description;
    std::vector<PresetChange> changes;
    // "mods": {"<name>": true|false} - mods the preset switches on or off (atmt_mods/ vs
    // atmt_mods/disabled/); a mod that is not installed is left alone.
    std::vector<std::pair<std::string, bool>> mods;
    std::vector<std::string> recommended_on;   // "linux" / "windows": part of "Install everything" there
    bool icon_pack = false;    // "icon_pack": true - a small screen: "Install everything" builds the icon pack
};

struct SupportedExe {
    std::string md5;
    std::string note;
};

// A file for the game folder.
struct PayloadFile {
    std::string rel;           // its path in the game folder
    fs::path source;
    std::string md5;
    std::string component;
};

struct Payload {
    std::vector<Component> components;   // the loader first, then by name
    std::vector<PayloadFile> files;
    std::vector<ModInfo> mods;           // the mod components
    std::vector<Preset> presets;
    std::vector<SupportedExe> supported;
    Json schema;
    IconPackConfig icon_pack;                    // icon_pack.json (empty: no icon pack)
    std::vector<std::string> remove_rules;       // install_rules.json "remove"

    // One payload of the given components (one per name, a loader among them).
    static bool FromComponents(std::vector<Component> components, Payload* out, std::string* error = nullptr);
    // A payload folder: every component folder in it (what --payload names, and the bundled one).
    static bool Load(const fs::path& root, Payload* out, std::string* error = nullptr);

    const Component* FindComponent(const std::string& name) const;
    const ModInfo* FindMod(const std::string& name) const;
    const PayloadFile* FindFile(const std::string& rel) const;
    std::string ExpectedMd5(const std::string& rel) const;
    bool IsSupportedExe(const std::string& md5) const;
    // name -> version of every component.
    std::map<std::string, std::string> Versions() const;
    // "loader 1.2.0, 6 mods" - for a log line.
    std::string Summary() const;
    // Every component's files against its manifest.md5.
    bool VerifyFiles(std::string* error = nullptr) const;
};

// The bundled payload folders: $ATMT_PAYLOAD, <exe>/payload, <exe>/../share/atmt_manager/payload.
std::vector<fs::path> PayloadRoots();
// Where downloaded components go: <DataDir>/components/<name>/<version>/.
fs::path DownloadedComponentsDir();
// Every component that loads, bundled and downloaded (all versions, usable or not).
std::vector<Component> AvailableComponents();
// The newest of each name the running app may use, as one payload; false without a loader.
bool FindBestPayload(Payload* out, std::string* error = nullptr);

}  // namespace atmt
