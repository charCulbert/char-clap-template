#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gpu {

void Ui::pointer(View::Pointer type, float x, float y)
{
    mouse.x = x, mouse.y = y;
    if (type == View::Pointer::down) mouse.down = mouse.pressed = true;
    if (type == View::Pointer::up) mouse.down = false, mouse.released = true;
}

void Ui::begin(float width, float height, float scale)
{
    s = scale;
    draw.begin(width, height);
}

void Ui::end(const wgpu::RenderPassEncoder &pass)
{
    draw.flush(pass);
    mouse.pressed = mouse.released = false; // each press or release is seen by one frame
}

double Ui::toUnit(const Param &p, double v, bool log)
{
    if (p.max <= p.min) return 0;
    if (log && p.min > 0) return std::log(v / p.min) / std::log(p.max / p.min);
    return (v - p.min) / (p.max - p.min);
}

double Ui::fromUnit(const Param &p, double u, bool log)
{
    u = std::clamp(u, 0.0, 1.0);
    if (log && p.min > 0) return p.min * std::pow(p.max / p.min, u);
    return p.min + u * (p.max - p.min);
}

bool Ui::drag(Params &params, Param &p, float x, float y, float w, float h, bool vertical, float travel, bool log)
{
    if (mouse.pressed && !active && mouse.x >= x && mouse.x <= x + w && mouse.y >= y && mouse.y <= y + h)
    {
        active = &p;
        grabX = mouse.x, grabY = mouse.y, grabUnit = toUnit(p, p.value, log);
        params.begin(p);
    }
    if (active != &p) return false;
    const double moved = vertical ? (grabY - mouse.y) / travel : (mouse.x - grabX) / travel;
    const double value = fromUnit(p, grabUnit + moved, log);
    if (value != p.value) params.change(p, value);
    if (!mouse.down)
    {
        params.end(p);
        active = nullptr;
    }
    return true;
}

void Ui::knob(Params &params, Param *p, float x, float y, float size, const char *format, bool log)
{
    if (!p) return;
    const float r = size * s / 2, cx = x * s + r, cy = y * s + r;
    const bool dragging = drag(params, *p, cx - r, cy - r, 2 * r, 2 * r, true, 200 * s, log);
    // 270 degrees of travel, from 7:30 to 4:30.
    const float from = 3.927f, sweep = 4.712f, u = float(toUnit(*p, p->value, log));
    const float width = std::max(3.0f * s, r * 0.14f);
    draw.arc(cx, cy, r - width, width, from, from + sweep, theme.track);
    if (u > 0) draw.arc(cx, cy, r - width, width, from, from + sweep * u, theme.accent);
    const float angle = from + sweep * u, inner = r - 2.6f * width;
    draw.circle(cx, cy, inner, theme.panel, 1 * s, dragging ? theme.accent : theme.edge);
    draw.line(cx + std::sin(angle) * inner * 0.35f, cy - std::cos(angle) * inner * 0.35f,
              cx + std::sin(angle) * inner * 0.85f, cy - std::cos(angle) * inner * 0.85f, 2 * s, theme.text);
    char value[32];
    std::snprintf(value, sizeof(value), format, p->value);
    draw.text(cx, cy + r + 13 * s, 11 * s, value, theme.text, Draw2D::Align::center);
    draw.text(cx, cy + r + 28 * s, 11 * s, p->name, theme.dim, Draw2D::Align::center);
}

void Ui::slider(Params &params, Param *p, float x, float y, float width, const char *format, bool log)
{
    if (!p) return;
    const float px = x * s, py = y * s, w = width * s, h = 22 * s;
    const bool dragging = drag(params, *p, px, py, w, h, false, w, log);
    const float u = float(toUnit(*p, p->value, log)), track = py + 16 * s;
    draw.text(px, py + 8 * s, 11 * s, p->name, theme.dim);
    char value[32];
    std::snprintf(value, sizeof(value), format, p->value);
    draw.text(px + w, py + 8 * s, 10 * s, value, theme.text, Draw2D::Align::right);
    draw.rect(px, track - 2 * s, w, 4 * s, 2 * s, theme.track);
    draw.rect(px, track - 2 * s, w * u, 4 * s, 2 * s, theme.accent);
    draw.circle(px + w * u, track, (dragging ? 7.0f : 6.0f) * s, theme.text);
}

Ui::Area Ui::area(const void *id, float x, float y, float w, float h)
{
    Area result;
    if (mouse.pressed && !active && mouse.x >= x && mouse.x <= x + w && mouse.y >= y && mouse.y <= y + h)
        active = id, result.pressed = true;
    if (active != id) return result;
    result.held = true, result.x = mouse.x, result.y = mouse.y;
    if (!mouse.down) result.released = true, active = nullptr;
    return result;
}

void Ui::label(float x, float y, float size, std::string_view text, Color color)
{
    draw.text(x * s, y * s, size * s, text, color);
}

}
