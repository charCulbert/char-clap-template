// The layout and the controls (libs/draw2d) without a GPU: synthetic pointer
// and key events in, element rectangles, values and parameter messages out.
// Draw2D records draws but never flushes them, having no device.
#include "../libs/draw2d/ui.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using gpu::Ui;
using gpu::View;

static int failures = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition), ++failures; \
    } while (0)

// One frame of a 400 x 300 point view at scale 1, its contents declared by declare.
template <typename F> static void frame(Ui &ui, F &&declare, const std::function<void(int, void *, gpu::Rect)> &onCustom = {})
{
    ui.begin(400, 300, 1);
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(10), .childGap = 10}})
    {
        declare();
    }
    ui.end(wgpu::RenderPassEncoder{}, onCustom);
}

static Clay_BoundingBox box(const char *kind, int index = 0)
{
    return Clay_GetElementData(Clay_GetElementIdWithIndex({false, int32_t(std::strlen(kind)), kind}, uint32_t(index))).boundingBox;
}
static void pointer(Ui &ui, View::Pointer type, float x, float y)
{
    ui.pointer({type, x, y});
}
static void clickAt(Ui &ui, Clay_BoundingBox b, const std::function<void()> &frameAfterEach)
{
    const float x = b.x + b.width / 2, y = b.y + b.height / 2;
    pointer(ui, View::Pointer::move, x, y);
    frameAfterEach();
    pointer(ui, View::Pointer::down, x, y);
    frameAfterEach();
    pointer(ui, View::Pointer::up, x, y);
    frameAfterEach();
}

static void layoutSizes()
{
    // Fixed, grow and padding come out exactly where Clay's rules put them.
    Ui ui;
    std::vector<gpu::Rect> boxes;
    frame(ui, [&] {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(20)}}, .custom = {.customData = ui.layout.custom(0)}}) {}
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20)}}, .custom = {.customData = ui.layout.custom(1)}}) {}
    }, [&](int, void *, gpu::Rect r) { boxes.push_back(r); });
    CHECK(boxes.size() == 2);
    CHECK(boxes[0].x == 10 && boxes[0].y == 10 && boxes[0].w == 50 && boxes[0].h == 20);
    CHECK(boxes[1].x == 70 && boxes[1].w == 320); // 400 - 10 - 50 - 10 - 10
}

static void buttonClicks()
{
    Ui ui;
    int clicks = 0;
    auto draw = [&] { frame(ui, [&] { if (ui.button("Go")) ++clicks; }); };
    draw();
    clickAt(ui, box("Go"), draw);
    CHECK(clicks == 1);
    // Pressed on it but released elsewhere: no click.
    const Clay_BoundingBox b = box("Go");
    pointer(ui, View::Pointer::down, b.x + 2, b.y + 2);
    draw();
    pointer(ui, View::Pointer::move, 300, 250);
    draw();
    pointer(ui, View::Pointer::up, 300, 250);
    draw();
    CHECK(clicks == 1);
}

static void toggleAndSegmented()
{
    Ui ui;
    bool on = false;
    int selected = 0;
    static const char *const items[] = {"A", "B", "C"};
    auto draw = [&] { frame(ui, [&] { ui.toggle("Sync", on); ui.segmented(items, 3, selected); }); };
    draw();
    clickAt(ui, box("Sync"), draw);
    CHECK(on);
    clickAt(ui, box("Segment", 2), draw);
    CHECK(selected == 2);
}

static void dropdownSelectsAndBlocks()
{
    Ui ui;
    int selected = 0, underPresses = 0;
    static const char *const items[] = {"Sine", "Saw", "Square"};
    // The dropdown, and a custom area under everything, as a plot would be:
    // the open menu must keep presses from it.
    auto draw = [&] {
        ui.begin(400, 300, 1);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(10)}})
        {
            ui.dropdown(items, 3, selected);
        }
        if (ui.area(&underPresses, 0, 0, 400, 300).pressed) ++underPresses;
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    clickAt(ui, box("Dropdown"), draw);
    const int pressesOpening = underPresses; // the area may see the press that opened it
    const Clay_BoundingBox item = box("DropdownItem", 2);
    CHECK(item.height > 0); // the menu is open
    clickAt(ui, item, draw);
    CHECK(selected == 2);
    CHECK(underPresses == pressesOpening);
}

static std::vector<std::string> sent;
static void setUpParams(gpu::Params &params)
{
    params.send = [](const core::Value &m) { sent.push_back(std::string(m["type"].text())); };
    core::Value::Array list;
    for (int i = 0; i < 2; ++i)
        list.push_back(core::Value::Map{{"id", double(i)}, {"name", i ? "B" : "A"}, {"min", 0.0}, {"max", 1.0}, {"default", 0.5}});
    params.receive(core::Value::Map{{"type", "params"}, {"params", list}});
}

static void knobDragAndKeys()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    auto draw = [&] {
        ui.begin(400, 300, 1);
        ui.knob(params, a, gpu::Rect{0, 0, 100, 140}, "%.2f");
        ui.knob(params, b, gpu::Rect{200, 0, 100, 140}, "%.2f");
        ui.slider(params, a, gpu::Rect{0, 200, 300, 40}, "%.2f"); // A again: must not take A's typing
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    sent.clear();
    // Drag up 100 pixels over 200 of travel: half the range.
    pointer(ui, View::Pointer::down, 50, 50);
    draw();
    pointer(ui, View::Pointer::move, 50, -50);
    draw();
    pointer(ui, View::Pointer::up, 50, -50);
    draw();
    CHECK(std::abs(a->value - 1.0) < 1e-9);
    CHECK(sent.size() >= 3 && sent.front() == "begin" && sent.back() == "end");
    // A clicked knob has focus; Tab moves it to the next; Up steps that one.
    ui.key(View::Key::tab, 0);
    draw();
    ui.key(View::Key::up, 0);
    draw();
    CHECK(std::abs(b->value - 0.51) < 1e-9);
    // Double-click resets.
    for (int i = 0; i < 2; ++i)
    {
        pointer(ui, View::Pointer::down, 50, 50);
        pointer(ui, View::Pointer::up, 50, 50);
    }
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    // A click on the value (under the dial) types one; Enter sets it.
    pointer(ui, View::Pointer::down, 50, 110);
    draw();
    pointer(ui, View::Pointer::up, 50, 110);
    draw();
    ui.key(View::Key::backspace, 0);
    ui.key(View::Key::backspace, 0);
    ui.key(View::Key::backspace, 0);
    ui.text("0.25");
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(std::abs(a->value - 0.25) < 1e-9);
}

static void textFieldEdits()
{
    Ui ui;
    std::string value = "x";
    int commits = 0;
    auto draw = [&] { frame(ui, [&] { if (ui.textField(value, 100)) ++commits; }); };
    draw();
    clickAt(ui, box("TextField"), draw);
    ui.text("yz");
    ui.key(View::Key::backspace, 0);
    draw();
    CHECK(value == "x"); // not until it's committed
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "xy" && commits == 1);
    // Text and keys apply in the order they came, and Backspace removes a
    // whole character (two bytes here).
    clickAt(ui, box("TextField"), draw);
    ui.text("é");
    ui.key(View::Key::backspace, 0);
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "xy" && commits == 1); // unchanged, so not a commit
    // Escape drops the edit.
    clickAt(ui, box("TextField"), draw);
    ui.text("q");
    ui.key(View::Key::escape, 0);
    draw();
    CHECK(value == "xy" && commits == 1);
}

static void pianoNotes()
{
    // C4 to B4, seven white keys 40 pixels wide.
    Ui ui;
    Ui::Notes notes;
    auto draw = [&] {
        ui.begin(400, 300, 1);
        notes = ui.piano(60, 71, gpu::Rect{0, 0, 280, 60});
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    pointer(ui, View::Pointer::down, 20, 50); // C, below the black keys
    draw();
    CHECK(notes.on == 60 && notes.off == -1);
    pointer(ui, View::Pointer::move, 60, 50); // slid to D
    draw();
    CHECK(notes.on == 62 && notes.off == 60);
    pointer(ui, View::Pointer::up, 60, 50);
    draw();
    CHECK(notes.on == -1 && notes.off == 62);
    pointer(ui, View::Pointer::down, 40, 10); // C sharp, on top of C and D
    draw();
    CHECK(notes.on == 61);
    pointer(ui, View::Pointer::up, 40, 10);
    draw();
    CHECK(notes.off == 61);
}

int main()
{
    layoutSizes();
    buttonClicks();
    toggleAndSegmented();
    dropdownSelectsAndBlocks();
    knobDragAndKeys();
    textFieldEdits();
    pianoNotes();
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
