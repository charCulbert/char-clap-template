#pragma once
#include "draw2d.h"
#include "params.h"
#include "view.h"

// Immediate-mode controls: call them from View::draw every frame; each draws
// itself, and a drag on it edits its parameter (with begin/change/end).
// Forward View::pointer to Ui::pointer. Sizes are in points (scaled here).
namespace gpu {

struct Theme
{
    Color text{0.90f, 0.93f, 0.95f}, dim{0.55f, 0.61f, 0.66f}, accent{0.37f, 0.71f, 0.64f};
    Color track{0.20f, 0.23f, 0.26f}, panel{0.11f, 0.13f, 0.15f, 0.92f}, edge{0.25f, 0.29f, 0.32f};
};

class Ui
{
public:
    Draw2D draw;
    Theme theme;

    void pointer(View::Pointer type, float x, float y);
    void begin(float width, float height, float scale); // starts a frame of controls
    void end(const wgpu::RenderPassEncoder &pass);

    // A rotary control; drag up/down to change. log: logarithmic mapping (min > 0).
    void knob(Params &params, Param *p, float x, float y, float size, const char *format, bool log = false);
    // A horizontal fader; drag left/right.
    void slider(Params &params, Param *p, float x, float y, float width, const char *format, bool log = false);
    void label(float x, float y, float size, std::string_view text, Color color);

    // A custom draggable region (pixels): reports press, drag and release on it.
    struct Area { bool pressed = false, held = false, released = false; float x = 0, y = 0; };
    Area area(const void *id, float x, float y, float w, float h);

    float s = 1; // pixels per point for this frame

private:
    // 0..1 position of a value, and back.
    static double toUnit(const Param &p, double v, bool log);
    static double fromUnit(const Param &p, double u, bool log);
    // Starts, continues or ends a drag on the control with rectangle x,y,w,h.
    // Returns true while that control is being dragged.
    bool drag(Params &params, Param &p, float x, float y, float w, float h, bool vertical, float travel, bool log);

    struct { float x = 0, y = 0; bool down = false, pressed = false, released = false; } mouse;
    const void *active = nullptr; // the control being dragged
    float grabX = 0, grabY = 0;
    double grabUnit = 0;
};

}
