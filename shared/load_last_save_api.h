// load_last_save_api.h - the load_last_save mod's interface for other mods.
//
// load_last_save (mods/load_last_save) owns the title route: a hook on the title screen's update that
// drives the title's own save menu and the save manager to one slot, so the game enters that save
// exactly as if the player had picked it under "Load" (docs/MODS.md, "Autoload"). It publishes this
// interface with the loader (AtmtModApi::service_publish, name ATMT_LOAD_LAST_SAVE_SERVICE); a mod that
// sends the game back to the title and wants a save loaded there (battle_load's title route) finds it
// with service_find.
//
// ABI rules as in mod_api.h: plain C, 32-bit, __cdecl, no exceptions across the boundary.
#ifndef ATMT_LOAD_LAST_SAVE_API_H
#define ATMT_LOAD_LAST_SAVE_API_H

#include <stdint.h>

#define ATMT_LOAD_LAST_SAVE_SERVICE "atmt.load_last_save"
#define ATMT_LOAD_LAST_SAVE_API_VERSION 1u

typedef struct AtmtLoadLastSaveApi {
    uint32_t version;   /* ATMT_LOAD_LAST_SAVE_API_VERSION */
    uint32_t size;      /* sizeof(AtmtLoadLastSaveApi) */
    /* Arms the title route for `file_name` ("save012.dat", "autosave03.dat" - a leaf name, the folder
     * is the game's): the next time the title screen is up and quiet, it loads that save. Any thread.
     * Returns 1, or 0 with the reason in `why_not` (NUL-terminated, may be NULL). */
    int(__cdecl* load_at_title)(const char* file_name, char* why_not, uint32_t why_not_size);
} AtmtLoadLastSaveApi;

#endif  // ATMT_LOAD_LAST_SAVE_API_H
