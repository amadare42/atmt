// mod_api.h - the contract between the loader and the mods it hosts.
//
// The loader (loader/) is game-agnostic: it gets into the process (as an injected
// dll or by impersonating a dll the game imports), provides a few services, and
// discovers + loads mod dlls. A mod is a plain dll in the loader's mod folder that
// exports the functions below (AtmtModInit; AtmtModShutdown and AtmtModDescription optional).
//
// A mod never talks to Windows hooking APIs directly: it uses hook_create/... so
// that only one MinHook instance exists in the process, no matter how many mods
// are installed. Anything the mod needs from its environment (paths, logging,
// config) comes through this struct, so a mod never has to guess where the game
// is or whether it was injected or auto-loaded.
//
// ABI rules: plain C, 32-bit, __cdecl, no C++ types, no exceptions across the
// boundary, and every pointer stays valid for the lifetime of the process.
#ifndef ATMT_MOD_API_H
#define ATMT_MOD_API_H

#include <stdint.h>
#include <wchar.h>

// Bump when the layout changes; the loader refuses mods that ask for more than it
// implements, and mods should refuse a loader that offers less than they need.
#define ATMT_MOD_API_VERSION 1u

// Exported by every mod.
#define ATMT_MOD_ENTRY_NAME "AtmtModInit"        // uint32_t AtmtModInit(const AtmtModApi*)
#define ATMT_MOD_SHUTDOWN_NAME "AtmtModShutdown" // void AtmtModShutdown(void)
// Optional, but every mod of the toolkit has it: what the mod does, in one sentence (UTF-8, plain
// text, a static string). It is an export rather than an api call so that it can be read without
// starting the mod and from a mod that has no settings: the loader reads it when it loads the dll
// and hands it out with the mod's settings (AtmtSettingsGroup::description - the overlay shows it
// at the top of the mod's menu), and tests/schema_dump.cpp copies it into settings_schema.json,
// where the manager's Mods screen takes it from.
#define ATMT_MOD_DESCRIPTION_NAME "AtmtModDescription" // const char* AtmtModDescription(void)

/* ------------------------------------------------------------------ settings
 * A mod describes its settings as data - a table of AtmtSetting - and hands it to
 * api->settings_register. It never draws anything and never parses its ini itself:
 *
 *   - on register, the loader reads every key from the mod's ini (config_path) into
 *     the mod's own storage (`value`), clamped to the declared range; a key the ini
 *     does not have yet is appended to it with its current value as the default and
 *     `help` as a comment above it, so the file stays a complete list of settings;
 *   - a settings UI (the overlay's top bar) enumerates every registered table with
 *     settings_acquire/settings_release and changes values with settings_set, which
 *     writes the mod's storage and calls on_change;
 *   - settings_commit writes every changed value back into its ini, in place (the
 *     line's value is replaced; its comment and every other line stay as they were).
 *
 * Storage per type (what `value` points at, owned by the mod, alive while registered):
 *   BOOL                     int32_t (0/1)
 *   INT                      int32_t, clamped to [min, max] when min < max
 *   FLOAT                    float,   clamped to [min, max] when min < max
 *   ENUM                     int32_t index into `choices` (the ini stores the token)
 *   STRING, KEY, PAD_BUTTON  char[capacity], NUL-terminated; KEY and PAD_BUTTON are
 *                            names ("F3", "B") - the type only tells a UI to offer
 *                            "press a key/button" instead of a text field
 *   ACTION                   none: a button; settings_set just calls on_change
 *   LABEL                    none: a caption / separator; `label` may be NULL
 *
 * Threads: settings_set (and so on_change) runs on whichever thread calls it - for
 * the overlay that is the render thread - with the registry lock held. A 32-bit
 * value written there is atomic on x86, so a hook may keep reading a plain global;
 * anything bigger than that belongs in on_change. on_change must not block.
 */
typedef enum AtmtSettingType {
    ATMT_SETTING_BOOL = 0,
    ATMT_SETTING_INT,
    ATMT_SETTING_FLOAT,
    ATMT_SETTING_ENUM,
    ATMT_SETTING_STRING,
    ATMT_SETTING_KEY,
    ATMT_SETTING_PAD_BUTTON,
    ATMT_SETTING_ACTION,
    ATMT_SETTING_LABEL,
} AtmtSettingType;

#define ATMT_SETTING_LIVE     0x1u  /* takes effect as soon as it is set */
#define ATMT_SETTING_RESTART  0x2u  /* takes effect after the game restarts */
#define ATMT_SETTING_ADVANCED 0x4u  /* diagnostics / development: hidden by default */
#define ATMT_SETTING_READONLY 0x8u  /* shown, never changed from a UI */

typedef struct AtmtSetting {
    uint32_t type;           /* AtmtSettingType */
    uint32_t flags;          /* ATMT_SETTING_* */
    const char* section;     /* ini section, e.g. "General" (NULL = "General") */
    const char* key;         /* ini key; NULL for ACTION and LABEL */
    const char* label;       /* what a UI shows ("Camera speed"); NULL = key */
    const char* help;        /* tooltip, and the comment above a newly added ini key */
    void* value;             /* see "storage per type" */
    uint32_t capacity;       /* STRING/KEY/PAD_BUTTON: size of the buffer */
    float min, max, step;    /* INT/FLOAT: range (ignored when min >= max) and UI step */
    const char* const* choices;   /* ENUM: the ini tokens, e.g. {"game","off","modern"} */
    uint32_t choice_count;
    void(__cdecl* on_change)(const struct AtmtSetting* setting, void* user);
    void* user;
} AtmtSetting;

/* One registered table, as settings_acquire hands it out. */
typedef struct AtmtSettingsGroup {
    const char* mod_name;    /* e.g. "stick_rotation_speed" */
    const char* title;       /* the menu title the mod asked for, e.g. "Camera" */
    const char* description; /* the mod's AtmtModDescription(), "" when it has none */
    const AtmtSetting* settings;   /* the loader's copy: pass these to settings_set */
    uint32_t count;
} AtmtSettingsGroup;

typedef struct AtmtModApi {
    /* --- version / identity --- */
    uint32_t version;         /* ATMT_MOD_API_VERSION implemented by the loader */
    uint32_t size;            /* sizeof(AtmtModApi), so mods can detect truncation */
    void* loader_module;      /* HMODULE of the loader dll */
    void* game_module;        /* HMODULE of the host executable (ed8.exe) */
    const wchar_t* game_dir;  /* folder the executable runs from (writable) */
    const wchar_t* mod_dir;   /* this mod's own folder: put assets here */
    const wchar_t* mod_name;  /* base name, e.g. L"trails_dialog_logger" */
    const wchar_t* config_path; /* <mod_dir>\<mod_name>.ini, always exists */
    const wchar_t* log_path;  /* <game_dir>\atmt_loader.log, shared by all mods */

    /* --- logging: one line in log_path, thread safe ---
     * log: what happened (written only with LogLevel=all in atmt_loader.ini - the default is errors)
     * log_error: something did not work (a hook that could not be installed, a setting that is not
     *   valid, a file that could not be written): written unless LogLevel=off. That is what a player
     *   who never touched a setting sees, so it is for failures only. */
    void(__cdecl* log)(const char* utf8_text);
    void(__cdecl* log_error)(const char* utf8_text);

    /* --- hooks (MinHook, single instance in the loader) ---
     * target   : address to hook (must be executable, or it is refused)
     * detour   : replacement function
     * trampoline: receives the address of the original code (may be NULL)
     * returns a handle on success, NULL on failure; handle == NULL passed to
     * enable/disable/remove means "all hooks".
     */
    void*(__cdecl* hook_create)(void* target, void* detour, void** trampoline);
    int(__cdecl* hook_enable)(void* handle);
    int(__cdecl* hook_disable)(void* handle);
    int(__cdecl* hook_remove)(void* handle);

    /* --- settings (see "settings" above) ---
     * settings_register: `self` is the api pointer this mod was given. The table is
     *   copied (its strings and `value` pointers must stay valid until unregistered);
     *   registering again replaces this mod's table. Loads the ini into `value`.
     *   Returns the number of settings, or -1 when the table is invalid (logged).
     * settings_unregister: drops this mod's table. The loader also does it when the
     *   mod is shut down or refuses to start, so a UI never points into a gone dll.
     * settings_acquire: locks the registry and returns every table (and `*count`;
     *   `*generation` changes whenever a table is added, replaced or removed). The
     *   pointers stay valid until settings_release, which must follow on the same
     *   thread. Recursive: settings_set may be called in between.
     * settings_set: `setting` is an entry of an acquired table, or of the mod's own
     *   table (matched by its `value` pointer). `new_value` points at the same storage
     *   type as `value` (a NUL-terminated string for the string types; NULL for
     *   ACTION). Clamps, stores, calls on_change. Returns 1 when the value changed
     *   (it is then saved by the next commit), 0 when it did not, -1 when unknown.
     * settings_commit: writes every changed value into its mod's ini. Returns the
     *   number of values written, or -1 when an ini could not be written.
     */
    int(__cdecl* settings_register)(const struct AtmtModApi* self, const char* title,
                                    const AtmtSetting* table, uint32_t count);
    void(__cdecl* settings_unregister)(const struct AtmtModApi* self);
    const AtmtSettingsGroup*(__cdecl* settings_acquire)(uint32_t* count, uint32_t* generation);
    void(__cdecl* settings_release)(void);
    int(__cdecl* settings_set)(const AtmtSetting* setting, const void* new_value);
    int(__cdecl* settings_commit)(void);

    /* --- services: an interface one mod offers the others, by name ---
     * service_publish: `iface` (a struct of function pointers, typically) under `name`; the
     *   loader drops it when the publishing mod shuts down or refuses to start. Publishing a
     *   name again replaces it. Returns 0, or -1 when `self` is not a mod.
     * service_find: the interface published under `name`, or NULL. Mods load one after the
     *   other in no guaranteed order, so a mod that wants another's service asks again later
     *   (from its own thread) rather than giving up on the first NULL.
     * The shape of each interface is the publisher's business: see shared/overlay_api.h.
     */
    int(__cdecl* service_publish)(const struct AtmtModApi* self, const char* name, const void* iface);
    const void*(__cdecl* service_find)(const char* name);
} AtmtModApi;

typedef uint32_t(__cdecl* AtmtModInitFn)(const AtmtModApi* api);
typedef void(__cdecl* AtmtModShutdownFn)(void);
typedef const char*(__cdecl* AtmtModDescriptionFn)(void);

#endif  // ATMT_MOD_API_H
