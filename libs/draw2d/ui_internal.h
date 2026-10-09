// What the ui*.cpp files share; only they include it.
#pragma once
#include "ui.h"
#include <algorithm>
#include <functional>
#include <string_view>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace gpu {

inline constexpr unsigned keyBit(View::Key key) { return 1u << unsigned(key); }
// The keys that step a value: arrows, Page Up and Down, Home and End.
inline constexpr unsigned stepKeys = keyBit(View::Key::left) | keyBit(View::Key::right) | keyBit(View::Key::up)
                                   | keyBit(View::Key::down) | keyBit(View::Key::pageUp) | keyBit(View::Key::pageDown)
                                   | keyBit(View::Key::home) | keyBit(View::Key::end);

// A control's id for screen readers: its address, as it is for focus.
inline uint64_t accessId(const void *me) { return uint64_t(reinterpret_cast<uintptr_t>(me)); }

}
