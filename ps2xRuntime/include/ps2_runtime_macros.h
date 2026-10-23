#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H

// The instruction macros now live in ps2_guest_macros.h, which recompiled
// code includes without the rest of the runtime. Runtime sources and tests
// keep including this header and get both.

#include "ps2_runtime.h"
#include "ps2_guest_macros.h"

#endif // PS2_RUNTIME_MACROS_H
