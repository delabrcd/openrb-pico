#pragma once

// Debug-enabled gate. When 0, forces ORB_LOG_LEVEL to NONE before orb_log.h so every
// LOG_* compiles out entirely -- this lets debug-gated declarations (e.g. get_command_name)
// be safely absent in a release build.
#ifndef OPENRB_DEBUG_ENABLED
#define OPENRB_DEBUG_ENABLED 1
#endif

#if !OPENRB_DEBUG_ENABLED
#undef ORB_LOG_LEVEL
#define ORB_LOG_LEVEL LOG_LEVEL_NONE
#endif

#include "orb_log.h"
