// updater.h - update checks and updates from a signed manifest (docs/MANAGER.md "Releases").
//
// The app asks one URL for manifest.json - by default the latest GitHub release's
// (https://github.com/<repo>/releases/latest/download/manifest.json) - and the same URL + ".sig" for
// its minisign signature. The manifest names the newest version of the manager and of every
// component (payload.h), each with its own download:
//
//   {"format": 1, "published": "...", "page": "<release page>",
//    "manager": {"version", "changelog",
//                "downloads": {"windows": {"url", "sha256", "size"}, "linux": {...}}},
//    "components": [{"name", "kind", "title", "version", "min_manager_version", "changelog",
//                    "url", "sha256", "size"}, ...]}
//
// Each of them has its own lifecycle: a release that changes one mod carries that mod's archive
// and a manifest whose other entries still point at the downloads of earlier releases.
//
// Nothing is trusted before the signature verifies with the key compiled into the app; every
// download must then match its sha256 in the manifest. A component not newer than the newest one
// already here is refused, and so is a manifest published before the newest one already seen from
// the same URL (an old, validly signed manifest served again would hide every update). Checking is automatic (at most once a day, with If-None-Match);
// installing asks unless the user turned automatic installation on.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "http.h"
#include "json.h"
#include "payload.h"
#include "util.h"

namespace atmt {

// The manifest URL and public key this build was configured with (empty: updates are off).
// state.json's update.manifest_url overrides the URL (a test channel); the key cannot be changed.
const char* DefaultManifestUrl();
std::string ManifestUrl();
const char* ReleasePublicKey();

// The manager's own state: <DataDir>/state.json (update settings, the last check, the game folder).
Json LoadAppState();
// The only way to change it: loads the file, applies `change` and saves it, under one lock, so
// writers from different threads (the window, the update check) never undo each other's keys.
// Returns the state as saved.
Json UpdateAppState(const std::function<void(Json&)>& change);

struct Download {
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};

struct RemoteComponent {
    std::string name;
    std::string kind;
    std::string title;
    std::string version;
    std::string min_manager_version;
    std::string changelog;
    Download download;
};

struct Manifest {
    std::string published;
    std::string page;                            // the release page, for the "Release page" button
    std::string manager_version;
    std::string manager_changelog;
    std::map<std::string, Download> manager_downloads;   // "windows" / "linux"
    std::vector<RemoteComponent> components;

    static bool FromJson(const Json& j, Manifest* out, std::string* error = nullptr);
};

// A component the manifest has newer than this app's payload (or that the payload lacks).
struct ComponentUpdate {
    RemoteComponent remote;
    std::string local_version;     // "" = new
    bool needs_newer_app = false;  // its min_manager_version is above this app: update the app first
};

struct UpdateCheck {
    enum class Status { NotConfigured, UpToDate, Available, Failed };
    Status status = Status::Failed;
    std::string message;
    Manifest manifest;
    std::vector<ComponentUpdate> components;
    bool app_newer = false;
    bool from_cache = false;     // not due yet (or 304 Not Modified): the last verified answer
    std::string checked_at;

    // The component updates this app can install now.
    std::vector<ComponentUpdate> Installable() const;
    bool NeedsNewerApp() const;
    // Fills components / app_newer / status / message from the manifest and the local versions.
    void Compare(const std::map<std::string, std::string>& local_versions);
};

class Updater {
public:
    // `http` may be null (no client on this system): every call then fails quietly.
    explicit Updater(Http* http) : http_(http) {}

    // `local_versions`: name -> version of the payload the app uses (Payload::Versions). `force`
    // ignores the once-a-day limit ("Check now").
    UpdateCheck Check(bool force, const std::map<std::string, std::string>& local_versions);

    // Downloads, verifies and unpacks each into <DataDir>/components/<name>/<version>; stops at the
    // first failure (the ones before it stay).
    bool DownloadComponents(const std::vector<ComponentUpdate>& updates, std::string* error,
                            const ProgressFn& progress = nullptr);

    // Replaces the running app with the manifest's (AppImage on Linux, the exe on Windows); it
    // takes effect on the next start. False with a reason when it cannot (not an AppImage, a
    // read-only folder): the GUI then links the release page instead.
    bool UpdateApp(const Manifest& manifest, std::string* error, const ProgressFn& progress = nullptr);

    // No network, for tests too: the manifest against its signature.
    static bool VerifyManifest(const std::string& text, const std::string& sig, const std::string& public_key,
                               Manifest* out, std::string* error);
    // A downloaded component archive: its sha256, then unpacked, checked (component.json matches the
    // manifest's entry, manifest.md5) and moved into <DataDir>/components/<name>/<version>; older
    // downloaded versions of it are removed.
    static bool InstallComponentArchive(const RemoteComponent& remote, const std::string& archive, std::string* error);

private:
    bool Fetch(const std::string& url, std::string* body, std::string* error, const ProgressFn& progress = nullptr);
    Http* http_;
};

// At start-up: removes what the previous app update left (Windows: the renamed old exe; Linux:
// the .prev AppImage once the new one has run once).
void CleanupAfterAppUpdate();

}  // namespace atmt
