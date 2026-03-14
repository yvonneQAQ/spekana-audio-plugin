#pragma once

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

// Xcode 15.4's macOS SDK can reference __builtin_verbose_trap from system
// headers even when the active AppleClang frontend doesn't provide it.
// Fall back to a plain trap so JUCE targets can still compile.
#if !__has_builtin(__builtin_verbose_trap)
#define __builtin_verbose_trap(category, reason) __builtin_trap()
#endif
