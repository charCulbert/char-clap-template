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
// images (Layout::image), shadows (Layout::shadow) and clipping (.clip, also
// for scrolling). Custom boxes (knobs, plots) come back
// with their rectangle in pixels, in drawing order. Clay does the rest:
// hit-testing (Clay_PointerOver), floating boxes for popups and tooltips,
// scroll containers (scroll with .clip = {.vertical = true,
// .childOffset = Clay_GetScrollOffset()}).
namespace gpu
{

struct Rect
{
    float x, y, w, h;
};

class Layout
{
public:
    // Room for maxElements boxes a frame (text runs count too). Clay's
    // default, 8192, takes 6 MB; a plugin's interface needs far fewer.
    explicit Layout (int maxElements = 2048);
    ~Layout();

    // Starts declaring a frame: the canvas in pixels, and pixels per point.
    void begin (Draw2D& draw, float width, float height, float scale);
    // The pointer, in pixels, for Clay_Hovered and Clay_PointerOver. Clay tests
    // it against the previous frame's layout. Call it from View::pointer.
    void pointer (float x, float y, bool down);
    // Wheel notches (positive away from the user) for the scroll container
    // under the pointer. Call it from View::pointer.
    void wheel (float notches);
    // Clay keeps pointers to text until end(); this keeps the characters alive.
    Clay_String text (std::string characters);
    // Marks a box as custom: pass the result as .custom = {.customData = ...}.
    void* custom (int kind, void* data = nullptr);
    // A soft shadow under a box with a background: pass the result as
    // .userData (which Layout keeps for this). Offset and blur in points.
    void* shadow (Color color, float blur = 8, float dx = 0, float dy = 2);
    // A Draw2D image for a box: pass the result as .image = {.imageData = ...}.
    // The box's backgroundColor alpha, if set, fades it.
    static void* image (int number)
    {
        return reinterpret_cast<void*> (intptr_t (number) + 1);
    }
    // Makes this layout's Clay context Clay's current one, which begin()
    // does: for asking Clay about the last frame between frames (from input
    // or a screen reader), when another instance's may be current.
    void activate() const
    {
        Clay_SetCurrentContext (context);
    }
    // Computes the layout, draws it, and calls onCustom for each custom box.
    void end (const std::function<void (int kind, void* data, Rect pixels)>& onCustom);
    // What the last end() laid out, in order: each element's id and box
    // (pixels), with its words for a line of text. Screen readers read it.
    struct Drawn
    {
        uint32_t id;
        Rect pixels;
        bool text = false;
        std::string words;
    };
    std::vector<Drawn> drawn;
    // The smallest size, in points, the last end()'s content fits: what its
    // boxes add up to at their least (fixed sizes, padding, gaps, the longest
    // word of each text), all of it but what scrolls or clips. Bigger than
    // the view when the content is cut off at its edge.
    float minimumWidth = 0, minimumHeight = 0;

private:
    struct Custom
    {
        int kind;
        void* data;
    };
    struct Shadow
    {
        Color color;
        float blur, dx, dy;
    };
    std::deque<Shadow> shadows;
    std::vector<char> memory;
    Clay_Context* context = nullptr;
    Draw2D* draw = nullptr;
    float scale = 1;
    float pointerX = -1e9f, pointerY = -1e9f;
    bool pointerDown = false;
    float wheelNotches = 0;
    std::chrono::steady_clock::time_point lastFrame;
    std::deque<std::string> strings;
    std::deque<Custom> customs;
};

// Clay colors are 0-255.
inline Clay_Color clayColor (Color c)
{
    return { c.r * 255, c.g * 255, c.b * 255, c.a * 255 };
}

} // namespace gpu
