// host.cpp - the mod's view of the services the loader provides.
//
// The mod is a guest in a larger process, so everything it needs from the outside
// world (paths, logging, hooks) arrives here as a small set of function pointers
// instead of being discovered by the mod itself. That is also what keeps the
// standalone test harnesses working: with nothing set, HostLog is silent and
// Hooks() reports no service, and every caller degrades gracefully.
#include "atmt.h"

namespace atmt {

namespace {
void (*g_host_log)(const char* text) = nullptr;
void (*g_host_log_error)(const char* text) = nullptr;
HookApi g_hooks;
}  // namespace

void SetHostLog(void (*fn)(const char* text)) {
    g_host_log = fn;
}

void HostLog(const char* message) {
    if (g_host_log != nullptr && message != nullptr) g_host_log(message);
}

void SetHostLogError(void (*fn)(const char* text)) {
    g_host_log_error = fn;
}

void HostLogError(const char* message) {
    if (g_host_log_error != nullptr && message != nullptr) g_host_log_error(message);
}

void SetHookApi(const HookApi& api) {
    g_hooks = api;
}

HookApi& Hooks() {
    return g_hooks;
}

}  // namespace atmt
