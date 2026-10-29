#pragma once
#include "../core/messages.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The plugin's parameters as the interface sees them: filled from the "params"
// and "values" messages, and edited with the begin/change/end messages the
// plugin turns into CLAP gesture and value events.
namespace gpu
{

struct Param
{
    int id = -1;
    std::string name;
    double min = 0, max = 1, defaultValue = 0, value = 0;
    bool stepped = false;  // CLAP_PARAM_IS_STEPPED: whole values only, as CLAP truncates them (min and max whole too)
    bool dragging = false; // in a gesture
    int gestures = 0;      // nested begins: several controls (or keys mid-drag) can share one
    // The value at the middle of a control's travel, for a range that wants
    // more of it at one end (Hz, seconds). Only for the interface: CLAP
    // doesn't carry it. None, or one not inside (min, max): even travel.
    std::optional<double> mid;
};

class Params
{
public:
    // Handles "params" and "values"; returns false for other messages.
    bool receive (const core::Value& message);

    // Stays valid for the Params' life, through a new "params" list. By
    // name, or by the CLAP parameter id; nullptr for one not listed.
    Param* find (std::string_view name);
    Param* find (uint32_t id);
    void begin (Param& p);
    void change (Param& p, double value); // clamps to the range; truncates a stepped one's
    void end (Param& p);

    std::function<void (const core::Value&)> send; // set to the View's send

private:
    std::deque<Param> params; // only ever added to: a Param stays where it is
};

} // namespace gpu
