#include "params.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace gpu
{

bool Params::receive (const core::Value& message)
{
    const auto type = message["type"].text();
    if (type == "params")
    {
        // Updated in place, by id (the plugin sends the list again when the
        // view is prepared again): the Param pointers a view holds, a control
        // mid-drag for one, stay good and its gesture goes on. One no longer
        // listed has its gesture ended and isn't found again.
        std::vector<bool> listed (params.size());
        if (auto list = std::get_if<core::Value::Array> (&message["params"].data))
            for (const auto& entry : *list)
            {
                const int id = int (entry["id"].number (-1));
                auto p = std::find_if (params.begin(), params.end(), [&] (const Param& q) { return q.id == id; });
                if (p == params.end())
                {
                    Param added;
                    added.id = id;
                    params.push_back (added);
                    listed.push_back (false);
                    p = std::prev (params.end());
                }
                listed[size_t (p - params.begin())] = true;
                p->name = entry["name"].text();
                p->min = entry["min"].number(), p->max = entry["max"].number (1),
                p->defaultValue = entry["default"].number();
                if (!p->dragging)
                    p->value = p->defaultValue;
            }
        for (size_t i = 0; i < params.size(); ++i)
            if (!listed[i])
            {
                if (params[i].gestures)
                    params[i].gestures = 1, end (params[i]);
                params[i].name.clear();
            }
        return true;
    }
    if (type == "values")
    {
        if (auto list = std::get_if<core::Value::Array> (&message["values"].data))
            for (auto& p : params)
                if (!p.dragging && size_t (p.id) < list->size())
                    p.value = (*list)[size_t (p.id)].number (p.value);
        return true;
    }
    return false;
}

Param* Params::find (std::string_view name)
{
    for (auto& p : params)
        if (p.name == name)
            return &p;
    return nullptr;
}

Param* Params::find (uint32_t id)
{
    for (auto& p : params)
        if (p.id == int64_t (id) && !p.name.empty()) // (one no longer listed has no name)
            return &p;
    return nullptr;
}

void Params::begin (Param& p)
{
    p.dragging = true;
    if (p.gestures++ == 0)
        send (core::Value::Map { { "type", "begin" }, { "id", p.id } });
}

void Params::change (Param& p, double value)
{
    if (std::isnan (value))
        return; // typed "nan": clamp would let it through
    p.value = std::clamp (value, p.min, p.max);
    send (core::Value::Map { { "type", "change" }, { "id", p.id }, { "value", p.value } });
}

void Params::end (Param& p)
{
    if (p.gestures == 0)
        return;
    p.dragging = --p.gestures > 0;
    if (!p.dragging)
        send (core::Value::Map { { "type", "end" }, { "id", p.id } });
}

} // namespace gpu
