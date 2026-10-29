// What the ui*.cpp files share; only they include it (and ui_test, for the
// knob law through a parameter's mid).
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

namespace gpu
{

inline constexpr unsigned keyBit (View::Key key)
{
    return 1u << unsigned (key);
}
// The keys that step a value: arrows, Page Up and Down, Home and End.
inline constexpr unsigned stepKeys =
    keyBit (View::Key::left) | keyBit (View::Key::right) | keyBit (View::Key::up) | keyBit (View::Key::down)
    | keyBit (View::Key::pageUp) | keyBit (View::Key::pageDown) | keyBit (View::Key::home) | keyBit (View::Key::end);

// A control's id for screen readers: its address, as it is for focus.
inline uint64_t accessId (const void* me)
{
    return uint64_t (reinterpret_cast<uintptr_t> (me));
}

// The travel of a parameter with a mid (gpu::Param): an exponential curve
// through min, mid and max, value = min + (max - min) * expm1 (u ln r) / (r - 1)
// at travel u, where r = ((1 - t) / t)^2 and t is mid's 0..1 place in the
// range. Unlike a power curve, it has a slope at both ends (no dead stretch
// at min), works from min 0 and on negative ranges, and with mid the geometric
// mean of min and max is the log law. Returns ln r, or 0 for even travel: no
// mid, one not inside (min, max), one at the middle, or a stepped parameter.
inline double midCurve (const Param& p)
{
    if (!p.mid || p.stepped || !(p.max > p.min))
        return 0;
    const double t = (*p.mid - p.min) / (p.max - p.min);
    if (!(t > 0 && t < 1) || std::abs (t - 0.5) < 1e-9)
        return 0;
    // A mid within 1e-12 of an end is as steep as doubles keep finite (r 1e24).
    const double inside = std::clamp (t, 1e-12, 1 - 1e-12);
    return 2 * std::log ((1 - inside) / inside);
}

// The curve's 0..1 place in the range at travel u, and back; k is midCurve's.
// Worked from the curve's shallow end, so expm1 never nears -1 (k < 0: mid
// past the middle, mirrored).
inline double curveValue (double u, double k)
{
    if (k < 0)
        return 1 - curveValue (1 - u, -k);
    return std::expm1 (u * k) / std::expm1 (k);
}

inline double curveTravel (double n, double k)
{
    n = std::clamp (n, 0.0, 1.0);
    if (k < 0)
        return 1 - curveTravel (1 - n, -k);
    return std::log1p (n * std::expm1 (k)) / k;
}

} // namespace gpu
