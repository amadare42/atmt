// test_manager.cpp - manager_core without a game: hashes, signatures, archives, the settings model,
// the whole install cycle (install, verify, rollback, toggle, restore, uninstall) on a sandbox
// game folder with a payload built here, and the icon pack builder (Falcom packages, .p3a archives,
// the resampler, staleness) on synthetic textures.
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/archive.h"
#include "core/falcom_pkg.h"
#include "core/hash.h"
#include "core/icon_pack.h"
#include "core/install.h"
#include "core/json.h"
#include "core/locator.h"
#include "core/p3a.h"
#include "core/payload.h"
#include "core/settings.h"
#include "core/platform.h"
#include "core/senpatcher.h"
#include "core/sign.h"
#include "core/steam_shortcuts.h"
#include "core/updater.h"
#include "core/util.h"

using namespace atmt;

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

std::string Read(const fs::path& p) {
    std::string s;
    ReadFile(p, &s);
    return s;
}

void Write(const fs::path& p, const std::string& s) { WriteFileAtomic(p, s); }

void SetEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

const unsigned char kTarGz[] = {
#include "tar_fixture.inc"
};

const unsigned char kZip[] = {
#include "zip_fixture.inc"
};

// ---------------------------------------------------------------- the basics
void TestHashes() {
    Check(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e", "md5 of nothing");
    Check(Md5Hex("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6", "md5 of the fox");
    Check(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    Check(Sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
          "sha256 across blocks");
    std::string out;
    Check(Base64Encode("Man") == "TWFu" && Base64Encode("Ma") == "TWE=" && Base64Encode("M") == "TQ==", "base64 encode");
    Check(Base64Decode("TWE=", &out) && out == "Ma", "base64 decode");
    Check(CompareVersions("1.2.10", "1.2.9") > 0 && CompareVersions("1.0.0-rc1", "1.0.0") < 0
              && CompareVersions("v2.0", "2.0.0") == 0,
          "version comparison");
}

void TestJson() {
    Json j;
    std::string err;
    Check(Json::Parse("{\"a\": [1, 2.5, true, null, \"x\\u00e9\\n\"], \"b\": {\"c\": \"d\"}}", &j, &err), "json parses");
    Check(j["a"].size() == 5 && j["a"].elements()[1].AsNumber() == 2.5 && j["b"].Str("c") == "d", "json values");
    Check(j["a"].elements()[4].AsString() == "x\xc3\xa9\n", "json unicode escape");
    Json back;
    Check(Json::Parse(j.Dump(), &back) && back.Dump() == j.Dump(), "json round trip keeps order");
    Check(!Json::Parse("{\"a\": }", &back, &err) && !err.empty(), "json rejects garbage");
}

void TestSignatures() {
    std::string pub, sec, sig, err, comment;
    Check(GenerateKeyPair(&pub, &sec, &err), "key pair");
    PublicKey key;
    Check(ParsePublicKey(pub, &key, &err), "public key parses");
    const std::string msg = "0123  payload-1.0.0.tar.gz\n";
    Check(SignMessage(sec, msg, "timestamp:1", &sig, &err), "signs");
    Check(VerifySignature(key, msg, sig, &err, &comment) && comment == "timestamp:1", "signature verifies");
    Check(!VerifySignature(key, msg + "x", sig, &err), "a changed message fails");
    std::string forged = sig;
    forged.replace(forged.find("timestamp:1"), 11, "timestamp:2");
    Check(!VerifySignature(key, msg, forged, &err), "a changed trusted comment fails");
    std::string pub2, sec2;
    GenerateKeyPair(&pub2, &sec2, &err);
    PublicKey other;
    ParsePublicKey(pub2, &other, &err);
    Check(!VerifySignature(other, msg, sig, &err), "another key fails");

}

void TestArchive(const fs::path& tmp) {
    const std::string gz(reinterpret_cast<const char*>(kTarGz), sizeof(kTarGz));
    std::string tar, err;
    Check(Gunzip(gz, &tar, &err), "gunzip (" + err + ")");
    const fs::path dest = tmp / "untar";
    Check(ExtractTar(tar, dest, &err), "untar (" + err + ")");
    Check(Read(dest / "sub/hello.txt").size() == 14 * 50, "a file in a folder");
    Check(Read(dest / Path(std::string(120, 'x') + ".txt")) == "long name", "a pax long name");
    Check(Md5Hex(Read(dest / "words.txt")) == "722fa85014d3f78a6822832a3b5e852c", "dynamic huffman content");
    std::string bad = gz;
    bad[bad.size() - 6] ^= 1;
    Check(!Gunzip(bad, &tar, &err), "a damaged gzip is refused");
    // an entry escaping the folder
    std::string evil(1024, '\0');
    std::memcpy(&evil[0], "../evil.txt", 11);
    std::memcpy(&evil[100], "0000644", 7);
    std::memcpy(&evil[124], "00000000000", 11);
    evil[156] = '0';
    std::memcpy(&evil[257], "ustar", 5);
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(evil[i]);
    char cks[8];
    std::snprintf(cks, sizeof(cks), "%06o", sum);
    std::memcpy(&evil[148], cks, 7);
    Check(!ExtractTar(evil, dest, &err) && !Exists(tmp / "evil.txt"), "a path outside the folder is refused");
}

void TestVdf() {
    const std::string vdf =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
        "\t\t\"apps\"\n\t\t{\n\t\t\t\"538680\"\t\t\"20295130780\"\n\t\t}\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/run/media/deck/SD\"\n\t}\n}\n";
    Vdf root;
    Check(ParseVdf(vdf, &root), "vdf parses");
    const Vdf* lf = root.Find("LibraryFolders");
    Check(lf != nullptr && lf->children.size() == 2, "vdf blocks");
    Check(lf != nullptr && lf->Find("0")->Str("path") == "C:\\Program Files (x86)\\Steam", "vdf escapes");
}

// ---------------------------------------------------------------- the other stores
void TestLocator(const fs::path& tmp) {
    const fs::path root = tmp / "stores";
    std::error_code ec;
    fs::remove_all(root, ec);
    auto game_at = [](const fs::path& dir) {
        Write(dir / "ed8.exe", "ed8");
        Write(dir / kProxyDll, "dll");
    };
    auto json_str = [](const fs::path& p) {   // a path as a JSON string
        std::string s;
        for (char c : U8(p)) s += c == '\\' ? std::string("\\\\") : std::string(1, c);
        return "\"" + s + "\"";
    };
    auto same = [](const fs::path& a, const fs::path& b) {
        std::error_code e;
        return fs::equivalent(a, b, e);
    };

    // a plain folder (GOG's default place), and one whose sub-folder has ed8.exe
    const fs::path plain_game = root / "GOG Games" / "Trails of Cold Steel";
    game_at(plain_game);
    const fs::path nested = root / "Nested" / "ColdSteel";
    game_at(nested / "Game");
    std::vector<GameInstall> found;

    // Heroic: GOG's installed.json with the prefix from GamesConfig
    const fs::path heroic = root / "heroic";
    const fs::path heroic_gog = root / "Games" / "Heroic" / "The Legend of Heroes Trails of Cold Steel";
    const fs::path heroic_prefix = root / "Games" / "Heroic" / "Prefixes" / "default" / "TLoH CS";
    game_at(heroic_gog);
    fs::create_directories(heroic_prefix / "drive_c", ec);
    Write(heroic / "gog_store" / "installed.json",
          "{\"installed\": [{\"appName\": \"2029703882\", \"platform\": \"windows\", \"install_path\": " + json_str(heroic_gog) + "}]}");
    Write(heroic / "GamesConfig" / "2029703882.json", "{\"2029703882\": {\"winePrefix\": " + json_str(heroic_prefix) + "}}");
    found = FindHeroicGames(heroic);
    Check(found.size() == 1 && same(found[0].dir, heroic_gog) && found[0].source == "Heroic (GOG)"
              && same(found[0].prefix, heroic_prefix) && found[0].prefix_exists,
          "Heroic: GOG install with its Wine prefix");

    // Lutris: games/*.yml
    const fs::path lutris_prefix = root / "lutris_prefix";
    const fs::path lutris_game = lutris_prefix / "drive_c" / "GOG Games" / "Trails of Cold Steel";
    game_at(lutris_game);
    Write(root / "lutris_games" / "trails-1690000000.yml",
          "game:\n  exe: " + U8(lutris_game / "ed8.exe") + "\n  prefix: '" + U8(lutris_prefix) + "'\nwine:\n  version: x\n");
    found = FindLutrisGames(root / "lutris_games");
    Check(found.size() == 1 && same(found[0].dir, lutris_game) && same(found[0].prefix, lutris_prefix) && found[0].source == "Lutris",
          "Lutris: the yml's exe and prefix");

    // a Wine prefix (Bottles): drive_c's usual install folders
    const fs::path bottle = root / "bottles" / "Games";
    const fs::path bottle_game = bottle / "drive_c" / "Program Files (x86)" / "GOG Galaxy" / "Games" / "Trails of Cold Steel";
    game_at(bottle_game);
    found = FindGamesInPrefix(bottle, "Bottles");
    Check(found.size() == 1 && same(found[0].dir, bottle_game) && same(found[0].prefix, bottle) && found[0].source == "Bottles",
          "a Wine prefix: GOG Galaxy's folder inside drive_c");

    // a store's default folder, and merging keeps one entry per folder
    found = FindGamesUnder(root / "GOG Games", "GOG");
    MergeGames(found, FindGamesUnder(root / "GOG Games", "GOG"));
    Check(found.size() == 1 && same(found[0].dir, plain_game), "one entry per folder");

    // a folder picked by hand
    Check(same(ResolveGameDir(U8(plain_game)), plain_game), "a typed folder with ed8.exe");
    Check(same(ResolveGameDir("\"" + U8(plain_game / "ed8.exe") + "\""), plain_game), "a typed ed8.exe path, quoted");
    Check(same(ResolveGameDir(U8(nested)), nested / "Game"), "a folder whose sub-folder has ed8.exe");
    Check(ResolveGameDir(U8(root / "nothing")).empty(), "a folder without ed8.exe is refused");
    const GameInstall picked = DescribeGameDir(heroic_gog, FindHeroicGames(heroic));
    Check(picked.source == "Heroic (GOG)" && !picked.prefix.empty(), "a known folder keeps what its store said");
    Check(DescribeGameDir(plain_game).source.empty(), "an unknown folder has no store");
#ifndef _WIN32
    Check(WinePath(Path("/p"), "C:\\Games\\Trails") == Path("/p/drive_c/Games/Trails"), "a Windows path inside a prefix");
    Check(DescribeGameDir(lutris_game).prefix == lutris_prefix, "a folder inside drive_c: its prefix");
#endif
}

// ---------------------------------------------------------------- a payload and a game folder
const char kLoaderBody[] = "MZ fake loader ... atmt_loader.ini ... v2";
const char kGameDll[] = "MZ the game's own NVIDIA SSAO library";

// manifest.md5 over every file of a component folder.
void Seal(const fs::path& dir) {
    std::vector<std::string> rels;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string rel = Rel(it->path(), dir);
        if (rel != "manifest.md5") rels.push_back(rel);
    }
    std::sort(rels.begin(), rels.end());
    std::string manifest;
    for (const std::string& rel : rels) manifest += Md5File(dir / Path(rel)) + "  " + rel + "\n";
    Write(dir / "manifest.md5", manifest);
}

void MakeComponent(const fs::path& dir, const std::string& name, const std::string& kind, const std::string& version,
                   const std::string& title, const std::vector<std::pair<std::string, std::string>>& files) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    Write(dir / "component.json", "{\"name\": \"" + name + "\", \"kind\": \"" + kind + "\", \"version\": \"" + version
                                      + "\", \"title\": \"" + title + "\", \"min_manager_version\": \"0.0.1\"}");
    for (const auto& f : files) Write(dir / Path(f.first), f.second);
    Seal(dir);
}

// A payload folder: the loader, the data and two mods; only the dialog logger's version and body vary.
void MakePayload(const fs::path& p, const std::string& logger_version, const std::string& logger_body) {
    std::error_code ec;
    fs::remove_all(p, ec);
    MakeComponent(p / "loader", "loader", "loader", "1.0.0", "Loader", {
        {std::string("files/") + kProxyDll, kLoaderBody},
        {"schema.json",
         "{\"schema_version\": 3, \"groups\": [{\"mod\": \"atmt_loader\", \"title\": \"Loader\", \"ini\": \"atmt_loader.ini\", \"settings\": ["
         " {\"type\": \"bool\", \"section\": \"Loader\", \"key\": \"LogEnabled\", \"default\": \"true\"}]}]}"}});
    MakeComponent(p / "data", "data", "data", "1.0.0", "Presets and data", {
        {"supported_exe.txt", Md5Hex("ed8 v1") + "  Steam build\n"},
        {"install_rules.json", "{\"remove\": [\"atmt_stale.dll\", \"atmt_stale_*.txt\", \"atmt_old_cache/\", \"atmt_mods/probe_*\", \"winmm.dll\", \"*.txt\", \"../atmt_up.txt\", \"/atmt_root.txt\", \"old_cache/\"]}"},
        {"presets/everywhere.json",
         "{\"name\": \"Everywhere\", \"recommended_on\": [\"linux\", \"windows\"], \"mods\": {\"deckscreen\": false}, \"icon_pack\": true, \"changes\": ["
         "{\"mod\": \"deckscreen\", \"key\": \"Mode\", \"value\": \"modern\"}]}"},
        {"presets/deck.json",
         "{\"name\": \"Deck\", \"mods\": {\"deckscreen\": true, \"not_installed\": true}, \"changes\": [{\"mod\": \"deckscreen\", \"key\": \"ForceDeckResolution\", "
         "\"value\": true}, {\"mod\": \"atmt_loader\", \"section\": \"Loader\", \"key\": \"LogEnabled\", \"value\": false}]}"},
        {"schema.json", "{\"input\": {\"keys\": [\"F1\", \"F2\"], \"pad_buttons\": [\"A\", \"B\"]}}"}});
    MakeComponent(p / "trails_dialog_logger", "trails_dialog_logger", "mod", logger_version, "Dialog log", {
        {"files/atmt_mods/trails_dialog_logger.dll", logger_body},
        {"files/atmt_mods/trails_dialog_logger.ini", "; template\n[General]\nLogToFile=true\n"},
        {"schema.json",
         "{\"mods\": [{\"mod\": \"trails_dialog_logger\", \"description\": \"Keeps every line of dialog.\"}], \"groups\": ["
         "{\"mod\": \"trails_dialog_logger\", \"title\": \"Dialog log\", \"ini\": \"atmt_mods/trails_dialog_logger.ini\", \"settings\": ["
         " {\"type\": \"bool\", \"section\": \"General\", \"key\": \"LogToFile\", \"default\": \"true\"},"
         " {\"type\": \"int\", \"section\": \"Overlay\", \"key\": \"OverlayLines\", \"default\": \"500\", \"help\": \"lines kept\"}]}]}"}});
    MakeComponent(p / "deckscreen", "deckscreen", "mod", "1.0.0", "Deckscreen", {
        {"files/atmt_mods/deckscreen.dll", "MZ letterbox"},
        {"schema.json",
         "{\"groups\": [{\"mod\": \"deckscreen\", \"title\": \"Letterbox\", \"ini\": \"atmt_mods/deckscreen.ini\", \"settings\": ["
         " {\"type\": \"bool\", \"section\": \"General\", \"key\": \"Enabled\", \"default\": \"true\", \"help\": \"the switch\"},"
         " {\"type\": \"bool\", \"section\": \"General\", \"key\": \"ForceDeckResolution\", \"default\": \"false\"},"
         " {\"type\": \"float\", \"section\": \"General\", \"key\": \"CrispTextWeight\", \"default\": \"1\", \"min\": 1, \"max\": 2},"
         " {\"type\": \"enum\", \"section\": \"General\", \"key\": \"Mode\", \"default\": \"game\", \"choices\": [\"game\", \"off\", \"modern\"]},"
         " {\"type\": \"label\", \"label\": \"x\"}]}]}"}});
}

void MakeGame(const fs::path& g, const std::string& exe) {
    std::error_code ec;
    fs::remove_all(g, ec);
    Write(g / "ed8.exe", exe);
    Write(g / kProxyDll, kGameDll);
    Write(g / "atmt_stale.dll", "stale");             // leftovers the payload's rules name
    Write(g / "atmt_stale_1.txt", "stale");
    Write(g / "atmt_old_cache/x.bin", "stale");
    Write(g / "winmm.dll", "another mod's");          // named by rules too, but not the toolkit's
    Write(g / "old_cache/x.bin", "another mod's");
    Write(g / "keep_me.txt", "the player's");
    Write(g / "mods/order.txt", "other_mod.p3a\n");   // SenPatcher is set up
    Write(g / "atmt_mods/atmt_settings.txt", "x");    // a settings file of the player's
}

void TestInstall(const fs::path& tmp) {
    const fs::path pay = tmp / "payload1";
    const fs::path game = tmp / "game";
    MakePayload(pay, "1.0.0", "MZ logger v1");
    MakeGame(game, "ed8 v1");
    Payload payload;
    std::string err;
    Check(Payload::Load(pay, &payload, &err), "payload loads (" + err + ")");
    Check(payload.VerifyFiles(&err), "payload matches its manifest");
    Check(payload.mods.size() == 2 && payload.FindMod("trails_dialog_logger")->title == "Dialog log"
              && payload.components.size() == 4 && payload.components[0].name == "loader"
              && payload.FindComponent("data")->version == "1.0.0",
          "payload components and mods");
    Check(payload.ExpectedMd5(kProxyDll) == Md5Hex(kLoaderBody) && payload.FindFile("atmt_mods/deckscreen.dll") != nullptr
              && payload.files.size() == 4,
          "the components' files/ are the game's files");
    Check(payload.schema["groups"].size() == 3, "the schema is put together from the components");
    Check(payload.presets.size() == 2 && payload.presets[0].mods.size() == 2 && payload.presets[1].mods.size() == 1
              && payload.presets[1].mods[0].first == "deckscreen" && !payload.presets[1].mods[0].second,
          "a preset's mod switches load");
    Check(payload.FindMod("trails_dialog_logger")->description == "Keeps every line of dialog."
              && payload.FindMod("deckscreen")->description.empty(),
          "a mod's description comes from its schema.json \"mods\" (absent there: none)");
    Check(payload.icon_pack.empty() && payload.remove_rules.size() == 9, "payload rules (and no icon pack)");
    Check(Schema::FromJson(payload.schema).input_keys.size() == 2 && Schema::FromJson(payload.schema).input_pad_buttons.size() == 2,
          "the schema's input names");

    GameStatus s = Inspect(game, &payload);
    Check(s.verdict == ExeVerdict::Supported && !s.installed, "status before: supported build, not installed");
    Check(s.SuggestedAction(&payload) == GameStatus::Action::Install, "suggests Install");

    InstallOptions o;
    o.tag = "t1";
    InstallResult r = Install(game, payload, o);
    Check(r.ok, "install (" + r.error + ")");
    Check(Read(game / kProxyDll) == kLoaderBody && Read(game / kOrigDll) == kGameDll, "loader in place, the game's dll kept as .orig");
    Check(Exists(game / "atmt_mods/trails_dialog_logger.dll") && Exists(game / "atmt_mods/deckscreen.dll"), "mods installed");
    Check(!Exists(game / "atmt_mods/disabled"), "nothing parked: every payload mod is on by default");
    Check(Exists(game / "atmt_mods/trails_dialog_logger.ini"), "a missing ini gets the template");
    Check(Read(game / "atmt_mods/trails_dialog_logger.ini").find("OverlayLines=500") != std::string::npos
              && Read(game / "atmt_mods/trails_dialog_logger.ini").find("; lines kept") != std::string::npos,
          "a key new in this version is added with its help");
    Check(!Exists(game / "atmt_stale.dll") && !Exists(game / "atmt_stale_1.txt") && !Exists(game / "atmt_old_cache")
              && Exists(game / "keep_me.txt"),
          "the payload's install rules remove leftovers, and only those");
    Check(Exists(game / "winmm.dll") && Exists(game / "old_cache/x.bin") && Exists(game / "ed8.exe"),
          "a rule that names anything but the toolkit's own files (atmt_*) is refused");
    Check(Read(game / "mods/order.txt") == "other_mod.p3a\n", "no icon pack in this payload: mods/order.txt untouched");
    Check(Exists(game / "atmt_mods/atmt_settings.txt"), "the player's own files in atmt_mods are kept");
    Check(Exists(game / "atmt_backup_t1/backup.json") && Read(game / "atmt_backup_t1" / kProxyDll) == kGameDll,
          "backup taken first");
    Json rec = ReadInstallRecord(game);
    Check(rec["components"].Str("trails_dialog_logger") == "1.0.0" && rec["components"].Str("loader") == "1.0.0"
              && rec["exe_verified"].AsBool(),
          "atmt_install.json written, with every component's version");

    s = Inspect(game, &payload);
    Check(s.installed && s.problems.empty() && s.SuggestedAction(&payload) == GameStatus::Action::Reinstall,
          "status after: installed, nothing to repair");

    // the player's ini survives a reinstall
    Write(game / "atmt_mods/trails_dialog_logger.ini", "[General]\nLogToFile=false ; mine\n");
    // Steam's file check puts the game's dll back over the loader
    Write(game / kProxyDll, kGameDll);
    s = Inspect(game, &payload);
    Check(!s.installed && !s.problems.empty() && s.SuggestedAction(&payload) == GameStatus::Action::Repair, "Repair detected");
    o.tag = "t2";
    r = Install(game, payload, o);
    Check(r.ok && Read(game / kProxyDll) == kLoaderBody, "repair puts the loader back");
    Check(Read(game / "atmt_mods/trails_dialog_logger.ini").find("LogToFile=false ; mine") != std::string::npos,
          "an existing ini is kept");

    // toggles
    Check(SetModEnabled(game, "deckscreen", false, &err) && Exists(game / "atmt_mods/disabled/deckscreen.dll")
              && !Exists(game / "atmt_mods/deckscreen.dll"),
          "disable moves the dll to atmt_mods/disabled");
    // an update keeps that choice
    const fs::path pay2 = tmp / "payload2";
    MakePayload(pay2, "1.1.0", "MZ logger v2");
    Payload payload2;
    Payload::Load(pay2, &payload2, &err);
    s = Inspect(game, &payload2);
    Check(s.SuggestedAction(&payload2) == GameStatus::Action::Update && s.Outdated(payload2).size() == 1
              && s.Outdated(payload2)[0] == "Dialog log 1.0.0 -> 1.1.0",
          "suggests Update for one newer component");
    o.tag = "t3";
    r = Install(game, payload2, o);
    Check(r.ok && Read(game / "atmt_mods/trails_dialog_logger.dll") == "MZ logger v2", "update installs the new files");
    Check(Exists(game / "atmt_mods/disabled/deckscreen.dll") && !Exists(game / "atmt_mods/deckscreen.dll"),
          "a disabled mod stays disabled across an update");

    // a damaged file after writing: everything is rolled back
    const fs::path pay3 = tmp / "payload3";
    MakePayload(pay3, "1.2.0", "MZ logger v3");
    Payload payload3;
    Payload::Load(pay3, &payload3, &err);
    o.tag = "t4";
    o.before_verify = [&]() { Write(game / "atmt_mods/trails_dialog_logger.dll", "corrupted"); };
    r = Install(game, payload3, o);
    o.before_verify = nullptr;
    Check(!r.ok && r.rolled_back, "a verification failure rolls back");
    Check(Read(game / "atmt_mods/trails_dialog_logger.dll") == "MZ logger v2"
              && ReadInstallRecord(game)["components"].Str("trails_dialog_logger") == "1.1.0",
          "the rollback restores the previous version");

    // backups
    // (each successful install keeps the newest kKeepBackups; the failed one's backup stays as well)
    const std::vector<BackupInfo> backups = ListBackups(game);
    Check(backups.size() == 3 && backups[0].name == "atmt_backup_t4" && backups[1].name == "atmt_backup_t3"
              && backups[2].name == "atmt_backup_t2" && !Exists(game / "atmt_backup_t1"),
          "the newest backups are kept, newest first");
    r = RestoreBackup(game, "atmt_backup_t2");
    Check(r.ok && Read(game / kProxyDll) == kGameDll && Read(game / "atmt_mods/trails_dialog_logger.dll") == "MZ logger v1"
              && Exists(game / "atmt_mods/atmt_settings.txt") && Exists(game / kInstallRecord),
          "restoring a backup gives that state back (" + r.error + ")");
    Check(ListBackups(game).size() == kKeepBackups && ListBackups(game)[0].reason == "restore",
          "a restore keeps the newest backups too, its own among them");
    o.tag = "t5";
    r = Install(game, payload2, o);
    Check(r.ok, "install again (" + r.error + ")");

    // the game's library cannot be put back (here: .orig is a folder): .orig stays, so the loader
    // still has something to forward to
    std::error_code fec;
    fs::rename(game / kOrigDll, game / "orig.keep", fec);
    fs::create_directories(game / kOrigDll / "x", fec);
    UninstallOptions failing;
    failing.tag = "uf";
    r = Uninstall(game, failing);
    Check(!r.ok && IsDir(game / kOrigDll) && Read(game / kProxyDll) == kLoaderBody,
          "an uninstall that cannot restore the game's library keeps .orig");
    fs::remove_all(game / kOrigDll, fec);
    fs::rename(game / "orig.keep", game / kOrigDll, fec);
    o.tag = "t6";
    r = Install(game, payload2, o);
    Check(r.ok, "install again (" + r.error + ")");

    // uninstall
    UninstallOptions uo;
    uo.tag = "u1";
    r = Uninstall(game, uo);
    Check(r.ok && Read(game / kProxyDll) == kGameDll && !Exists(game / kOrigDll) && !Exists(game / "atmt_mods")
              && !Exists(game / kInstallRecord),
          "uninstall puts the game's own files back");
    Check(Exists(game / "atmt_backup_u1/atmt_mods/trails_dialog_logger.ini"), "uninstall backs the settings up");
    Check(Read(game / "mods/order.txt") == "other_mod.p3a\n", "uninstall leaves the other SenPatcher mods alone");

    // install everything: every mod on but what the recommended preset switches off, its settings applied
    r = InstallEverything(game, payload, InstallOptions());
    Check(r.ok && Exists(game / "atmt_mods/trails_dialog_logger.dll") && Exists(game / "atmt_mods/disabled/deckscreen.dll")
              && !Exists(game / "atmt_mods/deckscreen.dll"),
          "install everything: every mod on, the one the recommended preset switches off parked (" + r.error + ")");
    // a preset's mod switches: deck switches deckscreen back on; a mod that is not installed is left alone
    const Preset& deck_preset = payload.presets[0];
    Check(PresetModChanges(game, deck_preset).size() == 1 && PresetModChanges(game, deck_preset)[0].first == "deckscreen",
          "a preset's mod switch shows as a change");
    std::vector<std::string> switched;
    Check(ApplyPresetMods(game, deck_preset, &switched, &err) && switched.size() == 1 && Exists(game / "atmt_mods/deckscreen.dll")
              && !Exists(game / "atmt_mods/disabled/deckscreen.dll") && PresetModChanges(game, deck_preset).empty(),
          "applying a preset switches the mod on (" + err + ")");
    Check(ApplyPresetMods(game, payload.presets[1], nullptr, &err) && Exists(game / "atmt_mods/disabled/deckscreen.dll"),
          "and another switches it off again");
    Check(Read(game / "atmt_mods/deckscreen.ini").find("Mode=modern") != std::string::npos,
          "install everything applies the preset recommended for " + PlatformName());

    // remove everything: nothing of ours is left, the player's own file is
    Write(game / "atmt_dialogs.jsonl", "{}");
    Write(game / "atmt_loader.log", "x");
    UninstallOptions purge;
    purge.purge = true;
    r = Uninstall(game, purge);
    bool any = false;
    std::error_code ec;
    for (fs::directory_iterator it(game, ec), end; !ec && it != end; it.increment(ec)) {
        any = any || StartsWith(U8(it->path().filename()), "atmt_");
    }
    Check(r.ok && !any && Read(game / kProxyDll) == kGameDll && !Exists(game / kOrigDll) && Exists(game / "keep_me.txt"),
          "remove everything leaves only the game's own files (" + r.error + ")");

    // hard stops and the unverified build
    MakeGame(game, "ed8 v2 (a game patch)");
    r = Install(game, payload2, InstallOptions());
    Check(!r.ok && r.needs_ack, "an unverified build needs an acknowledgement");
    InstallOptions ack;
    ack.acknowledge_unverified = true;
    r = Install(game, payload2, ack);
    Check(r.ok && ReadInstallRecord(game).Str("unverified_ack_md5") == Md5Hex("ed8 v2 (a game patch)"), "install anyway, ack stored (" + r.error + ")");
    r = Install(game, payload2, InstallOptions());
    Check(r.ok, "the ack is not asked again for the same exe");
    RemoveFile(game / kOrigDll);
    s = Inspect(game, &payload2);
    Check(!s.hard_stops.empty(), "our loader without .orig is a hard stop");
    RemoveFile(game / "ed8.exe");
    r = Install(game, payload2, ack);
    Check(!r.ok && !r.rolled_back, "no ed8.exe is a hard stop");
}

// SenPatcher's DINPUT8.dll is recognised (without an order.txt too); a pre-1.0 SenPatcher's on-disk
// backups are recognised and explained. (The icon pack's SenPatcher cases: TestIconPackInstall.)
void TestSenPatcher(const fs::path& tmp) {
    const fs::path pay = tmp / "payload_sp";
    MakePayload(pay, "1.0.0", "MZ logger v1");
    Payload payload;
    std::string err;
    Check(Payload::Load(pay, &payload, &err), "payload loads (" + err + ")");

    const fs::path vanilla = tmp / "game_vanilla";
    MakeGame(vanilla, "ed8 v1");
    std::error_code ec;
    fs::remove_all(vanilla / "mods", ec);
    Check(!Inspect(vanilla, &payload).senpatcher, "vanilla: no SenPatcher");
    InstallOptions o;
    InstallResult r;

    const fs::path senp = tmp / "game_senpatcher";
    MakeGame(senp, "ed8 v1");
    fs::remove_all(senp / "mods", ec);
    Write(senp / "DINPUT8.dll", std::string("MZ\0\0 SenPatcherHook\0Sen1\0", 26));
    Check(Inspect(senp, &payload).senpatcher && Inspect(senp, &payload).senpatcher_mods,
          "SenPatcher's DINPUT8.dll is recognised");
    Check(!Inspect(vanilla, &payload).senpatcher_mods && SenPatcherSetUp(senp) && !SenPatcherSetUp(vanilla),
          "SenPatcher's mods/ loader: set up / not set up");

    const fs::path old = tmp / "game_old_senpatcher";
    MakeGame(old, "ed8 patched on disk");
    Write(old / "ed8.exe.senpatcher.bkp", "ed8 v1");
    const GameStatus s = Inspect(old, &payload);
    Check(s.old_senpatcher && s.verdict == ExeVerdict::Unverified, "a pre-1.0 SenPatcher's backup is recognised");
    o.tag = "sp3";
    r = Install(old, payload, o);
    Check(!r.ok && r.needs_ack && r.error.find("SenPatcher older than v1.0") != std::string::npos,
          "the unverified-build question says how to restore the exe");
}


// ---------------------------------------------------------------- SenPatcher through Proton
// Answers from a table: url -> (status, body); every request is counted.
class FakeHttp : public Http {
public:
    std::map<std::string, std::pair<int, std::string>> answers;
    std::vector<std::string> asked;
    bool Get(const std::string& url, const std::vector<std::pair<std::string, std::string>>&, HttpResponse* out,
             const ProgressFn&) override {
        asked.push_back(url);
        auto it = answers.find(url);
        if (it == answers.end()) {
            out->status = 0;
            out->error = "offline";
            return false;
        }
        out->status = it->second.first;
        out->body = it->second.second;
        return true;
    }
};

void MakeExecutable(const fs::path& p, const std::string& text) {
    Write(p, text);
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, ec);
}

void TestSenPatcherProton(const fs::path& tmp) {
    // the zip reader
    const std::string zip(reinterpret_cast<const char*>(kZip), sizeof(kZip));
    std::vector<ZipEntry> entries;
    std::string err;
    Check(ListZip(zip, &entries, &err) && entries.size() == 6, "zip: the central directory lists 6 entries (" + err + ")");
    std::string exe;
    bool read = false;
    for (const ZipEntry& e : entries) {
        if (e.name == "SenPatcher-v9.9.0/SenPatcher.exe") read = ReadZipEntry(zip, e, &exe, &err) && e.method == 8;
    }
    Check(read && Md5Hex(exe) == "60771fd62b338790f8067d8fa6a4fa6c", "zip: a deflated entry reads back exactly");
    std::string bad = zip;
    bool crc = false;
    for (const ZipEntry& e : entries) {
        if (e.name != "SenPatcher-v9.9.0/LICENSE.txt") continue;
        bad[static_cast<size_t>(e.local_offset) + 30 + e.name.size()] ^= 1;   // the stored data's first byte
        crc = !ReadZipEntry(bad, e, &exe, &err) && err.find("CRC") != std::string::npos;
    }
    Check(crc, "zip: a damaged entry fails its CRC");
    Check(!ListZip("not a zip at all, just text that is long enough", &entries, &err), "zip: not a zip is refused");

    // GitHub's latest release
    const std::string sha = Sha256Hex(zip);
    auto release_json = [&](const std::string& tag, const std::string& digest) {
        return "{\"tag_name\": \"" + tag + "\", \"html_url\": \"https://github.com/x/releases/tag/" + tag
               + "\", \"assets\": [{\"name\": \"notes.txt\", \"browser_download_url\": \"https://example.invalid/notes\"},"
                 " {\"name\": \"SenPatcher-" + tag + ".zip\", \"size\": " + std::to_string(zip.size())
               + ", \"digest\": \"" + digest + "\", \"browser_download_url\": \"https://example.invalid/" + tag + ".zip\"}]}";
    };
    Json j;
    SenPatcherRelease rel;
    err.clear();
    Check(Json::Parse(release_json("v9.9.0", "sha256:" + sha), &j) && ParseSenPatcherRelease(j, &rel, &err)
              && rel.tag == "v9.9.0" && rel.asset == "SenPatcher-v9.9.0.zip" && rel.sha256 == sha && rel.size == zip.size(),
          "the latest release: its tag, its zip and the zip's sha256 (" + err + ")");
    Check(Json::Parse("{\"tag_name\": \"v1\", \"assets\": [{\"name\": \"x.7z\", \"browser_download_url\": \"https://a/b\"}]}", &j)
              && !ParseSenPatcherRelease(j, &rel, &err),
          "a release without a SenPatcher zip is refused");
    Check(Json::Parse(release_json("../../x", "sha256:" + sha), &j) && !ParseSenPatcherRelease(j, &rel, &err),
          "a tag that is not a plain folder name is refused");

    // the copy here
    Json::Parse(release_json("v9.9.0", "sha256:" + sha), &j);
    ParseSenPatcherRelease(j, &rel);
    SenPatcherCopy copy;
    SenPatcherRelease wrong = rel;
    wrong.sha256 = std::string(64, '0');
    Check(!InstallSenPatcherZip(wrong, zip, &copy, &err) && err.find("sha256") != std::string::npos && !LocalSenPatcher().ok(),
          "a zip that does not match GitHub's sha256 is refused");
    err.clear();
    Check(InstallSenPatcherZip(rel, zip, &copy, &err) && LocalSenPatcher().tag == "v9.9.0", "the zip is unpacked (" + err + ")");
    Check(Exists(copy.exe) && Exists(copy.dir / "Trails of Cold Steel" / "DINPUT8.dll")
              && Exists(copy.dir / "Trails of Cold Steel" / "senpatcher_settings.ini") && Exists(copy.dir / "LICENSE.txt")
              && !Exists(copy.dir / "Trails of Cold Steel II"),
          "only SenPatcher.exe, its license and the CS1 folder are kept");
    SenPatcherRelease newer = rel;
    newer.tag = "v10.0.0";
    Check(InstallSenPatcherZip(newer, zip, &copy, &err) && LocalSenPatcher().tag == "v10.0.0"
              && !Exists(SenPatcherRoot() / "v9.9.0"),
          "a newer release replaces the older copy");

    FakeHttp http;
    const std::string api = "https://api.github.com/repos/AdmiralCurtiss/SenPatcher/releases/latest";
    http.answers[api] = {200, release_json("v10.0.0", "sha256:" + sha)};
    std::string note;
    Check(EnsureSenPatcher(&http, &copy, &note, &err) && copy.tag == "v10.0.0" && http.asked.size() == 1,
          "the latest release is the copy here: nothing is downloaded");
    http.answers[api] = {200, release_json("v10.1.0", "sha256:" + sha)};
    http.answers["https://example.invalid/v10.1.0.zip"] = {200, zip};
    http.asked.clear();
    err.clear();
    Check(EnsureSenPatcher(&http, &copy, &note, &err) && copy.tag == "v10.1.0" && http.asked.size() == 2,
          "a newer release is downloaded (" + err + ")");
    http.answers.clear();
    note.clear();
    Check(EnsureSenPatcher(&http, &copy, &note, &err) && copy.tag == "v10.1.0" && note.find("from before") != std::string::npos,
          "offline: the copy here is used, with a note");

    // Proton: a sandbox Steam with Valve's Protons, the runtime and a custom one
    Check(OfficialProtonName("Proton 9.0 (Beta)") == "proton_9" && OfficialProtonName("Proton 6.3") == "proton_63"
              && OfficialProtonName("Proton 10.0") == "proton_10" && OfficialProtonName("Proton - Experimental") == "proton_experimental"
              && OfficialProtonName("Proton Hotfix") == "proton_hotfix" && OfficialProtonName("Proton EasyAntiCheat Runtime").empty()
              && OfficialProtonName("Steam Linux Runtime 3.0 (sniper)").empty(),
          "Valve's Proton folders -> the names Steam's settings use");
    const fs::path home = tmp / "sp_home";
    const fs::path root = home / ".local" / "share" / "Steam";
    const fs::path common = root / "steamapps" / "common";
    const fs::path game = common / "Trails of Cold Steel";
    Write(game / "ed8.exe", "ed8 v1");
    Write(root / "steamapps" / "appmanifest_538680.acf", "\"AppState\" { \"appid\" \"538680\" \"installdir\" \"Trails of Cold Steel\" }");
    // proton: records its arguments and environment; reg.exe makes the prefix with the override,
    // SenPatcher.exe installs the CS1 dll the way SenPatcher does
    const std::string proton =
        "#!/bin/sh\n"
        "printf '%s %s\\n' \"$(basename \"$(dirname \"$0\")\")\" \"$*\" >> \"$STEAM_COMPAT_DATA_PATH/calls.txt\"\n"
        "echo \"app=$STEAM_COMPAT_APP_ID tools=$STEAM_COMPAT_TOOL_PATHS install=$STEAM_COMPAT_INSTALL_PATH\" >> \"$STEAM_COMPAT_DATA_PATH/calls.txt\"\n"
        "case \"$2\" in\n"
        "  *reg.exe) mkdir -p \"$STEAM_COMPAT_DATA_PATH/pfx\"; printf 'WINE REGISTRY Version 2\\n\\n[Software\\\\\\\\Wine\\\\\\\\DllOverrides] 1700000000\\n#time=1\\n\"%s\"=\"%s\"\\n' \"$6\" \"${10}\" > \"$STEAM_COMPAT_DATA_PATH/pfx/user.reg\" ;;\n"
        "  *SenPatcher.exe) cp \"$(dirname \"$2\")/Trails of Cold Steel/DINPUT8.dll\" \"$STEAM_COMPAT_INSTALL_PATH/DINPUT8.dll\" ;;\n"
        "esac\n";
    const std::string manifest = "\"manifest\" { \"commandline\" \"/proton %verb%\" \"require_tool_appid\" \"1628350\" }";
    for (const char* folder : {"Proton 8.0", "Proton 9.0 (Beta)", "Proton - Experimental"}) {
        MakeExecutable(common / folder / "proton", proton);
        Write(common / folder / "toolmanifest.vdf", manifest);
    }
    MakeExecutable(common / "SteamLinuxRuntime_sniper" / "_v2-entry-point",
                   "#!/bin/sh\nwhile [ \"$1\" != \"--\" ]; do shift; done\nshift\nexec \"$@\"\n");
    Write(common / "SteamLinuxRuntime_sniper" / "toolmanifest.vdf", "\"manifest\" { \"commandline\" \"/_v2-entry-point --verb=%verb% --\" }");
    Write(root / "steamapps" / "appmanifest_1628350.acf", "\"AppState\" { \"appid\" \"1628350\" \"installdir\" \"SteamLinuxRuntime_sniper\" }");
    const fs::path ge = root / "compatibilitytools.d" / "GE-Proton9-1";
    MakeExecutable(ge / "proton", proton);
    Write(ge / "toolmanifest.vdf", "\"manifest\" { \"commandline\" \"/proton %verb%\" }");
    Write(ge / "compatibilitytool.vdf",
          "\"compatibilitytools\" { \"compat_tools\" { \"GE-Proton9-1\" { \"install_path\" \".\" \"display_name\" \"GE-Proton9-1\" } } }");

    const std::vector<fs::path> roots = {root};
    const std::vector<fs::path> libs = SteamLibraries(roots);
    Check(FindProtonTools(roots, libs).size() == 4, "the Protons: three of Valve's and a custom one");
    const GameInstall g = FindGames(libs).empty() ? GameInstall() : FindGames(libs)[0];
    ProtonLaunch l;
    err.clear();
    Check(PlanProtonLaunch(g, roots, libs, &l, &err) && l.tool.name == "proton_9" && l.why.find("newest") != std::string::npos,
          "no setting: the newest of Valve's numbered Protons (" + err + ")");
    Check(l.command.size() == 5 && Path(l.command[0]) == common / "SteamLinuxRuntime_sniper" / "_v2-entry-point"
              && l.command[1] == "--verb=waitforexitandrun" && l.command[2] == "--"
              && Path(l.command[3]) == common / "Proton 9.0 (Beta)" / "proton" && l.command[4] == "waitforexitandrun",
          "Proton runs inside the Steam Linux Runtime it asks for, as Steam starts it");
    const std::string config_vdf =
        "\"InstallConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"CompatToolMapping\" {\n"
        "  \"0\" { \"name\" \"proton_8\" \"config\" \"\" \"priority\" \"75\" }\n%s} } } } }";
    char buf[512];
    std::snprintf(buf, sizeof(buf), config_vdf.c_str(), "");
    Write(root / "config" / "config.vdf", buf);
    Check(CompatToolSetting(root, "0") == "proton_8" && PlanProtonLaunch(g, roots, libs, &l, &err) && l.tool.name == "proton_8"
              && l.why.find("default") != std::string::npos,
          "Steam's default compatibility tool");
    std::snprintf(buf, sizeof(buf), config_vdf.c_str(), "  \"538680\" { \"name\" \"GE-Proton9-1\" \"config\" \"\" \"priority\" \"250\" }\n");
    Write(root / "config" / "config.vdf", buf);
    Check(PlanProtonLaunch(g, roots, libs, &l, &err) && l.tool.name == "GE-Proton9-1" && l.command.size() == 2
              && l.why.find("game's") != std::string::npos,
          "the game's own setting: a custom Proton, which asks for no runtime");
    std::snprintf(buf, sizeof(buf), config_vdf.c_str(), "  \"538680\" { \"name\" \"proton_7\" }\n");
    Write(root / "config" / "config.vdf", buf);
    Check(PlanProtonLaunch(g, roots, libs, &l, &err) && l.tool.name == "proton_9" && l.why.find("proton_7") != std::string::npos,
          "a setting whose Proton is not installed: the newest, saying so");

    // the dll override: user.reg and launch options
    const fs::path pfx = tmp / "sp_pfx";
    Write(pfx / "user.reg", "WINE REGISTRY Version 2\n\n[Software\\\\Wine\\\\Debug] 1\n\"dinput8\"=\"builtin\"\n\n"
                            "[Software\\\\Wine\\\\DllOverrides] 1700000000\n#time=1d\n\"*d3d11\"=\"native\"\n\"*DINPUT8\"=\"n,b\"\n");
    Check(PrefixDllOverride(pfx, "dinput8") == "n,b" && PrefixDllOverride(pfx, "d3d11") == "native"
              && PrefixDllOverride(pfx, "dsound").empty() && OverrideLoadsNative("native,builtin") && !OverrideLoadsNative("builtin"),
          "user.reg: the DllOverrides section only, any case, with or without *");
    Check(LaunchOptionLoadsNative("WINEDLLOVERRIDES=DINPUT8=n,b %command%", "dinput8")
              && LaunchOptionLoadsNative("WINEDLLOVERRIDES=\"d3d9=n;dinput8,dsound=n,b\" %command%", "dinput8")
              && !LaunchOptionLoadsNative("WINEDLLOVERRIDES=dinput8=b %command%", "dinput8")
              && !LaunchOptionLoadsNative("PROTON_LOG=1 %command%", "dinput8"),
          "launch options: WINEDLLOVERRIDES with dinput8 native first");
    Write(root / "userdata" / "123" / "config" / "localconfig.vdf",
          "\"UserLocalConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"apps\" { \"538680\" { \"LaunchOptions\" \"WINEDLLOVERRIDES=dinput8=n,b %command%\" } } } } } }");
    const std::vector<std::string> opts = SteamLaunchOptions(roots, "538680");
    Check(opts.size() == 1 && LaunchOptionLoadsNative(opts[0], "dinput8"), "the game's launch options are read from localconfig.vdf");
    fs::remove_all(root / "userdata");

#ifndef _WIN32
    // the whole run against the sandbox Steam: HOME points at it, the latest release is the copy here
    const std::string old_home = GetEnv("HOME");
    SetEnv("HOME", U8(home));
    fs::remove(root / "config" / "config.vdf");
    http.answers[api] = {200, release_json("v10.1.0", "sha256:" + sha)};
    const SenPatcherStatus before = GetSenPatcherStatus(DescribeGameDir(game, FindGames()));
    Check(before.can_run && !before.dll && !before.override_set, "status before: can run, nothing installed, no override");
    const SenPatcherRun run = RunSenPatcher(game, &http);
    const std::string calls = Read(root / "steamapps" / "compatdata" / "538680" / "calls.txt");
    Check(run.ok && run.installed && run.tag == "v10.1.0", "SenPatcher ran through Proton and installed its dll (" + run.error + ")");
    Check(calls.find("Proton 9.0 (Beta) waitforexitandrun c:\\windows\\system32\\reg.exe add HKCU\\Software\\Wine\\DllOverrides /v dinput8 /t REG_SZ /d native,builtin /f")
                  != std::string::npos
              && calls.find("SenPatcher.exe") != std::string::npos && calls.find("app=538680") != std::string::npos
              && calls.find("install=" + U8(game)) != std::string::npos
              && calls.find("SteamLinuxRuntime_sniper") != std::string::npos,
          "first the override through reg.exe, then SenPatcher.exe, inside the runtime, with Steam's environment");
    const fs::path gui_ini_path = root / "steamapps" / "compatdata" / "538680" / "pfx" / "drive_c" / "users" / "steamuser"
                                  / "AppData" / "Local" / "SenPatcherGui" / "gui.ini";
    const std::string gui_ini = Read(gui_ini_path);
    Check(gui_ini.find("Sen1Path=Z:" + std::string(1, '\\')) != std::string::npos && gui_ini.find("Trails of Cold Steel") != std::string::npos,
          "SenPatcher's file picker starts at the game (its gui.ini, a Wine path)");
    const SenPatcherStatus after = GetSenPatcherStatus(DescribeGameDir(game, FindGames()));
    Check(after.dll && after.override_set && after.override_where == "prefix" && after.Text().find("override set") != std::string::npos,
          "status after: installed, and the override is in the prefix");
    Write(gui_ini_path, "[GamePaths]\nSen1Path=D:\\Games\\CS1\n");
    const SenPatcherRun again = RunSenPatcher(game, &http);
    Check(again.ok && Read(gui_ini_path).find("D:\\Games\\CS1") != std::string::npos,
          "a game folder SenPatcher already remembers is left alone");
    const SenPatcherRun only = RunSenPatcher(game, &http, true);
    Check(only.ok && only.tag.empty(), "override only: no download, no window (" + only.error + ")");
    const fs::path other = tmp / "sp_other_game";
    MakeGame(other, "ed8 v1");
    const SenPatcherRun not_steam = RunSenPatcher(other, &http);
    Check(!not_steam.ok && not_steam.error.find("Steam version") != std::string::npos, "a game outside Steam is refused, saying why");
    SetEnv("HOME", old_home);
#else
    // Windows: SenPatcher.exe runs as it is (RunAndWait), for any store's game; no override
    const fs::path run_log = tmp / "sp_run.log";
    int code = -1;
    err.clear();
    // (a batch file: cmd /c keeps the quotes around a command with & in it)
    Write(tmp / "sp_run.cmd", "@echo [%ATMT_TEST%]\r\n@cd\r\n@exit /b 3\r\n");
    Check(RunAndWait({GetEnv("COMSPEC"), "/c", U8(tmp / "sp_run.cmd")}, {{"ATMT_TEST", "a b"}}, tmp, run_log, &code, &err)
              && code == 3,
          "RunAndWait: waits for the program and returns its exit code (" + std::to_string(code) + " " + err + ")");
    const std::string out = Read(run_log);
    Check(out.find("[a b]") != std::string::npos && Lower(out).find(Lower(U8(fs::path(tmp).make_preferred()))) != std::string::npos,
          "RunAndWait: the environment, the folder and the output in the log");
    Check(!RunAndWait({U8(tmp / "no_such.exe")}, {}, tmp, fs::path(), &code, &err), "RunAndWait: a missing program fails");
    const fs::path win_game = tmp / "sp_win_game";
    MakeGame(win_game, "ed8 v1");
    const SenPatcherStatus ws = GetSenPatcherStatus(DescribeGameDir(win_game, FindGames()));
    Check(ws.can_run && !ws.needs_override && ws.cannot_run.empty(), "Windows: Run SenPatcher for any game, no override");
    const SenPatcherRun wo = RunSenPatcher(win_game, &http, true);
    Check(!wo.ok && wo.error.find("Windows") != std::string::npos, "Windows: no dll override to set");
#endif
}

// ---------------------------------------------------------------- the icon pack
// Falcom type 1: a hand-made stream with back references and escapes, and round trips.
void TestFalcom() {
    // backref byte 0x10: "abc", copy 3 back x6, an escaped 0x10, offset byte 0x15 (= 0x14 back) x2
    std::string data = "abc";
    std::string stream = "abc";
    stream += std::string("\x10\x03\x06", 3);
    data += "abcabc";
    stream += std::string("\x10\x10", 2);
    data += '\x10';
    for (int i = 0; i < 0x14 - 10; ++i) {
        stream += static_cast<char>('A' + i);
        data += static_cast<char>('A' + i);
    }
    stream += std::string("\x10\x15\x02", 3);
    data += data.substr(data.size() - 0x14, 2);
    std::string blob;
    const uint32_t head[3] = {static_cast<uint32_t>(data.size()), static_cast<uint32_t>(12 + stream.size()), 0x10};
    blob.assign(reinterpret_cast<const char*>(head), 12);
    blob += stream;
    std::string out, err;
    Check(FalcomDecompress(blob, static_cast<uint32_t>(data.size()), &out, &err) && out == data,
          "Falcom type 1: back references, escapes, offsets above the backref byte (" + err + ")");
    Check(!FalcomDecompress(blob, static_cast<uint32_t>(data.size()) + 1, &out, &err), "a wrong size is an error");

    std::string bytes;
    for (int i = 0; i < 70000; ++i) bytes += static_cast<char>((i * 7 + i / 300) % 256);
    for (const std::string& d : {std::string(), std::string("x"), bytes, std::string(5000, '\0')}) {
        std::string back;
        const std::string packed = FalcomCompress(d);
        Check(FalcomDecompress(packed, static_cast<uint32_t>(d.size()), &back, &err) && back == d,
              "Falcom compress/decompress round trip of " + std::to_string(d.size()) + " bytes");
    }
}

std::string MakePkgBytes(const std::vector<std::pair<std::string, std::string>>& files, const std::string& crc_name) {
    Pkg pkg;
    pkg.unknown = 0x1234;
    for (const auto& f : files) {
        Pkg::Entry e;
        e.name = f.first;
        e.name.resize(0x40, '\0');
        e.size = static_cast<uint32_t>(f.second.size());
        if (f.first == crc_name) {
            e.flags = 3;   // a crc32 prefix before the type-1 stream
            e.stored = "CRC!" + FalcomCompress(f.second);
        } else if (f.first.find(".raw") != std::string::npos) {
            e.flags = 0;
            e.stored = f.second;
        } else {
            e.flags = 1;
            e.stored = FalcomCompress(f.second);
        }
        pkg.entries.push_back(e);
    }
    return pkg.Build();
}

void TestPkg() {
    const std::string raw = MakePkgBytes({{"a.png.phyre", std::string(3000, 'a') + "tail"}, {"b.raw", "raw bytes"},
                                          {"c.bin", "with a crc"}},
                                         "c.bin");
    Pkg pkg;
    std::string err, out;
    Check(pkg.Parse(raw, &err) && pkg.entries.size() == 3 && pkg.unknown == 0x1234, "pkg parses (" + err + ")");
    Check(pkg.Names() == std::vector<std::string>({"a.png.phyre", "b.raw", "c.bin"}), "pkg names");
    Check(pkg.Read("a.png.phyre", &out) && out == std::string(3000, 'a') + "tail", "pkg reads a type-1 entry");
    Check(pkg.Read("b.raw", &out) && out == "raw bytes", "pkg reads a stored entry");
    Check(pkg.Read("c.bin", &out) && out == "with a crc", "pkg reads an entry with a crc prefix");
    Check(!pkg.Read("nope", &out), "a missing entry is an error");
    Check(pkg.Build() == raw, "pkg build gives the same bytes back");
    Check(pkg.Replace("b.raw", "new data") && !pkg.Replace("nope", "x"), "replace");
    Pkg again;
    Check(again.Parse(pkg.Build(), &err) && again.Read("b.raw", &out) && out == "new data" && again.entries[1].flags == 1
              && again.Read("a.png.phyre", &out) && out == std::string(3000, 'a') + "tail" && again.Read("c.bin", &out)
              && out == "with a crc",
          "pkg replace + build round trip");
    Pkg bad;
    Check(!bad.Parse(std::string("\0\0\0\0\xff\0\0\0", 8), &err), "a damaged pkg is refused");
}

void TestP3a(const fs::path& tmp) {
    Check(Xxh64("", 0) == 0xEF46DB3751D8E999ull, "xxh64 of nothing");
    std::string out;
    // "abcd", then 8 bytes from 4 back, then the literal "x"
    const std::string lz4 = std::string("\x44" "abcd" "\x04\x00" "\x10" "x", 9);
    Check(Lz4BlockDecompress(lz4, 13, &out) && out == "abcdabcdabcdx", "lz4 block");
    Check(!Lz4BlockDecompress(lz4, 12, &out), "lz4 with a wrong size is an error");
    std::string big;
    for (int i = 0; i < 100000; ++i) big += static_cast<char>((i / 7) % 251);
    Check(ZstdDecompress(ZstdCompress(big, 3), big.size(), &out) && out == big, "zstd round trip");

    for (int level : {0, 3}) {
        const fs::path file = tmp / ("test_" + std::to_string(level) + ".p3a");
        std::string err;
        Check(WriteP3a(file, {{"data/asset/D3D11/A.pkg", big}, {"data/x.bin", "small"}, {"empty", ""}}, level, &err),
              "p3a written (zstd level " + std::to_string(level) + ")");
        const std::string bytes = Read(file);
        uint64_t stored_hash;
        std::memcpy(&stored_hash, bytes.data() + 24, 8);
        Check(bytes.size() > 32 && stored_hash == Xxh64(bytes.data(), 24), "p3a header hash");
        P3aReader r;
        Check(r.Open(file, &err) && r.entries().size() == 3, "p3a opens (" + err + ")");
        Check(r.Has("DATA\\asset\\d3d11\\a.pkg") && !r.Has("data/asset/D3D11/B.pkg"), "p3a names: case and slashes as SenPatcher");
        bool aligned = true;
        for (const auto& kv : r.entries()) aligned = aligned && kv.second.offset % 16 == 0;
        Check(aligned, "p3a data aligned to 16");
        Check(r.Read("data/asset/D3D11/A.pkg", &out, &err) && out == big && r.Read("data/x.bin", &out) && out == "small"
                  && r.Read("empty", &out) && out.empty(),
              "p3a write -> read round trip (" + err + ")");
        const P3aReader::Entry& e = r.entries().at("data/asset/d3d11/a.pkg");
        Check(e.kind == (level > 0 ? 2u : 0u) && Xxh64(bytes.data() + e.offset, static_cast<size_t>(e.stored)) ==
                                                     [&]() { uint64_t h; std::memcpy(&h, bytes.data() + 0x20 + 288, 8); return h; }(),
              "p3a entry kind and stored-bytes hash");
    }
    P3aReader r;
    Write(tmp / "not.p3a", "PK zip, not a p3a");
    Check(!r.Open(tmp / "not.p3a"), "a file that is not a p3a is refused");
}

// Pillow's Image.resize(LANCZOS) of an 11x11 RGBA pattern to 4x4 (premultiplied alpha), byte for byte.
void TestLanczos() {
    const int n = 11;
    std::vector<uint8_t> in(n * n * 4);
    const int alphas[7] = {255, 0, 128, 200, 17, 255, 90};
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            uint8_t* p = &in[(y * n + x) * 4];
            p[0] = static_cast<uint8_t>((x * 37 + y * 11) % 256);
            p[1] = static_cast<uint8_t>((x * x * 5 + y * 3) % 256);
            p[2] = static_cast<uint8_t>((y * 29 + x * 7 + 13) % 256);
            p[3] = static_cast<uint8_t>(alphas[(x + 2 * y) % 7]);
        }
    }
    const uint8_t pillow[] = {38, 5, 45, 134, 161, 75, 60, 139, 132, 169, 77, 131, 78, 185, 101, 146, 73, 12, 122, 125, 181, 86,
                              151, 136, 124, 147, 176, 137, 99, 137, 200, 126, 105, 19, 207, 145, 175, 102, 182, 134, 58, 83,
                              153, 135, 150, 82, 140, 136, 154, 28, 90, 135, 128, 103, 44, 133, 60, 150, 57, 134, 188, 48, 61, 146};
    const std::vector<uint8_t> out = ResizeLanczos(in.data(), n, 4);
    Check(out == std::vector<uint8_t>(pillow, pillow + sizeof(pillow)), "Lanczos matches Pillow byte for byte");
    std::vector<uint8_t> flat(64 * 64 * 4);
    for (size_t i = 0; i < flat.size(); i += 4) {
        flat[i] = 10, flat[i + 1] = 200, flat[i + 2] = 77, flat[i + 3] = 255;
    }
    const std::vector<uint8_t> small = ResizeLanczos(flat.data(), 64, 20);
    bool same = true;
    for (size_t i = 0; i < small.size(); i += 4) same = same && small[i] == 10 && small[i + 1] == 200 && small[i + 2] == 77 && small[i + 3] == 255;
    Check(same, "Lanczos keeps a flat colour");
    const std::vector<uint8_t> two = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4};
    const std::vector<uint8_t> four = StretchNearest(two, 2, 4);
    Check(four[0] == 1 && four[4] == 1 && four[8] == 2 && four[12] == 2 && four[4 * 4 * 2] == 3 && four[4 * 4 * 3 + 12] == 4,
          "nearest stretch back to the cell");
}

// A PhyreEngine-like texture: a header that names RGBA8 and has height, width as u32s ~140 bytes
// before the pixels, then w x h BGRA pixels.
std::string MakePhyre(int w, int h, int seed) {
    std::string head(2000, '\0');
    std::memcpy(&head[100], "PHYRE RGBA8", 11);
    const uint32_t hw[2] = {static_cast<uint32_t>(h), static_cast<uint32_t>(w)};
    std::memcpy(&head[2000 - 140], hw, 8);
    // as the game's: the pixel byte count at 80, log2 of the size 12 bytes before height/width
    const uint32_t bytes = static_cast<uint32_t>(w) * h * 4;
    uint32_t log2 = 0;
    while ((1u << log2) < static_cast<uint32_t>(std::max(w, h))) ++log2;
    std::memcpy(&head[80], &bytes, 4);
    std::memcpy(&head[2000 - 152], &log2, 4);
    std::string px(static_cast<size_t>(w) * h * 4, '\0');
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            char* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<char>((x * 13 + seed) % 256);
            p[1] = static_cast<char>((y * 7 + x * x + seed) % 256);
            p[2] = static_cast<char>(((x / 3) * (y / 5) + seed) % 256);
            p[3] = static_cast<char>((x + y) % 5 == 0 ? 0 : 255 - (x * y) % 128);
        }
    }
    return head + px;
}

std::string Cell(const std::string& phyre, int w, int cell, int row, int col) {
    std::string out;
    const size_t base = phyre.size() - static_cast<size_t>(w) * w * 4;
    for (int y = 0; y < cell; ++y) {
        out += phyre.substr(base + (static_cast<size_t>(row * cell + y) * w + static_cast<size_t>(col) * cell) * 4,
                            static_cast<size_t>(cell) * 4);
    }
    return out;
}

const char kIconConfig[] =
    "{\"targets\": [{\"pkg\": [\"data/asset/D3D11_us/I_TEST.pkg\", \"data/asset/D3D11/I_TEST.pkg\", \"data/asset/D3D11/NONE.pkg\"],"
    " \"texture\": \"icons.png.phyre\", \"cell_grid\": 4, \"draw_size\": 12, \"rows\": [0], \"original_art\": true},"
    " {\"pkg\": [\"data/asset/D3D11/I_TEST.pkg\"], \"texture\": \"icons.png.phyre\", \"cell_grid\": 4, \"draw_size\": 30,"
    " \"rows\": [1]}, {\"disabled\": true, \"pkg\": [\"x\"], \"texture\": \"y\", \"cell_grid\": 1, \"draw_size\": 1}]}";

// A game with SenPatcher's order.txt, a 128px atlas in the data folder (D3D11/), and a "texture
// mod" archive with a 256px one for D3D11_us/.
void MakeIconGame(const fs::path& g) {
    MakeGame(g, "ed8 v1");
    Write(g / "data/asset/D3D11/I_TEST.pkg", MakePkgBytes({{"icons.png.phyre", MakePhyre(128, 128, 1)}, {"other.raw", "keep"}}, ""));
    Write(g / "data/asset/D3D11_us/I_TEST.pkg", MakePkgBytes({{"icons.png.phyre", MakePhyre(128, 128, 2)}}, ""));
    WriteP3a(g / "mods/texmod.p3a",
             {{"data/asset/D3D11_us/I_TEST.pkg", MakePkgBytes({{"icons.png.phyre", MakePhyre(256, 256, 3)}, {"x.raw", "y"}}, "")}}, 3);
    Write(g / "mods/order.txt", "\xef\xbb\xbftexmod.p3a\r\nother_mod.p3a\r\n");
}

void TestIconPack(const fs::path& tmp) {
    std::string err;
    TextureLayout lay;
    Check(FindTextureLayout(MakePhyre(128, 128, 0), 0, 0, &lay, &err) && lay.width == 128 && lay.height == 128 && lay.offset == 2000
              && !lay.mips,
          "texture layout: the header's size picks 128x128 over 64x256 (" + err + ")");
    Check(!FindTextureLayout(std::string(70000, 'x'), 0, 0, &lay, &err), "texture layout: not RGBA8");

    // enlarge: the same texture twice as big, each texel repeated, the header patched
    {
        const std::string small_tex = MakePhyre(64, 64, 5);
        FindTextureLayout(small_tex, 0, 0, &lay, &err);
        std::string big;
        TextureLayout big_lay, found;
        const bool ok = EnlargeTexture(small_tex, lay, 2, &big, &big_lay, &err);
        Check(ok && big_lay.width == 128 && big_lay.height == 128 && big.size() == 2000 + 128 * 128 * 4
                  && big.substr(0, 2000) == MakePhyre(128, 128, 5).substr(0, 2000),
              "enlarge: the header is the 128px texture's (" + err + ")");
        Check(ok && FindTextureLayout(big, 0, 0, &found, &err) && found.width == 128 && found.offset == 2000,
              "enlarge: the result's layout is found again");
        bool texels = ok;
        for (int y = 0; texels && y < 128; ++y) {
            for (int x = 0; texels && x < 128; ++x) {
                texels = big.compare(2000 + (static_cast<size_t>(y) * 128 + x) * 4, 4, small_tex,
                                     2000 + (static_cast<size_t>(y / 2) * 64 + x / 2) * 4, 4) == 0;
            }
        }
        Check(texels, "enlarge: each texel repeated 2x2");
        std::string bad = small_tex;
        bad[80] ^= 1;
        Check(!EnlargeTexture(bad, lay, 2, &big, &big_lay, &err), "enlarge: an unknown header is refused");
    }

    Json j;
    IconPackConfig config;
    Check(Json::Parse(kIconConfig, &j) && IconPackConfig::FromJson(j, &config, &err) && config.targets.size() == 2
              && config.targets[0].original_art && config.targets[0].rows.size() == 1,
          "icon pack config (a disabled target is left out)");

    const fs::path game = tmp / "game_icons";
    MakeIconGame(game);
    Write(game / "mods/zzz_senpatcher_cs1asset.p3a", "x");
    Write(game / "mods/unlisted.p3a", "x");
    const std::vector<std::string> order = ModLoadOrder(game);
    Check(order == std::vector<std::string>({"texmod.p3a", "other_mod.p3a", "unlisted.p3a", "zzz_senpatcher_cs1asset.p3a"}),
          "load order: order.txt (BOM, CRLF), then the unlisted archives, SenPatcher's own last");
    RemoveFile(game / "mods/zzz_senpatcher_cs1asset.p3a");
    RemoveFile(game / "mods/unlisted.p3a");

    Check(GetIconPackStatus(game, config, false).state == IconPackState::NeedsSenPatcher, "status without SenPatcher");
    Check(GetIconPackStatus(game, config, true).state == IconPackState::NotBuilt, "status: not built");

    // --out: the game folder is only read
    const std::string order_before = Read(game / "mods/order.txt");
    IconPackResult r = BuildIconPackFile(game, config, tmp / "out_pack.p3a");
    Check(r.ok && r.packages == 2 && Exists(tmp / "out_pack.p3a") && !Exists(game / "mods" / kIconPackFile)
              && Read(game / "mods/order.txt") == order_before,
          "build to a file of its own leaves the game folder alone (" + r.error + ")");

    r = InstallIconPack(game, config);
    Check(r.ok && r.packages == 2, "icon pack built (" + r.error + ")");
    Check(Read(game / "mods/order.txt") == std::string(kIconPackFile) + "\ntexmod.p3a\nother_mod.p3a\n",
          "the pack goes first in mods/order.txt, the rest kept");
    Check(GetIconPackStatus(game, config, true).state == IconPackState::Built, "status: built");

    P3aReader pack;
    Check(pack.Open(game / "mods" / kIconPackFile, &err) && pack.entries().size() == 2, "the pack has both packages");
    Pkg us, base;
    std::string raw, tex;
    pack.Read("data/asset/D3D11_us/I_TEST.pkg", &raw);
    us.Parse(raw);
    pack.Read("data/asset/D3D11/I_TEST.pkg", &raw);
    base.Parse(raw);
    // D3D11_us: from the texture mod (256px, 64px cells), pictures from the game's own 32px cells
    us.Read("icons.png.phyre", &tex);
    const std::string mod_tex = MakePhyre(256, 256, 3);
    const std::string own_us = MakePhyre(128, 128, 2);
    Check(tex.size() == mod_tex.size(), "D3D11_us is the texture mod's atlas");
    bool cells_ok = true;
    for (int col = 0; col < 4; ++col) {
        const std::string pic = Cell(own_us, 128, 32, 0, col);
        const std::vector<uint8_t> want = StretchNearest(ResizeLanczos(reinterpret_cast<const uint8_t*>(pic.data()), 32, 12), 12, 64);
        cells_ok = cells_ok && Cell(tex, 256, 64, 0, col) == std::string(want.begin(), want.end());
    }
    Check(cells_ok, "original_art: row 0 is the game's own art, pre-shrunk to 12px and stretched to 64px cells");
    Check(Cell(tex, 256, 64, 1, 2) == Cell(mod_tex, 256, 64, 1, 2) && Cell(tex, 256, 64, 0, 1) != Cell(mod_tex, 256, 64, 0, 1),
          "only the configured row changed");
    Check(us.Read("x.raw", &raw) && raw == "y", "the package's other files are kept");
    // D3D11: from the data folder; row 0 (32px cells, 12px: ratio 2.67) changed, the 30px target
    // (ratio 1.07) skipped
    base.Read("icons.png.phyre", &tex);
    const std::string own = MakePhyre(128, 128, 1);
    Check(Cell(tex, 128, 32, 0, 3) != Cell(own, 128, 32, 0, 3) && Cell(tex, 128, 32, 1, 3) == Cell(own, 128, 32, 1, 3),
          "a ratio below 2 is skipped, the rest of the package still packed");
    Check(base.Read("other.raw", &raw) && raw == "keep", "the data folder's package keeps its other files");

    // staleness
    const fs::path tm = game / "mods/texmod.p3a";
    WriteP3a(tm, {{"data/asset/D3D11_us/I_TEST.pkg", MakePkgBytes({{"icons.png.phyre", MakePhyre(256, 256, 4)}}, "")}}, 0);
    IconPackStatus st = GetIconPackStatus(game, config, true);
    Check(st.state == IconPackState::Stale && st.text.find("texture mods changed") != std::string::npos,
          "a changed texture mod makes it out of date (" + st.text + ")");
    WriteP3a(game / "mods/music.p3a", {{"data/bgm/a.opus", "music"}}, 0);
    Check(InstallIconPack(game, config).ok && GetIconPackStatus(game, config, true).state == IconPackState::Built,
          "rebuilt: up to date again");
    WriteP3a(game / "mods/music2.p3a", {{"data/bgm/b.opus", "more music"}}, 0);
    Check(GetIconPackStatus(game, config, true).state == IconPackState::Built, "an archive without the packages changes nothing");
    IconPackConfig other = config;
    other.hash = "changed";
    Check(GetIconPackStatus(game, other, true).text.find("other icons") != std::string::npos, "a changed target list is out of date");
    PutFirstInModOrder(game, "texmod.p3a");
    Check(GetIconPackStatus(game, config, true).text.find("not first") != std::string::npos, "not first in order.txt: out of date");
    PutFirstInModOrder(game, kIconPackFile);

    // nothing to pack (every target below ratio 2): the old pack goes, not an error
    IconPackConfig small;
    Json js;
    Json::Parse("{\"targets\": [{\"pkg\": [\"data/asset/D3D11/I_TEST.pkg\"], \"texture\": \"icons.png.phyre\", "
                "\"cell_grid\": 4, \"draw_size\": 20}]}",
                &js);
    IconPackConfig::FromJson(js, &small);
    r = InstallIconPack(game, small);
    Check(r.ok && r.packages == 0 && !Exists(game / "mods" / kIconPackFile)
              && Read(game / "mods/order.txt").find(kIconPackFile) == std::string::npos,
          "nothing to pack removes the old pack and its order.txt line (" + r.error + ")");
    Check(GetIconPackStatus(game, small, true).state == IconPackState::NotNeeded, "status: not needed");

    // the same target with "enlarge": the 128px atlas (32px cells at 20px) is doubled and packed,
    // the pictures from its own 32px cells
    IconPackConfig enlarge;
    Json::Parse("{\"targets\": [{\"pkg\": [\"data/asset/D3D11/I_TEST.pkg\"], \"texture\": \"icons.png.phyre\", "
                "\"cell_grid\": 4, \"draw_size\": 20, \"rows\": [0], \"enlarge\": true}]}",
                &js);
    IconPackConfig::FromJson(js, &enlarge);
    r = BuildIconPackFile(game, enlarge, tmp / "enlarged.p3a");
    Check(r.ok && r.packages == 1, "enlarge: packed (" + r.error + ")");
    P3aReader big_pack;
    Pkg big_pkg;
    big_pack.Open(tmp / "enlarged.p3a");
    big_pack.Read("data/asset/D3D11/I_TEST.pkg", &raw);
    big_pkg.Parse(raw);
    big_pkg.Read("icons.png.phyre", &tex);
    Check(tex.size() == 2000 + 256 * 256 * 4, "enlarge: the atlas is 256px");
    bool big_ok = true;
    for (int col = 0; col < 4; ++col) {
        const std::string pic = Cell(own, 128, 32, 0, col);
        const std::vector<uint8_t> want = StretchNearest(ResizeLanczos(reinterpret_cast<const uint8_t*>(pic.data()), 32, 20), 20, 64);
        big_ok = big_ok && Cell(tex, 256, 64, 0, col) == std::string(want.begin(), want.end());
    }
    Check(big_ok, "enlarge: row 0 pre-shrunk from the 32px cells into 64px ones");
    std::string doubled;
    for (int y = 0; y < 64; ++y) {
        const std::string src_row = Cell(own, 128, 32, 1, 2).substr(static_cast<size_t>(y / 2) * 32 * 4, 32 * 4);
        for (int x = 0; x < 64; ++x) doubled += src_row.substr(static_cast<size_t>(x / 2) * 4, 4);
    }
    Check(Cell(tex, 256, 64, 1, 2) == doubled, "enlarge: the other cells are the old ones doubled");

    InstallIconPack(game, config);
    Check(RemoveIconPack(game, &err) && !Exists(game / "mods" / kIconPackFile) && !Exists(game / "mods" / kIconPackRecord)
              && Read(game / "mods/order.txt").find(kIconPackFile) == std::string::npos && Exists(game / "mods/texmod.p3a"),
          "remove takes the pack, its record and its order.txt line");
}

// An install makes a new pack only when asked (a small-screen preset under Install everything,
// --icon-pack) and keeps one that is there up to date; uninstall and "remove everything" take it out.
void TestIconPackInstall(const fs::path& tmp) {
    const fs::path pay = tmp / "payload_icons";
    MakePayload(pay, "1.0.0", "MZ logger v1");
    Write(pay / "data/icon_pack.json", kIconConfig);
    Seal(pay / "data");
    Payload payload;
    std::string err;
    Check(Payload::Load(pay, &payload, &err) && payload.icon_pack.targets.size() == 2 && payload.VerifyFiles(&err),
          "a payload with icon_pack.json loads (" + err + ")");

    const fs::path game = tmp / "game_icons_install";
    MakeIconGame(game);
    Check(Inspect(game, &payload).icon_pack.state == IconPackState::NotBuilt, "Inspect: the icon pack is not built");
    InstallResult r = Install(game, payload, InstallOptions());
    Check(r.ok && !Exists(game / "mods" / kIconPackFile) && Inspect(game, &payload).icon_pack.state == IconPackState::NotBuilt,
          "a plain install makes no icon pack (" + r.error + ")");
    InstallOptions build;
    build.icon_pack = InstallOptions::IconPack::Build;
    r = Install(game, payload, build);
    Check(r.ok && Exists(game / "mods" / kIconPackFile) && Lines(Read(game / "mods/order.txt"))[0] == kIconPackFile,
          "--icon-pack builds it, first in order.txt (" + r.error + ")");
    Check(Inspect(game, &payload).icon_pack.state == IconPackState::Built, "Inspect: built");
    fs::remove(game / "mods" / kIconPackFile);
    Check(Install(game, payload, InstallOptions()).ok && Exists(game / "mods" / kIconPackFile)
              && Inspect(game, &payload).icon_pack.state == IconPackState::Built,
          "a plain install rebuilds the pack that was there");
    InstallOptions no;
    no.icon_pack = InstallOptions::IconPack::Skip;
    RemoveIconPack(game);
    Check(Install(game, payload, no).ok && !Exists(game / "mods" / kIconPackFile), "--no-icon-pack leaves it out");
    Check(payload.presets[1].icon_pack && !payload.presets[0].icon_pack, "a preset's icon_pack is read");
    Check(PresetBuildsIconPack(payload.presets[1], Inspect(game, &payload).icon_pack)
              && !PresetBuildsIconPack(payload.presets[0], Inspect(game, &payload).icon_pack),
          "applying a small-screen preset builds the icon pack");
    Check(InstallEverything(game, payload, InstallOptions()).ok && Exists(game / "mods" / kIconPackFile),
          "install everything builds it when the preset recommended here asks for it");
    RemoveIconPack(game);
    Check(InstallEverything(game, payload, no).ok && !Exists(game / "mods" / kIconPackFile),
          "but not with --no-icon-pack");
    Check(Install(game, payload, build).ok && Exists(game / "mods" / kIconPackFile), "and --icon-pack builds it again");
    // removed by the player: gone, remembered across installs, until built by hand again
    Check(RemoveIconPackByPlayer(game, &err) && !Exists(game / "mods" / kIconPackFile)
              && Inspect(game, &payload).icon_pack.state == IconPackState::Removed,
          "removing the icon pack by hand (" + err + ")");
    Check(!PresetBuildsIconPack(payload.presets[1], Inspect(game, &payload).icon_pack), "but not one the player removed");
    Check(Install(game, payload, build).ok && !Exists(game / "mods" / kIconPackFile)
              && Inspect(game, &payload).icon_pack.state == IconPackState::Removed,
          "an install leaves a removed icon pack out, even one asked to build it");
    Check(BuildIconPackByPlayer(game, payload.icon_pack).ok && Exists(game / "mods" / kIconPackFile)
              && !IconPackRemovedByPlayer(game) && Inspect(game, &payload).icon_pack.state == IconPackState::Built,
          "building it by hand takes the removal back");
    r = Uninstall(game, UninstallOptions());
    Check(r.ok && !Exists(game / "mods" / kIconPackFile) && !Exists(game / "mods" / kIconPackRecord)
              && Read(game / "mods/order.txt") == "texmod.p3a\nother_mod.p3a\n" && Exists(game / "mods/texmod.p3a"),
          "uninstall removes the icon pack and its order.txt line (" + r.error + ")");
    Check(Exists(r.backup_dir / "mods" / kIconPackFile), "the uninstall backup has it");
    Install(game, payload, build);
    UninstallOptions purge;
    purge.purge = true;
    r = Uninstall(game, purge);
    Check(r.ok && !Exists(game / "mods" / kIconPackFile) && !Exists(game / "mods" / kIconPackRecord) && Exists(game / "mods/texmod.p3a"),
          "remove everything removes it too (" + r.error + ")");

    // the vanilla game: nothing in mods/, and the status says why
    const fs::path vanilla = tmp / "game_icons_vanilla";
    MakeIconGame(vanilla);
    std::error_code ec;
    fs::remove_all(vanilla / "mods", ec);
    r = Install(vanilla, payload, InstallOptions());
    Check(r.ok && !Exists(vanilla / "mods"), "vanilla: installed, nothing put into mods/ (" + r.error + ")");
    Check(Inspect(vanilla, &payload).icon_pack.state == IconPackState::NeedsSenPatcher, "vanilla: the icon pack needs SenPatcher");
    // SenPatcher's DLL without mods/ or order.txt: both are created
    Write(vanilla / "DINPUT8.dll", std::string("MZ\0\0 SenPatcherHook\0Sen1\0", 26));
    r = Install(vanilla, payload, build);
    Check(r.ok && Exists(vanilla / "mods" / kIconPackFile) && Read(vanilla / "mods/order.txt") == std::string(kIconPackFile) + "\n",
          "SenPatcher without order.txt: the pack is built and listed (" + r.error + ")");
}

void TestSettings(const fs::path& tmp) {
    const fs::path pay = tmp / "payload1";
    const fs::path game = tmp / "game_settings";
    MakePayload(pay, "1.0.0", "MZ logger v1");
    MakeGame(game, "ed8 v1");
    Payload payload;
    std::string err;
    Payload::Load(pay, &payload, &err);
    Install(game, payload, InstallOptions());
    Write(game / "atmt_mods/deckscreen.ini",
          "; settings\n[General]\nEnabled=true   ; keep this comment\nCrispTextWeight=1.50\nCustom=42\n\n[Other]\nX=1\n");

    SettingsModel m;
    m.Load(game, Schema::FromJson(payload.schema));
    Check(m.Get("deckscreen", "General", "Enabled") == "true", "reads a value");
    Check(m.Get("deckscreen", "General", "CrispTextWeight") == "1.5", "normalises a float");
    Check(m.Get("deckscreen", "General", "Mode") == "game", "a missing key shows its default");
    Check(!m.Set("deckscreen", "General", "CrispTextWeight", "3", &err), "out of range is refused");
    Check(!m.Set("deckscreen", "General", "Mode", "fast", &err), "an unknown choice is refused");
    Check(m.Set("deckscreen", "General", "Mode", "MODERN", &err) && m.Get("deckscreen", "General", "Mode") == "modern",
          "an enum is stored in its schema spelling");
    Check(m.Set("deckscreen", "General", "Enabled", "0") && m.PendingCount() == 2, "pending changes");
    Check(m.Set("deckscreen", "General", "Enabled", "yes") && m.PendingCount() == 1, "setting the ini's value drops the change");
    const std::vector<RawEntry> raw = m.Raw("deckscreen");
    Check(raw.size() == 2 && raw[0].key == "Custom", "keys the schema does not know are raw");
    m.SetRaw("deckscreen", "General", "Custom", "43");
    Check(m.Save(&err) == SettingsModel::SaveResult::Saved, "save");
    const std::string ini = Read(game / "atmt_mods/deckscreen.ini");
    Check(ini.find("Enabled=true   ; keep this comment") != std::string::npos && ini.find("Custom=43") != std::string::npos
              && ini.find("Mode=modern") != std::string::npos && ini.find("[Other]\nX=1") != std::string::npos,
          "written in place: comments and other lines survive");

    // a change on disk meanwhile is a conflict
    m.Set("deckscreen", "General", "ForceDeckResolution", "true");
    Write(game / "atmt_mods/deckscreen.ini", ini + "; the overlay wrote this\n");
    std::vector<std::string> conflicts;
    Check(m.Save(&err, &conflicts) == SettingsModel::SaveResult::Conflict && conflicts.size() == 1,
          "an ini changed meanwhile is a conflict, not overwritten");
    m.Reload();
    Check(m.Save(&err) == SettingsModel::SaveResult::Saved
              && Read(game / "atmt_mods/deckscreen.ini").find("; the overlay wrote this") != std::string::npos,
          "after a reload the change is written on top");

    // presets
    const Preset& deck = payload.presets[0];
    auto diff = m.PresetDiff(deck);
    Check(diff.size() == 1 && diff[0].first.key == "LogEnabled", "a preset's diff leaves out what is already set");
    Check(m.ApplyPreset(deck, &err) && m.Save(&err) == SettingsModel::SaveResult::Saved
              && Read(game / "atmt_loader.ini").find("LogEnabled=false") != std::string::npos,
          "a preset writes the loader's ini too");
    m.ResetToDefaults("deckscreen");
    m.Save(&err);
    Check(m.Get("deckscreen", "General", "ForceDeckResolution") == "false"
              && m.Get("deckscreen", "General", "Mode") == "game",
          "reset to defaults");

    // queued while the game runs
    m.Set("deckscreen", "General", "Enabled", "false");
    Check(m.Queue(&err) && HasQueuedSettings(game) && !m.Dirty(), "changes queued");
    Check(ApplyQueuedSettings(game, m.schema(), &err) == 1 && !HasQueuedSettings(game)
              && Read(game / "atmt_mods/deckscreen.ini").find("Enabled=false") != std::string::npos,
          "queued changes written later");
}

// The values a recorded key / pad press is saved as: the schema's spelling and modifier order.
void TestInputChords() {
    Json j;
    Check(Json::Parse("{\"input\": {\"keys\": [\"F3\", \"PageUp\", \"L\"], \"key_modifiers\": [\"Ctrl\", \"Shift\", "
                      "\"Alt\", \"Win\"], \"key_chord_separator\": \"+\", \"pad_buttons\": [\"A\", \"LB\", \"START\"], "
                      "\"pad_chord_separator\": \"+\"}, \"groups\": []}",
                      &j),
          "input schema parses");
    const Schema s = Schema::FromJson(j);
    Check(s.input_key_modifiers.size() == 4 && s.key_chord_separator == "+", "the schema's key modifiers");
    Check(s.KeyChordValue({}, "f3") == "F3", "a plain key, in the schema's spelling");
    Check(s.KeyChordValue({"alt", "Shift"}, "L") == "Shift+Alt+L", "modifiers in the schema's order");
    Check(s.KeyChordValue({"Ctrl", "Hyper"}, "PageUp") == "Ctrl+PageUp", "an unknown modifier is dropped");
    Check(s.KeyChordValue({"Ctrl"}, "NoSuchKey").empty(), "an unknown key is no value");
    Check(s.PadChordValue({"LB", "a"}) == "LB+A" && s.PadChordValue({"start"}) == "START", "pad chords");
    Check(s.PadChordValue({"L2"}).empty(), "a button the schema does not list is no value");
}

void TestSteamShortcuts(const fs::path& tmp) {
    // a shortcuts.vdf with somebody else's entry, using every value type Steam writes
    BinVdf root;
    BinVdf list;
    list.type = BinVdf::Map;
    list.name = "shortcuts";
    BinVdf other;
    other.type = BinVdf::Map;
    other.name = "0";
    BinVdf v;
    v.type = BinVdf::Int32;
    v.name = "appid";
    v.u32 = 0x8badf00d;
    other.children.push_back(v);
    v = BinVdf();
    v.type = BinVdf::String;
    v.name = "AppName";
    v.str = "Some Emulator";
    other.children.push_back(v);
    v = BinVdf();
    v.type = BinVdf::UInt64;
    v.name = "big";
    v.u64 = 0x1122334455667788ull;
    other.children.push_back(v);
    BinVdf tags;
    tags.type = BinVdf::Map;
    tags.name = "tags";
    v = BinVdf();
    v.type = BinVdf::String;
    v.name = "0";
    v.str = "favorite";
    tags.children.push_back(v);
    other.children.push_back(tags);
    list.children.push_back(other);
    root.children.push_back(list);
    const std::string original = WriteBinVdf(root);
    BinVdf back;
    Check(ParseBinVdf(original, &back) && WriteBinVdf(back) == original, "binary vdf round trip");

    const fs::path file = tmp / "steam" / "shortcuts.vdf";
    Write(file, original);
    std::string err;
    Check(AddShortcut(file, tmp / "My Apps" / "atmt-manager.AppImage", tmp / "icon.png", &err) && HasShortcut(file),
          "a shortcut is added (" + err + ")");
    Check(AddShortcut(file, tmp / "atmt-manager.AppImage", fs::path(), &err), "adding again replaces it");
    BinVdf now;
    ParseBinVdf(Read(file), &now);
    const BinVdf* entries = now.Find("shortcuts");
    Check(entries != nullptr && entries->children.size() == 2 && entries->children[1].Find("Exe")->str.find("My Apps") == std::string::npos,
          "one entry of ours, the other kept");
    Check(entries != nullptr && entries->children[0].Find("big")->u64 == 0x1122334455667788ull
              && entries->children[0].Find("tags")->Find("0")->str == "favorite",
          "the other entry keeps every value");
    Check(Exists(fs::path(file).concat(".atmt_bak")) && Read(fs::path(file).concat(".atmt_bak")) == original,
          "the file is backed up before the first change");
    bool removed = false;
    Check(RemoveShortcut(file, &removed, &err) && removed && Read(file) == original, "removing gives the original file back");
    Check(RemoveShortcut(file, &removed, &err) && !removed, "removing again changes nothing");
    const fs::path fresh = tmp / "steam2" / "shortcuts.vdf";
    Check(AddShortcut(fresh, tmp / "a.AppImage", fs::path(), &err) && HasShortcut(fresh), "a missing shortcuts.vdf is created");
    Check((ShortcutAppId("\"x\"", "y") & 0x80000000u) != 0, "a non-Steam app id has the top bit set");

    // the Deck controller layout (configset_controller_neptune.vdf, as Steam writes it: a BOM, tabs)
    SteamAccount acct;
    acct.config_dir = tmp / "steam3" / "userdata" / "42" / "config";
    acct.id = "42";
    const fs::path cs = DeckControllerConfig(acct);
    Check(cs == tmp / "steam3" / "steamapps" / "common" / "Steam Controller Configs" / "42" / "config" / "configset_controller_neptune.vdf",
          "the layout file is the account's in Steam Controller Configs");
    Check(SetDeckControllerLayout(cs, &err) && Read(cs).find("\"atmt manager\"") != std::string::npos
              && Read(cs).find(kDeckControllerTemplate) != std::string::npos,
          "a missing layout file is created with ours (" + err + ")");
    Check(ClearDeckControllerLayout(cs, &err) && Read(cs).find("atmt manager") == std::string::npos, "and removed again");
    const std::string theirs =
        "\xEF\xBB\xBF\"controller_config\"\n{\n\t\"538680\"\n\t{\n\t\t\"autosave\"\t\t\"1\"\n\t}\n"
        "\t\"x\\\"y\"\n\t{\n\t\t\"template\"\t\t\"a\\\\b.vdf\"\n\t}\n\t\"atmt manager\"\n\t{\n\t\t\"workshop\"\t\t\"123\"\n\t}\n}\n";
    Write(cs, theirs);
    Check(SetDeckControllerLayout(cs, &err) && Read(cs) == theirs, "a layout the player chose is kept, the file untouched");
    Check(ClearDeckControllerLayout(cs, &err) && Read(cs) == theirs, "and not removed");
    Write(cs, theirs.substr(0, theirs.find("\t\"atmt manager\"")) + "}\n");
    Check(SetDeckControllerLayout(cs, &err), "ours is added next to the others (" + err + ")");
    Vdf cv;
    Check(ParseVdf(Read(cs), &cv) && cv.Find("controller_config") != nullptr
              && cv.Find("controller_config")->Find("538680") != nullptr
              && cv.Find("controller_config")->Find("538680")->Str("autosave") == "1"
              && cv.Find("controller_config")->Find("x\"y") != nullptr
              && cv.Find("controller_config")->Find("x\"y")->Str("template") == "a\\b.vdf"
              && cv.Find("controller_config")->Find("atmt manager")->Str("template") == kDeckControllerTemplate,
          "the other games' layouts survive the rewrite, quotes and backslashes too");
}

// A .tar.gz of the given files: ustar headers, the gzip made of stored deflate blocks.
std::string TarGz(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string tar;
    for (const auto& f : files) {
        char h[512] = {};
        std::snprintf(h, 100, "%s", f.first.c_str());
        std::snprintf(h + 100, 8, "%07o", 0644);
        std::snprintf(h + 108, 8, "%07o", 0);
        std::snprintf(h + 116, 8, "%07o", 0);
        std::snprintf(h + 124, 12, "%011o", static_cast<unsigned>(f.second.size()));
        std::snprintf(h + 136, 12, "%011o", 0);
        h[156] = '0';
        std::memcpy(h + 257, "ustar\0" "00", 8);
        std::memset(h + 148, ' ', 8);
        unsigned sum = 0;
        for (char c : h) sum += static_cast<unsigned char>(c);
        std::snprintf(h + 148, 8, "%06o", sum);
        tar.append(h, 512);
        tar += f.second;
        tar.append((512 - f.second.size() % 512) % 512, '\0');
    }
    tar.append(1024, '\0');
    std::string gz("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    for (size_t pos = 0;;) {
        const size_t n = std::min<size_t>(65535, tar.size() - pos);
        const bool last = pos + n == tar.size();
        gz += static_cast<char>(last ? 1 : 0);
        gz += static_cast<char>(n & 0xff);
        gz += static_cast<char>(n >> 8);
        gz += static_cast<char>(~n & 0xff);
        gz += static_cast<char>((~n >> 8) & 0xff);
        gz += tar.substr(pos, n);
        pos += n;
        if (last) break;
    }
    const uint32_t crc = Crc32(tar), size = static_cast<uint32_t>(tar.size());
    for (int i = 0; i < 4; ++i) gz += static_cast<char>((crc >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) gz += static_cast<char>((size >> (8 * i)) & 0xff);
    return gz;
}

// A component folder as the archive a release carries.
std::string ComponentArchive(const fs::path& dir) {
    std::vector<std::pair<std::string, std::string>> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) files.emplace_back(Rel(it->path(), dir), Read(it->path()));
    }
    return TarGz(files);
}

// The manifest, its signature, the comparison with the local versions, and a component download
// put next to the bundled payload (no network: the archive is handed over).
void TestManifest(const fs::path& tmp) {
    std::string pub, sec, sig, err;
    GenerateKeyPair(&pub, &sec, &err);
    const fs::path bundled = tmp / "bundled";
    MakePayload(bundled, "1.0.0", "MZ logger v1");
    MakePayload(tmp / "payload_next", "1.1.0", "MZ logger v2");
    const std::string archive = ComponentArchive(tmp / "payload_next" / "trails_dialog_logger");
    const std::string manifest_text =
        "{\"format\": 1, \"published\": \"2026-10-02T00:00:00Z\", \"page\": \"https://example.invalid/r\","
        " \"manager\": {\"version\": \"99.0.0\", \"changelog\": \"faster\", \"downloads\": {"
        "  \"windows\": {\"url\": \"https://example.invalid/m.exe\", \"sha256\": \"" + Sha256Hex("exe") + "\"},"
        "  \"linux\": {\"url\": \"https://example.invalid/m.AppImage\", \"sha256\": \"" + Sha256Hex("appimage") + "\"}}},"
        " \"components\": ["
        "  {\"name\": \"loader\", \"kind\": \"loader\", \"title\": \"Loader\", \"version\": \"1.0.0\","
        "   \"url\": \"https://example.invalid/loader-1.0.0.tar.gz\", \"sha256\": \"" + Sha256Hex("old") + "\"},"
        "  {\"name\": \"trails_dialog_logger\", \"kind\": \"mod\", \"title\": \"Dialog log\", \"version\": \"1.1.0\","
        "   \"changelog\": \"more lines\", \"url\": \"https://example.invalid/trails_dialog_logger-1.1.0.tar.gz\","
        "   \"sha256\": \"" + Sha256Hex(archive) + "\"},"
        "  {\"name\": \"new_mod\", \"kind\": \"mod\", \"title\": \"New\", \"version\": \"0.1.0\", \"min_manager_version\": \"99.0.0\","
        "   \"url\": \"https://example.invalid/new_mod-0.1.0.tar.gz\", \"sha256\": \"" + Sha256Hex("new") + "\"}]}";
    SignMessage(sec, manifest_text, "manifest", &sig, &err);
    Manifest m;
    Check(Updater::VerifyManifest(manifest_text, sig, pub, &m, &err) && m.components.size() == 3
              && m.manager_downloads.size() == 2 && m.manager_version == "99.0.0",
          "a signed manifest verifies (" + err + ")");
    Check(!Updater::VerifyManifest(manifest_text + " ", sig, pub, &m, &err), "a changed manifest is refused");
    std::string other_pub, other_sec;
    GenerateKeyPair(&other_pub, &other_sec, &err);
    Check(!Updater::VerifyManifest(manifest_text, sig, other_pub, &m, &err), "another key is refused");
    Json bad;
    Json::Parse("{\"format\": 1, \"components\": [{\"name\": \"../x\", \"version\": \"1\", \"url\": \"https://a\", \"sha256\": \""
                    + Sha256Hex("x") + "\"}]}", &bad);
    Check(!Manifest::FromJson(bad, &m, &err), "a component name that is not a plain name is refused");
    Json::Parse("{\"format\": 1, \"components\": [{\"name\": \"x\", \"version\": \"1\", \"url\": \"http://a\", \"sha256\": \""
                    + Sha256Hex("x") + "\"}]}", &bad);
    Check(!Manifest::FromJson(bad, &m, &err), "a download that is not https is refused");
    Json::Parse("{\"format\": 2}", &bad);
    Check(!Manifest::FromJson(bad, &m, &err), "a manifest format this app does not know is refused");
    Updater::VerifyManifest(manifest_text, sig, pub, &m, &err);

    SetEnv("ATMT_PAYLOAD", U8(bundled));
    Payload local;
    Check(FindBestPayload(&local, &err) && local.FindComponent("trails_dialog_logger")->version == "1.0.0",
          "the bundled payload is found (" + err + ")");
    UpdateCheck c;
    c.manifest = m;
    c.Compare(local.Versions());
    Check(c.status == UpdateCheck::Status::Available && c.app_newer && c.components.size() == 2
              && c.components[0].remote.name == "trails_dialog_logger" && c.components[0].local_version == "1.0.0"
              && c.components[1].remote.name == "new_mod" && c.components[1].needs_newer_app
              && c.Installable().size() == 1 && c.NeedsNewerApp(),
          "each component is compared on its own: the loader is current, the logger newer, a new mod needs a newer app");

    RemoteComponent wrong = c.components[0].remote;
    wrong.download.sha256 = Sha256Hex("something else");
    Check(!Updater::InstallComponentArchive(wrong, archive, &err), "a download that does not match its sha256 is refused");
    RemoteComponent renamed = c.components[0].remote;
    renamed.name = "deckscreen";
    renamed.version = "9.0.0";
    Check(!Updater::InstallComponentArchive(renamed, archive, &err), "an archive of another component is refused");
    Check(Updater::InstallComponentArchive(c.components[0].remote, archive, &err)
              && Exists(DownloadedComponentsDir() / "trails_dialog_logger" / "1.1.0" / "component.json"),
          "a component download is unpacked into the components folder (" + err + ")");
    Payload fresh;
    Check(FindBestPayload(&fresh, &err) && fresh.FindComponent("trails_dialog_logger")->version == "1.1.0"
              && Read(fresh.FindFile("atmt_mods/trails_dialog_logger.dll")->source) == "MZ logger v2"
              && fresh.FindComponent("loader")->dir == bundled / "loader",
          "the payload takes the newest of each component: the downloaded logger, the bundled loader (" + err + ")");
    Check(!Updater::InstallComponentArchive(c.components[0].remote, archive, &err), "the same version again is refused");
    c.Compare(fresh.Versions());
    Check(c.Installable().empty() && c.components.size() == 1, "after the download only the app and the new mod are left");

    // a component download this app cannot use yet is not taken
    MakeComponent(tmp / "future", "deckscreen", "mod", "2.0.0", "Deckscreen", {{"files/atmt_mods/deckscreen.dll", "MZ v2"}});
    Write(tmp / "future" / "component.json",
          "{\"name\": \"deckscreen\", \"kind\": \"mod\", \"version\": \"2.0.0\", \"min_manager_version\": \"99.0.0\"}");
    Seal(tmp / "future");
    RemoteComponent future;
    future.name = "deckscreen";
    future.version = "2.0.0";
    const std::string future_archive = ComponentArchive(tmp / "future");
    future.download.sha256 = Sha256Hex(future_archive);
    Check(Updater::InstallComponentArchive(future, future_archive, &err) && FindBestPayload(&fresh, &err)
              && fresh.FindComponent("deckscreen")->version == "1.0.0",
          "a component that needs a newer app stays unused");
    SetEnv("ATMT_PAYLOAD", "");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetLog([](const std::string&) {});   // the core's own lines would drown the results
    const fs::path tmp = fs::temp_directory_path() / "atmt_manager_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    // the downloaded components and state.json go into the test's own folder
    SetEnv("LOCALAPPDATA", U8(tmp / "appdata"));
    SetEnv("XDG_DATA_HOME", U8(tmp / "appdata"));

    TestHashes();
    TestJson();
    TestSignatures();
    TestArchive(tmp);
    TestVdf();
    TestLocator(tmp);
    TestInstall(tmp);
    TestSenPatcher(tmp);
    TestSenPatcherProton(tmp);
    TestFalcom();
    TestPkg();
    TestP3a(tmp);
    TestLanczos();
    TestIconPack(tmp);
    TestIconPackInstall(tmp);
    TestSettings(tmp);
    TestInputChords();
    TestSteamShortcuts(tmp);
    TestManifest(tmp);

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
