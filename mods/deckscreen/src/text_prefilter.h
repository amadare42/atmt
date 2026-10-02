// text_prefilter.h - crisp small text (see text_prefilter.cpp).
#pragma once

#include <cstdint>

#include "mod_api.h"

namespace text_prefilter {

using LogFn = void (*)(const char* fmt, ...);

// Both are read live: `setting` nonzero = on; `weight` lifts thin strokes' coverage (1 = none).
void Init(const AtmtModApi* api, LogFn log, const int32_t* setting, const float* weight);
// Called from the text draw hook (game thread): the Phyre texture the line is drawn from, and the
// quad it is drawn with (FUN_005a1750's second argument).
void OnTextDraw(const void* texture, const void* quad);

}  // namespace text_prefilter
