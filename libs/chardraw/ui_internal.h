// What the ui*.cpp files share; only they include it (and ui_test, for the
// knob law).
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

// The law between a control's travel u (0..1) and a parameter's value, the
// same as Compost's parameter-scale.js, by its curve and mid (gpu::Param):
//
//   log, min > 0      value = min * (max / min)^(u^shape); shape puts the mid at
//                     u = 0.5, and is 1 (plain log) with no mid inside the range
//   linear            two straight lines meeting at the mid at u = 0.5, with a
//                     mid strictly inside the range; else even travel
//
// A log curve with min <= 0 is linear. A stepped parameter's travel is even.
inline bool hasLogTravel (const Param& p)
{
    return p.curve == Param::Curve::log && !p.stepped && p.min > 0 && p.max > p.min;
}

inline bool hasTwoLines (const Param& p)
{
    return p.mid && !p.stepped && *p.mid > p.min && *p.mid < p.max;
}

// shape = ln e / ln 0.5, where e is the mid's place between min and max on a
// log scale. 1 for no mid, or one outside the range or within 1e-12 of an end.
inline double logShape (const Param& p)
{
    if (!p.mid || !(*p.mid - p.min > 1e-12 && p.max - *p.mid > 1e-12))
        return 1;
    const double e = std::log (*p.mid / p.min) / std::log (p.max / p.min);
    if (!(e > 0 && e < 1))
        return 1;
    return std::max (1e-12, std::log (e) / std::log (0.5));
}

inline double curveValue (const Param& p, double u)
{
    u = std::clamp (u, 0.0, 1.0);
    if (hasLogTravel (p))
        return p.min * std::pow (p.max / p.min, std::pow (u, logShape (p)));
    if (hasTwoLines (p))
        return u <= 0.5 ? p.min + (*p.mid - p.min) * (u / 0.5) : *p.mid + (p.max - *p.mid) * ((u - 0.5) / 0.5);
    return p.min + (p.max - p.min) * u;
}

// A value outside the range counts as its nearest end, one not finite as min.
// Needs max > min.
inline double curveTravel (const Param& p, double v)
{
    v = std::isfinite (v) ? std::clamp (v, p.min, p.max) : p.min;
    if (hasLogTravel (p))
        return std::clamp (std::pow (std::log (v / p.min) / std::log (p.max / p.min), 1 / logShape (p)), 0.0, 1.0);
    if (hasTwoLines (p))
    {
        const double u =
            v <= *p.mid ? 0.5 * ((v - p.min) / (*p.mid - p.min)) : 0.5 + 0.5 * ((v - *p.mid) / (p.max - *p.mid));
        return std::clamp (u, 0.0, 1.0);
    }
    return (v - p.min) / (p.max - p.min);
}

} // namespace gpu
