// loader.h - internals of the loader dll (paths, logging, hook service).
//
// The loader is deliberately game-agnostic and small: it gets itself into the
// process, provides services to mods (paths, logging, hooks), loads the mod dlls it
// finds, and stays out of the way. Game-specific behaviour belongs in a mod.
#pragma once

#include <windows.h>

#include <string>

#include "loader_config.h"
#include "mod_api.h"

namespace atmt_loader {

// This dll's own module handle, stored by DllMain. Every path is built from it, so
// the loader works the same whether it sits in the game folder (proxy role) or
// somewhere else (injected by path).
extern HMODULE g_self_module;

// Paths are returned as references to cached strings: the mod API hands their
// c_str() out to mods, so they must stay valid for the whole process lifetime.
const std::wstring& SelfDir();        // folder of this dll
const std::wstring& SelfPath();
const std::wstring& GameDir();        // folder of the host executable
const std::wstring& GameModulePath();

// ---------------------------------------------------------------- logging
// LogInit says where <game_dir>\atmt_loader.log goes and what it takes (LogLevel in atmt_loader.ini).
// The file is opened - truncating an earlier session's - with the first line that is written, so
// with the default (errors) a session where nothing fails leaves no file at all. Without LogInit
// (and whatever the level) every line still goes to OutputDebugString.
void LogInit(LogLevel level);
void Log(const char* format, ...);          // informational: written with LogLevel=all only
void LogW(const wchar_t* format, ...);
void LogError(const char* format, ...);     // a failure: written unless LogLevel=off
void LogErrorW(const wchar_t* format, ...);
void LogClose();
void ModLog(const char* text);              // what mods call through api->log
void ModLogError(const char* text);         // ... and through api->log_error

// ---------------------------------------------------------------- hooks
// MinHook lives here only: one instance per process, shared by all mods. The mod
// API exports these through function pointers (see shared/mod_api.h).
void* HookCreate(void* target, void* detour, void** trampoline);
int HookEnable(void* handle);
int HookDisable(void* handle);
int HookRemove(void* handle);
void HookShutdown();

// ---------------------------------------------------------------- settings
// The registry behind AtmtModApi::settings_* (loader_settings.cpp). `owner` is the mod's
// api pointer; loader_main.cpp resolves it to the mod's name, ini and description (its
// AtmtModDescription export, NULL when it has none) before calling SettingsRegister, and
// unregisters a mod when it shuts down or refuses to start.
int SettingsRegister(const void* owner, const std::string& mod_name, const std::wstring& ini_path,
                     const char* title, const char* description, const AtmtSetting* table,
                     uint32_t count);
void SettingsUnregister(const void* owner);
const AtmtSettingsGroup* SettingsAcquire(uint32_t* count, uint32_t* generation);
void SettingsRelease();
int SettingsSet(const AtmtSetting* setting, const void* new_value);
int SettingsCommit();

}  // namespace atmt_loader
