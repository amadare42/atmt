// updater.cpp - see updater.h.
#include "updater.h"

#include <mutex>

#include "archive.h"
#include "hash.h"
#include "platform.h"
#include "release_config.h"
#include "sign.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace atmt {

const char* DefaultManifestUrl() { return ATMT_MANIFEST_URL; }
const char* ReleasePublicKey() { return ATMT_RELEASE_PUBKEY; }

namespace {

constexpr int64_t kCheckInterval = 24 * 3600;

fs::path StatePath() { return DataDir() / "state.json"; }

bool ParseDownload(const Json& j, Download* out, std::string* error) {
    out->url = j.Str("url");
    out->sha256 = Lower(j.Str("sha256"));
    out->size = static_cast<uint64_t>(j["size"].AsInt(0));
    if (!StartsWith(out->url, "https://")) {
        *error = "a download is not https: '" + out->url + "'";
        return false;
    }
    if (out->sha256.size() != 64) {
        *error = "a download has no sha256: " + out->url;
        return false;
    }
    return true;
}

}  // namespace

std::string ManifestUrl() {
    const std::string over = LoadAppState()["update"].Str("manifest_url");
    return over.empty() ? std::string(DefaultManifestUrl()) : over;
}

Json LoadAppState() {
    std::string text;
    Json j;
    if (!ReadFile(StatePath(), &text) || !Json::Parse(text, &j) || !j.is_object()) return Json::MakeObject();
    return j;
}

Json UpdateAppState(const std::function<void(Json&)>& change) {
    // One writer at a time, and each starts from what is on disk: the window and the update check
    // (a worker thread) change different keys, and neither may put back the other's old values.
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    Json state = LoadAppState();
    change(state);
    WriteFileAtomic(StatePath(), state.Dump());
    return state;
}

// ---------------------------------------------------------------- the manifest
bool Manifest::FromJson(const Json& j, Manifest* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = "manifest.json: " + why;
        return false;
    };
    if (j["format"].AsInt(0) != 1) return fail("unknown format " + j["format"].Dump(false) + " - a newer app is needed");
    Manifest m;
    m.published = j.Str("published");
    m.page = j.Str("page");
    const Json& app = j["manager"];
    m.manager_version = app.Str("version");
    m.manager_changelog = app.Str("changelog");
    std::string why;
    for (const auto& kv : app["downloads"].items()) {
        Download d;
        if (!ParseDownload(kv.second, &d, &why)) return fail("manager: " + why);
        m.manager_downloads[kv.first] = d;
    }
    for (const Json& c : j["components"].elements()) {
        RemoteComponent r;
        r.name = c.Str("name");
        r.kind = c.Str("kind");
        r.title = c.Str("title", r.name);
        r.version = c.Str("version");
        r.min_manager_version = c.Str("min_manager_version", "0.0.0");
        r.changelog = c.Str("changelog");
        if (!ValidComponentName(r.name)) return fail("a component's name '" + r.name + "' is not [a-z0-9_]+");
        if (r.version.empty()) return fail(r.name + " has no version");
        for (const RemoteComponent& other : m.components) {
            if (other.name == r.name) return fail(r.name + " is listed twice");
        }
        if (!ParseDownload(c, &r.download, &why)) return fail(r.name + ": " + why);
        m.components.push_back(r);
    }
    *out = std::move(m);
    return true;
}

// ---------------------------------------------------------------- comparing
std::vector<ComponentUpdate> UpdateCheck::Installable() const {
    std::vector<ComponentUpdate> out;
    for (const ComponentUpdate& u : components) {
        if (!u.needs_newer_app) out.push_back(u);
    }
    return out;
}

bool UpdateCheck::NeedsNewerApp() const {
    for (const ComponentUpdate& u : components) {
        if (u.needs_newer_app) return true;
    }
    return false;
}

void UpdateCheck::Compare(const std::map<std::string, std::string>& local_versions) {
    components.clear();
    for (const RemoteComponent& r : manifest.components) {
        auto it = local_versions.find(r.name);
        const std::string local = it != local_versions.end() ? it->second : std::string();
        if (!local.empty() && CompareVersions(r.version, local) <= 0) continue;
        ComponentUpdate u;
        u.remote = r;
        u.local_version = local;
        u.needs_newer_app = CompareVersions(r.min_manager_version, AppVersion()) > 0;
        components.push_back(u);
    }
    app_newer = !manifest.manager_version.empty() && CompareVersions(manifest.manager_version, AppVersion()) > 0;
    const bool any = app_newer || !components.empty();
    status = any ? Status::Available : Status::UpToDate;
    if (!any) {
        message = "up to date";
        return;
    }
    std::vector<std::string> names;
    if (app_newer) names.push_back("the app " + manifest.manager_version);
    for (const ComponentUpdate& u : components) names.push_back(u.remote.title + " " + u.remote.version);
    message = "updates available: " + Join(names, ", ");
}

bool Updater::VerifyManifest(const std::string& text, const std::string& sig, const std::string& public_key,
                             Manifest* out, std::string* error) {
    PublicKey key;
    if (!ParsePublicKey(public_key, &key, error)) return false;
    std::string why;
    if (!VerifySignature(key, text, sig, &why)) {
        if (error != nullptr) *error = "manifest.json: " + why + " - update refused";
        return false;
    }
    Json j;
    if (!Json::Parse(text, &j, &why)) {
        if (error != nullptr) *error = "manifest.json: " + why;
        return false;
    }
    return Manifest::FromJson(j, out, error);
}

// ---------------------------------------------------------------- checking
bool Updater::Fetch(const std::string& url, std::string* body, std::string* error, const ProgressFn& progress) {
    if (http_ == nullptr) {
        if (error != nullptr) *error = "no HTTPS client on this system";
        return false;
    }
    HttpResponse resp;
    if (!http_->Get(url, {{"Accept", "application/octet-stream"}}, &resp, progress) || resp.status != 200) {
        if (error != nullptr) {
            *error = resp.error.empty() ? "HTTP " + std::to_string(resp.status) + " for " + url : resp.error;
        }
        return false;
    }
    *body = std::move(resp.body);
    return true;
}

UpdateCheck Updater::Check(bool force, const std::map<std::string, std::string>& local_versions) {
    UpdateCheck c;
    const std::string url = ManifestUrl();
    if (url.empty() || ReleasePublicKey()[0] == '\0') {
        c.status = UpdateCheck::Status::NotConfigured;
        c.message = "this build has no manifest URL or release key configured";
        return c;
    }
    const Json state = LoadAppState();
    const Json& u = state["update"];
    const int64_t now = UnixNow();
    auto save = [](const std::function<void(Json&)>& change) {
        UpdateAppState([&](Json& s) { change(s["update"]); });
    };
    auto use = [&](const Manifest& m, bool from_cache, const std::string& checked_at) {
        c.manifest = m;
        c.from_cache = from_cache;
        c.checked_at = checked_at;
        c.Compare(local_versions);
        return c;
    };
    Manifest cached_manifest;
    const bool cached = u["manifest"].is_object() && u.Str("manifest_checked_url") == url
                        && Manifest::FromJson(u["manifest"], &cached_manifest);
    if (!force && cached && now - u["last_check"].AsInt(0) < kCheckInterval) {
        return use(cached_manifest, true, u.Str("checked_at"));
    }

    // Failures are quiet: offline, rate limited, timed out - a line in System, nothing more.
    auto failed = [&](const std::string& why) {
        save([&](Json& v) {
            v["last_error"] = why;
            v["last_error_at"] = IsoTimeUtc();
        });
        c.status = UpdateCheck::Status::Failed;
        c.message = "couldn't check for updates: " + why;
        return c;
    };
    if (http_ == nullptr) return failed("no HTTPS client on this system");
    std::vector<std::pair<std::string, std::string>> headers = {{"Accept", "application/octet-stream"}};
    if (cached && !u.Str("etag").empty()) headers.emplace_back("If-None-Match", u.Str("etag"));
    HttpResponse resp;
    http_->Get(url, headers, &resp);
    if (resp.status == 304 && cached) {
        const std::string checked_at = IsoTimeUtc();
        save([&](Json& v) {
            v["last_check"] = now;
            v["checked_at"] = checked_at;
            v["last_error"] = "";
        });
        return use(cached_manifest, true, checked_at);
    }
    if (resp.status == 404) return failed("no release published yet");
    if (resp.status == 403 || resp.status == 429) return failed("rate limited - try again later");
    if (resp.status != 200) return failed(resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error);

    std::string sig, why;
    if (!Fetch(url + ".sig", &sig, &why)) return failed(why);
    Manifest m;
    if (!VerifyManifest(resp.body, sig, ReleasePublicKey(), &m, &why)) {
        // not quiet: a bad signature is worth seeing
        Log("update: " + why);
        return failed(why);
    }
    // A manifest older than one this URL already gave is refused: the signature alone cannot tell an
    // old (validly signed) manifest served again - which would hide every newer update - from the newest.
    // "published" is ISO 8601 UTC, so the strings compare as the times do.
    const std::string seen = u.Str("newest_published_url") == url ? u.Str("newest_published") : std::string();
    if (!seen.empty() && (m.published.empty() || m.published < seen)) {
        why = "the release manifest (published " + (m.published.empty() ? std::string("?") : m.published)
              + ") is older than one already seen (" + seen + ") - refused";
        Log("update: " + why);
        return failed(why);
    }
    Json parsed;
    Json::Parse(resp.body, &parsed);
    const std::string checked_at = IsoTimeUtc();
    save([&](Json& v) {
        v["manifest"] = parsed;
        v["manifest_checked_url"] = url;
        v["etag"] = resp.etag;
        v["last_check"] = now;
        v["checked_at"] = checked_at;
        v["last_error"] = "";
        if (!m.published.empty()) {
            v["newest_published"] = m.published;
            v["newest_published_url"] = url;
        }
    });
    return use(m, false, checked_at);
}

// ---------------------------------------------------------------- components
bool Updater::InstallComponentArchive(const RemoteComponent& remote, const std::string& archive, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = remote.name + " " + remote.version + ": " + why;
        return false;
    };
    if (!ValidComponentName(remote.name)) return fail("not a component name");
    for (const Component& have : AvailableComponents()) {
        if (have.name == remote.name && CompareVersions(remote.version, have.version) <= 0) {
            return fail("not newer than " + have.version + " (already here) - downgrades are refused");
        }
    }
    if (Sha256Hex(archive) != remote.download.sha256) return fail("the download does not match the manifest's sha256 - refused");

    const fs::path root = DownloadedComponentsDir() / remote.name;
    const fs::path tmp = root / ("." + remote.version + ".tmp");
    const fs::path dest = root / remote.version;
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::string tar, why;
    if (!Gunzip(archive, &tar, &why) || !ExtractTar(tar, tmp, &why)) {
        fs::remove_all(tmp, ec);
        return fail(why);
    }
    Component c;
    if (!Component::Load(tmp, &c, &why) || !c.VerifyFiles(&why)) {
        fs::remove_all(tmp, ec);
        return fail("the download is not a valid component: " + why);
    }
    if (c.name != remote.name || c.version != remote.version) {
        fs::remove_all(tmp, ec);
        return fail("the archive is " + c.name + " " + c.version + ", not what the manifest says");
    }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) return fail("cannot move it into place: " + ec.message());
    // Older downloads of it are never used again (a newer one is here now).
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string v = U8(it->path().filename());
        if (it->path() == dest || StartsWith(v, ".")) continue;
        if (CompareVersions(v, remote.version) < 0) {
            std::error_code rec;
            fs::remove_all(it->path(), rec);
        }
    }
    Log("update: " + remote.name + " " + remote.version + " verified and unpacked into " + U8(dest));
    return true;
}

bool Updater::DownloadComponents(const std::vector<ComponentUpdate>& updates, std::string* error,
                                 const ProgressFn& progress) {
    for (const ComponentUpdate& u : updates) {
        if (u.needs_newer_app) {
            if (error != nullptr) *error = u.remote.title + " " + u.remote.version + " needs app " + u.remote.min_manager_version;
            return false;
        }
        std::string data, why;
        Log("update: downloading " + u.remote.name + " " + u.remote.version + " ...");
        if (!Fetch(u.remote.download.url, &data, &why, progress)) {
            if (error != nullptr) *error = u.remote.title + ": " + why;
            return false;
        }
        if (!InstallComponentArchive(u.remote, data, error)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- the app itself
bool Updater::UpdateApp(const Manifest& manifest, std::string* error, const ProgressFn& progress) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
#ifndef _WIN32
    if (!RunningFromAppImage()) return fail("this copy is not an AppImage - download the new version from the release page");
#endif
    auto it = manifest.manager_downloads.find(PlatformName());
    if (it == manifest.manager_downloads.end()) return fail("this release has no app for this system");
    const Download& d = it->second;
    const fs::path self = SelfExe();
    const fs::path fresh = fs::path(self).concat(".new");
    {   // writable at all? (a read-only location: say so and link the release page)
        std::string probe_err;
        if (!WriteFileAtomic(fresh, "", &probe_err)) return fail("the app's folder is not writable - download the new version from the release page");
    }
    std::string data, why;
    Log("update: downloading the app " + manifest.manager_version + " ...");
    if (!Fetch(d.url, &data, &why, progress)) return fail(why);
    if (Sha256Hex(data) != d.sha256) return fail("the app download does not match the manifest's sha256 - update refused");
    if (!WriteFileAtomic(fresh, data, &why)) return fail(why);
    std::error_code ec;
    std::string cleanup_file;
    int64_t cleanup_after_runs = 0;
#ifdef _WIN32
    // A running exe cannot be overwritten, but it can be renamed: the new instance deletes the old.
    const fs::path old = fs::path(self).concat(".old");
    fs::remove(old, ec);
    fs::rename(self, old, ec);
    if (ec) return fail("cannot rename the running app: " + ec.message());
    fs::rename(fresh, self, ec);
    if (ec) {
        fs::rename(old, self, ec);
        return fail("cannot put the new app in place");
    }
    cleanup_file = U8(old);
#else
    chmod(fresh.c_str(), 0755);
    // The old one stays for one run as .prev; the Game Mode shortcut points at the stable path.
    const fs::path prev = fs::path(self).concat(".prev");
    fs::remove(prev, ec);
    fs::copy_file(self, prev, ec);
    fs::rename(fresh, self, ec);
    if (ec) return fail("cannot put the new AppImage in place: " + ec.message());
    cleanup_file = U8(prev);
    cleanup_after_runs = 1;
#endif
    UpdateAppState([&](Json& state) {
        state["cleanup_file"] = cleanup_file;
        if (cleanup_after_runs > 0) state["cleanup_after_runs"] = cleanup_after_runs;
    });
    Log("update: the app was replaced by " + manifest.manager_version + " - it is used from the next start");
    return true;
}

void CleanupAfterAppUpdate() {
    if (LoadAppState().Str("cleanup_file").empty()) return;
    UpdateAppState([](Json& state) {
        const std::string file = state.Str("cleanup_file");
        if (file.empty()) return;
        const int64_t runs = state["cleanup_after_runs"].AsInt(0);
        if (runs > 0) {
            state["cleanup_after_runs"] = runs - 1;
        } else {
            std::error_code ec;
            fs::remove(Path(file), ec);
            state.Erase("cleanup_file");
            state.Erase("cleanup_after_runs");
        }
    });
}

}  // namespace atmt
