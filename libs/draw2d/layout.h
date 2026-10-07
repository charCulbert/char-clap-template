#pragma once
#include "draw2d.h"
#include <clay.h>
#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// Flexbox-style layout with Clay (github.com/nicbarker/clay). Each frame,
// declare the interface as nested CLAY(...) boxes, sized in points. Layout
// draws what Clay asks for with Draw2D: boxes (each corner its own radius),
// borders (each side its own width), text (.fontId is a Draw2D font),
// images (Layout::image) and clipping (.clip, also for scrolling). Custom boxes (knobs, plots) come back
// with their rectangle in pixels, in drawing order. Clay does the rest:
// hit-testing (Clay_PointerOver), floating boxes for popups and tooltips,
// scroll containers (scroll with .clip = {.vertical = true,
// .childOffset = Clay_GetScrollOffset()}).
namespace gpu {

struct Rect
{
    float x, y, w, h;
};

class Layout
{
public:
    Layout();
    ~Layout();

    // Starts declaring a frame: the canvas in pixels, and pixels per point.
    void begin(Draw2D &draw, float width, float height, float scale);
    // The pointer, in pixels, for Clay_Hovered and Clay_PointerOver. Clay tests
    // it against the previous frame's layout. Call it from View::pointer.
    void pointer(float x, float y, bool down);
    // Wheel notches (positive away from the user) for the scroll container
    // under the pointer. Call it from View::pointer.
    void wheel(float notches);
    // Clay keeps pointers to text until end(); this keeps the characters alive.
    Clay_String text(std::string characters);
    // Marks a box as custom: pass the result as .custom = {.customData = ...}.
    void *custom(int kind, void *data = nullptr);
    // A Draw2D image for a box: pass the result as .image = {.imageData = ...}.
    // The box's backgroundColor alpha, if set, fades it.
    static void *image(int number) { return reinterpret_cast<void *>(intptr_t(number) + 1); }
    // Computes the layout, draws it, and calls onCustom for each custom box.
    void end(const std::function<void(int kind, void *data, Rect pixels)> &onCustom);

private:
    struct Custom { int kind; void *data; };
    std::vector<char> memory;
    Clay_Context *context = nullptr;
    Draw2D *draw = nullptr;
    float scale = 1;
    float pointerX = -1e9f, pointerY = -1e9f;
    bool pointerDown = false;
    float wheelNotches = 0;
    std::chrono::steady_clock::time_point lastFrame;
    std::deque<std::string> strings;
    std::deque<Custom> customs;
};

// Clay colors are 0-255.
inline Clay_Color clayColor(Color c) { return {c.r * 255, c.g * 255, c.b * 255, c.a * 255}; }

}
