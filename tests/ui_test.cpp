// The layout and the controls (libs/chardraw) without a GPU: synthetic pointer
// and key events in, element rectangles, values and parameter messages out.
// Draw2D records draws but never flushes them, having no device.
#include "../libs/chardraw/ui.h"
#include "../libs/chardraw/views.h"
#include "../libs/chardraw/dock.h"
#include <set>
#include <thread>
#include <chrono>
#include <clocale>
#include <cstring>
#include <iterator>
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
template <typename F> static void frame(Ui &ui, F &&declare)
{
    ui.begin(400, 300, 1);
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(10), .childGap = 10}})
    {
        declare();
    }
    ui.end(wgpu::RenderPassEncoder{});
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
        // (Made before CLAY(...): MSVC can't take a lambda there inside another lambda.)
        void *first = ui.paint([&](gpu::Rect r) { boxes.push_back(r); }), *second = ui.paint([&](gpu::Rect r) { boxes.push_back(r); });
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(20)}}, .custom = {.customData = first}}) {}
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20)}}, .custom = {.customData = second}}) {}
    });
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

// Two quick clicks on a button or toggle are two clicks, not one swallowed
// as a double-click: only knobs, sliders and fields give double-clicks a meaning.
static void quickClicksCount()
{
    Ui ui;
    int clicks = 0;
    bool on = false;
    auto draw = [&] {
        frame(ui, [&] {
            if (ui.button("Go")) ++clicks;
            ui.toggle("Sync", on);
        });
    };
    draw();
    clickAt(ui, box("Go"), draw);
    clickAt(ui, box("Go"), draw);
    CHECK(clicks == 2);
    clickAt(ui, box("Sync"), draw);
    clickAt(ui, box("Sync"), draw);
    CHECK(!on);
}

static void keyboardFocus()
{
    // Tab reaches buttons, toggles, segments and dropdowns, in order.
    Ui ui;
    int clicks = 0, selected = 0, shape = 0;
    bool on = false;
    static const char *const items[] = {"A", "B", "C"};
    auto draw = [&] {
        frame(ui, [&] {
            if (ui.button("Go")) ++clicks;
            ui.toggle("Sync", on);
            ui.segmented(items, 3, selected);
            ui.dropdown(items, 3, shape);
        });
    };
    draw();
    clickAt(ui, box("Go"), draw); // focus the button (and press it once)
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(clicks == 2);
    ui.key(View::Key::tab, 0);
    draw();
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(on);
    CHECK(ui.text(" ")); // Space flips it too, as HTML's checkbox
    draw();
    CHECK(!on);
    ui.text(" ");
    draw();
    CHECK(on);
    ui.key(View::Key::tab, 0);
    draw();
    ui.key(View::Key::right, 0);
    ui.key(View::Key::right, 0);
    draw();
    CHECK(selected == 2);
    ui.key(View::Key::right, 0); // wraps, as APG's radio group
    draw();
    CHECK(selected == 0);
    ui.key(View::Key::left, 0);
    draw();
    CHECK(selected == 2);
    ui.key(View::Key::tab, 0);
    draw();
    // The dropdown, as HTML's select: Down opens it on the choice, Down
    // moves the highlight, Enter chooses it.
    ui.key(View::Key::down, 0);
    draw();
    CHECK(shape == 0 && ui.accessibility()->nodes.size() > 0);
    ui.key(View::Key::down, 0);
    draw();
    CHECK(shape == 0); // only highlighted
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(shape == 1);
    // Escape closes it as it was.
    ui.key(View::Key::enter, 0);
    draw();
    ui.key(View::Key::down, 0);
    draw();
    ui.key(View::Key::escape, 0);
    draw();
    CHECK(shape == 1);
    // Space opens it and chooses; a letter jumps to its option.
    CHECK(ui.text(" "));
    draw();
    ui.key(View::Key::down, 0);
    draw();
    CHECK(ui.text(" "));
    draw();
    CHECK(shape == 2);
    CHECK(ui.text("a"));
    draw();
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(shape == 0);
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
    // Open, a press outside it closes it and goes no further (as a context
    // menu's): the area under it doesn't get it.
    auto expanded = [&] {
        for (const gpu::Accessible &n : ui.accessibility()->nodes)
            if (n.role == gpu::Accessible::Role::comboBox) return n.expanded == 1;
        return false;
    };
    clickAt(ui, box("Dropdown"), draw);
    CHECK(expanded());
    const int before = underPresses;
    clickAt(ui, {300, 250, 10, 10}, draw);
    CHECK(underPresses == before && !expanded() && selected == 2);
    clickAt(ui, {200, 250, 10, 10}, draw); // closed: the next one does (not where the last was: a double-click)
    CHECK(underPresses == before + 1);
}

// A dropdown, tabs and a segmented control with no items: drawn empty, and
// their keys do nothing (no out-of-range item, no division by zero).
static void noItems()
{
    Ui ui;
    int choice = 0, tab = 0, segment = 0;
    auto draw = [&] {
        frame(ui, [&] {
            ui.dropdown(nullptr, 0, choice);
            ui.tabs(nullptr, 0, tab);
            ui.segmented(nullptr, 0, segment);
        });
    };
    draw();
    for (int i = 0; i < 3; ++i)
    {
        ui.key(View::Key::tab, 0);
        draw();
        for (View::Key key : {View::Key::down, View::Key::up, View::Key::left, View::Key::right, View::Key::home, View::Key::end,
                              View::Key::enter})
            ui.key(key, 0);
        ui.text("a");
        ui.text(" ");
        draw();
    }
    clickAt(ui, box("Dropdown"), draw);
    CHECK(choice == 0 && tab == 0 && segment == 0 && box("DropdownMenu").height == 0);
}

// Sliders drag absolute (the value goes under the pointer, an upright one's
// over its track, not past it) or relative; a filmstrip knob drags as a
// knob; the resize grip resizes.
static void slidersAndGrip()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    float resizedW = 0, resizedH = 0;
    ui.resizeTo = [&](float w, float h) { resizedW = w, resizedH = h; };
    auto draw = [&] {
        ui.begin(400, 300, 1);
        ui.vslider(params, a, gpu::Rect{0, 0, 60, 200}, "%.2f");
        ui.slider(params, b, gpu::Rect{100, 0, 200, 40}, "%.2f");
        ui.imageKnob(params, b, 0, 64, gpu::Rect{100, 100, 64, 64});
        ui.resizeGrip(400, 300);
        ui.end(wgpu::RenderPassEncoder{});
    };
    auto drag = [&](float x0, float y0, float x1, float y1) {
        pointer(ui, View::Pointer::down, x0, y0), draw();
        pointer(ui, View::Pointer::move, x1, y1), draw();
        pointer(ui, View::Pointer::up, x1, y1), draw();
    };
    a->value = 0.25;
    draw();
    // The upright slider's track: 1.6 lines in from each end (13-point text).
    const float top = 1.6f * 13, bottom = 200 - 1.6f * 13, thumbY = bottom - 0.25f * (bottom - top);
    pointer(ui, View::Pointer::down, 30, thumbY); // on its thumb: it stays put
    draw();
    CHECK(std::abs(a->value - 0.25) < 1e-3);
    pointer(ui, View::Pointer::move, 30, top);
    draw();
    CHECK(std::abs(a->value - 1) < 1e-6);
    pointer(ui, View::Pointer::up, 30, top);
    draw();
    // Absolute, the default: a press puts the value where it is.
    drag(250, 30, 250, 30);
    CHECK(std::abs(b->value - 0.75) < 1e-6);
    // Relative: the value moves by how far the pointer goes, from where it was.
    ui.sliderDrag = Ui::Drag::relative;
    drag(150, 30, 170, 30);
    CHECK(std::abs(b->value - 0.85) < 1e-6);
    ui.sliderDrag = Ui::Drag::absolute;
    // The filmstrip knob: up 100 pixels over 200 of travel, half the range;
    // to screen readers an upright slider, as a knob.
    drag(132, 132, 132, 132 - 30);
    CHECK(std::abs(b->value - 1) < 1e-6);
    drag(140, 140, 140, 140 + 100); // (not where the last press was: that would be a double-click)
    CHECK(std::abs(b->value - 0.5) < 1e-6);
    int upright = 0;
    for (const gpu::Accessible &n : ui.accessibility()->nodes) upright += n.role == gpu::Accessible::Role::slider && n.orientation == 2;
    CHECK(upright == 2); // the upright slider and the filmstrip knob
    // The grip, in the corner: dragged, it asks for the size under the pointer
    // (less where in the grip it was held).
    drag(395, 295, 495, 345);
    CHECK(resizedW == 500 && resizedH == 350);
}

// Laid out, a slider grows to its row's width and drags over it.
static void laidOutSlider()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    auto draw = [&] { frame(ui, [&] { ui.slider(params, a, "%.2f"); }); };
    draw();
    draw();
    const Clay_BoundingBox b = box("Slider", int(a->id));
    CHECK(b.width == 380 && b.height > 0);
    clickAt(ui, {b.x + b.width / 10, b.y + b.height - 6, 0, 0}, draw); // in the track, a tenth of the way along
    CHECK(std::abs(a->value - 0.1) < 1e-3);
}

// A toggle and segmented control of parameters send a whole gesture per
// click and follow the parameter's value.
static void parameterToggleAndSegmented()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params); // A and B: 0..1
    gpu::Param *a = params.find("A"), *b = params.find("B");
    static const char *const items[] = {"Off", "On"};
    auto draw = [&] { frame(ui, [&] { ui.toggle(params, a, "Sync"); ui.segmented(params, b, items, 2); }); };
    a->value = 0, b->value = 0;
    draw();
    sent.clear();
    clickAt(ui, box("Sync", a->id), draw);
    CHECK(a->value == 1);
    CHECK(sent.size() == 3 && sent[0] == "begin" && sent[1] == "change" && sent[2] == "end");
    clickAt(ui, box("Segment", b->id * 64 + 1), draw);
    CHECK(b->value == 1);
    b->value = 0; // the host changed it: the control shows that
    draw();
    clickAt(ui, box("Sync", a->id), draw);
    CHECK(a->value == 0);
    // A screen reader hears the segmented control by its parameter's name.
    bool named = false;
    for (const gpu::Accessible &n : ui.accessibility()->nodes) named |= n.role == gpu::Accessible::Role::radioGroup && n.name == "B";
    CHECK(named);
}

// Controls describe themselves for screen readers, and take their requests.
static void screenReader()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params); // A and B: 0..1
    gpu::Param *a = params.find("A");
    bool on = false;
    int clicks = 0;
    auto draw = [&] {
        frame(ui, [&] {
            ui.knob(params, a, "%.2f");
            ui.toggle("Sync", on);
            if (ui.button("Go")) ++clicks;
        });
    };
    draw();
    const gpu::Accessibility *tree = ui.accessibility();
    auto find = [&](const char *name) -> const gpu::Accessible * {
        for (const gpu::Accessible &n : tree->nodes)
            if (n.name == name) return &n;
        return nullptr;
    };
    const gpu::Accessible *knob = find("A"), *sync = find("Sync"), *go = find("Go");
    CHECK(knob && knob->role == gpu::Accessible::Role::slider && knob->value == "0.50" && knob->max == 1);
    CHECK(sync && sync->role == gpu::Accessible::Role::checkBox && sync->checked == 0);
    CHECK(go && go->role == gpu::Accessible::Role::button && go->width > 0);
    CHECK(tree->nodes.front().children.size() == 3);
    const uint64_t knobId = knob->id, syncId = sync->id, goId = go->id;
    const unsigned version = tree->version;
    ui.accessAction(knobId, gpu::Accessible::setValue, 0.25);
    ui.accessAction(syncId, gpu::Accessible::click, 0);
    draw();
    CHECK(a->value == 0.25 && on && tree->version != version);
    ui.accessAction(goId, gpu::Accessible::click, 0);
    draw();
    CHECK(clicks == 1);
    ui.accessAction(knobId, gpu::Accessible::increment, 0);
    draw();
    CHECK(a->value > 0.25);
}

// What a screen reader reads, as a page's: layout order, the captions, and
// the names, descriptions and omissions set for it.
static void screenReaderReading()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    int shape = 0;
    static const char *const shapes[] = {"Sine", "Saw"};
    auto draw = [&] {
        frame(ui, [&] {
            ui.knob(params, a, "%.2f"); // described as Clay draws it, after the others
            ui.button("Go");
            ui.tooltip("Starts it");
            ui.dropdown(shapes, 2, shape);
            ui.label("Shape");
            CLAY_TEXT(CLAY_STRING("Hello caption"), CLAY_TEXT_CONFIG({.fontSize = 13}));
            const Clay_ElementId deco = CLAY_IDI("Decoration", 0);
            CLAY({.id = deco}) { CLAY_TEXT(CLAY_STRING("ornament"), CLAY_TEXT_CONFIG({.fontSize = 13})); }
            ui.hidden(deco);
        });
    };
    draw();
    const gpu::Accessibility *tree = ui.accessibility();
    std::vector<std::string> read;
    for (uint64_t id : tree->nodes.front().children)
        for (const gpu::Accessible &n : tree->nodes)
            if (n.id == id) read.push_back(n.role == gpu::Accessible::Role::label ? n.value : n.name);
    CHECK(read.size() == 4 && read[0] == "A" && read[1] == "Go" && read[2] == "Shape" && read[3] == "Hello caption");
    for (const gpu::Accessible &n : tree->nodes)
    {
        if (n.name == "Go") CHECK(n.description == "Starts it");
        if (n.name == "Shape") CHECK(n.role == gpu::Accessible::Role::comboBox && n.value == "Sine");
        CHECK(n.value != "ornament");
    }
    // Announced: read out once, then cleared (not left on the page), so the
    // same words again are read again.
    auto live = [&] {
        for (const gpu::Accessible &n : tree->nodes)
            if (n.live) return n.value;
        return std::string("(none)");
    };
    ui.announce("Preset loaded");
    draw();
    CHECK(live() == "Preset loaded" && ui.animating());
    draw();
    CHECK(live().empty());
    ui.announce("Preset loaded");
    draw();
    CHECK(live() == "Preset loaded");
    ui.announce("Preset loaded"); // straight after: it waits for the clearing
    draw();
    CHECK(live().empty());
    draw();
    CHECK(live() == "Preset loaded");
}

// A screen reader focusing part of a control (a radio of a segmented
// control, a tab, a dropdown's option, an XY pad's axis): the keys then go
// to the control it's part of.
static void screenReaderFocusesParts()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    static const char *const items[] = {"One", "Two", "Three"};
    int segment = 0, tab = 0, choice = 0;
    auto draw = [&] {
        frame(ui, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                ui.segmented(items, 3, segment);
                ui.label("Segments");
                ui.tabs(items, 3, tab);
                ui.label("Tabs");
                ui.dropdown(items, 3, choice);
                ui.label("Choice");
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(100)}}}) { ui.xyPad(params, a, b); }
            }
        });
    };
    draw();
    draw();
    auto node = [&](gpu::Accessible::Role role, const std::string &name) -> uint64_t {
        for (const gpu::Accessible &n : ui.accessibility()->nodes)
            if (n.role == role && n.name == name) return n.id;
        return 0;
    };
    using Role = gpu::Accessible::Role;
    ui.accessAction(node(Role::radioButton, "Two"), gpu::Accessible::focus, 0);
    draw();
    draw(); // (the frame it asks for, as the presenter draws it)
    ui.key(View::Key::right, 0);
    draw();
    CHECK(segment == 1); // the group has the keys (focus was on One's group, with One checked)
    ui.accessAction(node(Role::tab, "Three"), gpu::Accessible::focus, 0);
    draw();
    draw();
    CHECK(ui.key(View::Key::left, 0));
    draw();
    CHECK(tab == 2); // wrapped from the first
    // The dropdown opened, and a screen reader on one of its options.
    ui.accessAction(node(Role::comboBox, "Choice"), gpu::Accessible::expand, 0);
    draw();
    draw();
    ui.accessAction(node(Role::option, "Three"), gpu::Accessible::focus, 0);
    draw();
    draw();
    CHECK(ui.key(View::Key::down, 0)); // the dropdown's keys: the highlight moves
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(choice == 1);
    const double before = a->value;
    ui.accessAction(node(Role::slider, "A"), gpu::Accessible::focus, 0); // the pad's x axis
    draw();
    draw();
    CHECK(ui.key(View::Key::right, 0));
    draw();
    CHECK(a->value > before);
}

// Tab reaches a number box after a knob, and its keys step it.
static void numberBoxTabs()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    auto draw = [&] { frame(ui, [&] { ui.knob(params, a, "%.2f"); ui.numberBox(params, b, "%.2f"); }); };
    draw();
    ui.key(View::Key::tab, 0); // the knob
    draw();
    ui.key(View::Key::tab, 0); // the number box
    draw();
    const double before = b->value;
    CHECK(ui.key(View::Key::up, 0));
    draw();
    CHECK(b->value > before && a->value == a->defaultValue);
}

// Everything is reachable with Tab (WCAG 2.1.1): a text field starts
// editing, and Tab out of it keeps the edit; a drawer opens with Enter; an
// XY pad moves with the arrows.
static void keyboardReachesEverything()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    std::string name = "One";
    bool open = false;
    auto draw = [&] {
        frame(ui, [&] {
            ui.textField(name);
            ui.drawer("More", open);
            ui.xyPad(params, a, b, gpu::Rect{200, 150, 100, 100});
        });
    };
    draw();
    ui.key(View::Key::tab, 0); // into the field: it edits from the next frame
    draw();
    draw();
    CHECK(ui.editingText());
    ui.text("Two"); // replaces the selection
    draw();
    ui.key(View::Key::tab, 0); // to the drawer, keeping the edit
    draw();
    draw();
    CHECK(!ui.editingText() && name == "Two");
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(open);
    ui.key(View::Key::tab, 0); // to the pad
    draw();
    const double x = a->value;
    ui.key(View::Key::right, 0);
    draw();
    CHECK(a->value > x);
}

// As compost's: a double-click resets, on its release; a double-click
// that drags is a fine drag, and resets nothing.
static void doubleClickOrFineDrag()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    auto draw = [&] {
        frame(ui, [&] { ui.knob(params, a, "%.2f"); ui.numberBox(params, b, "%.2f"); });
    };
    draw();
    auto twice = [&](const Clay_BoundingBox &box, float dx, float dy) {
        const float x = box.x + box.width / 2, y = box.y + box.height / 3;
        pointer(ui, View::Pointer::down, x, y);
        draw();
        pointer(ui, View::Pointer::up, x, y);
        draw();
        pointer(ui, View::Pointer::down, x, y);
        draw();
        pointer(ui, View::Pointer::move, x + dx, y + dy);
        draw();
        pointer(ui, View::Pointer::up, x + dx, y + dy);
        draw();
    };
    a->value = 0.8, b->value = 0.8;
    twice(box("Knob", a->id), 0, 0);
    CHECK(a->value == a->defaultValue);
    twice(box("Knob", a->id), 0, -40); // up 40 points: a tenth of a normal drag's change
    const double knobFine = a->value - a->defaultValue;
    CHECK(knobFine > 0 && knobFine < 0.03);
    twice(box("Number", b->id), 0, 0);
    CHECK(b->value == b->defaultValue);
    twice(box("Number", b->id), 36, 0); // a fifth of 180 points, at a tenth
    CHECK(std::abs(b->value - (b->defaultValue + 0.02)) < 0.005);
}

// A split's divider: dragged, moved with the keys, collapsed and restored
// with Enter, and a splitter for screen readers.
static void splitPanes()
{
    Ui ui;
    float size = 150;
    auto draw = [&] {
        frame(ui, [&] { ui.split("Panes", size, [] {}, [] {}, {.min = 50, .secondMin = 60}); });
    };
    draw();
    draw(); // the second frame knows the split's box
    const Clay_BoundingBox divider = box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Panes"), 0).id);
    CHECK(divider.width == 7 && std::abs(divider.x - (10 + 150)) < 1);
    const float x = divider.x + 3, y = divider.y + 20;
    pointer(ui, View::Pointer::down, x, y);
    draw();
    pointer(ui, View::Pointer::move, x + 40, y);
    draw();
    pointer(ui, View::Pointer::up, x + 40, y);
    draw();
    CHECK(std::abs(size - 190) < 0.5f);
    pointer(ui, View::Pointer::down, x + 40, y); // to the far right: stops leaving the second area 60
    draw();
    pointer(ui, View::Pointer::move, 2000, y);
    draw();
    pointer(ui, View::Pointer::up, 2000, y);
    draw();
    CHECK(std::abs(size - (380 - 7 - 60)) < 0.5f);
    ui.key(View::Key::left, 0); // focused by the press: 16 points
    draw();
    CHECK(std::abs(size - (380 - 7 - 60 - 16)) < 0.5f);
    ui.key(View::Key::enter, 0); // collapse
    draw();
    CHECK(size == 50);
    ui.key(View::Key::enter, 0); // restore
    draw();
    CHECK(std::abs(size - (380 - 7 - 60 - 16)) < 0.5f);
    ui.key(View::Key::left, View::shift); // Shift: a point (the key's own Shift, not the pointer's)
    draw();
    CHECK(std::abs(size - (380 - 7 - 60 - 17)) < 0.5f);
    const gpu::Accessible *splitter = nullptr;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::splitter) splitter = &n;
    CHECK(splitter && splitter->name == "Panes" && splitter->number == size && splitter->min == 50);
    ui.accessAction(splitter->id, gpu::Accessible::setValue, 120);
    draw();
    CHECK(size == 120);

    // Side by side in a row that grows: dragged far right, the section stays
    // the row's width, and the second area keeps its minimum.
    float wide = 100;
    auto row = [&] {
        frame(ui, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(100)}}})
            {
                ui.split("Row", wide, [] {}, [] {}, {.min = 20, .secondMin = 40});
            }
            CLAY({.id = CLAY_IDI("Neighbour", 0), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(100)}}}) {} // shares the row
        });
    };
    row();
    row();
    const Clay_BoundingBox rowBox = box("Row");
    const Clay_BoundingBox rowLine = box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Row"), 0).id);
    pointer(ui, View::Pointer::down, rowLine.x + 3, rowLine.y + 50);
    row();
    for (float x = rowLine.x + 20; x < 2000; x += 40)
    {
        pointer(ui, View::Pointer::move, x, rowLine.y + 50);
        row();
    }
    pointer(ui, View::Pointer::up, 2000, rowLine.y + 50);
    row();
    row();
    CHECK(box("Row").width == rowBox.width && box("Neighbour").width == rowBox.width); // neither took the other's room
    CHECK(std::abs(wide - (rowBox.width - 7 - 40)) < 0.5f);

    // Stacked, in a box 200 points tall: dragged far down, the second area
    // keeps its 30 points; it can't be pushed out of view.
    float top = 60;
    auto stacked = [&] {
        frame(ui, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(300), CLAY_SIZING_FIXED(200)}}})
            {
                ui.split("Stack", top, [] {}, [] {}, {.stacked = true, .min = 20, .secondMin = 30});
            }
        });
    };
    stacked();
    stacked();
    const Clay_BoundingBox line = box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Stack"), 0).id);
    pointer(ui, View::Pointer::down, line.x + 50, line.y + 3);
    stacked();
    for (float y = line.y + 20; y < 900; y += 40) // a slow drag, frame by frame
    {
        pointer(ui, View::Pointer::move, line.x + 50, y);
        stacked();
    }
    pointer(ui, View::Pointer::up, line.x + 50, 900);
    stacked();
    stacked();
    CHECK(std::abs(top - (200 - 7 - 30)) < 0.5f);
}

// A window: dragged by its header and kept inside the view, resized from
// its corner within its limits, in front when pressed; nothing under it
// takes the pointer; its header's keys; a dialog for screen readers.
// Every node but the root is some node's child, as AccessKit requires.
static bool everyNodeHasParent(const Ui &ui)
{
    const gpu::Accessibility *tree = ui.accessibility();
    if (!tree) return true;
    std::set<uint64_t> children;
    for (const gpu::Accessible &n : tree->nodes) children.insert(n.children.begin(), n.children.end());
    for (size_t i = 1; i < tree->nodes.size(); ++i)
        if (!children.count(tree->nodes[i].id)) return false;
    return true;
}

static void windows()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    gpu::Rect one{40, 40, 200, 150}, two{120, 80, 200, 150};
    bool openOne = true, openTwo = true, clicked = false;
    auto draw = [&] {
        frame(ui, [&] {
            ui.knob(params, a, "%.2f"); // under the windows
            // On the page, under where Two ends up (as the gallery's text is under its notes).
            CLAY({.floating = {.offset = {two.x + 20, two.y + 50}, .attachTo = CLAY_ATTACH_TO_ROOT}}) { ui.button("Under"); }
            ui.window("One", one, openOne, [&] { if (ui.button("Inside")) clicked = true; });
            ui.window("Two", two, openTwo, [] { CLAY_TEXT(CLAY_STRING("Words"), CLAY_TEXT_CONFIG({.fontSize = 13})); },
                      {.minWidth = 160, .minHeight = 100});
        });
        CHECK(everyNodeHasParent(ui)); // the frame a window closes in, too
    };
    draw();
    draw();
    const Clay_BoundingBox header = box("WindowHeader", Clay_GetElementIdWithIndex(CLAY_STRING("Two"), 0).id);
    // Dragged by the header, and not past the view's edge.
    pointer(ui, View::Pointer::down, header.x + 20, header.y + 5);
    draw();
    pointer(ui, View::Pointer::move, header.x + 60, header.y + 25);
    draw();
    CHECK(std::abs(two.x - 160) < 0.5f && std::abs(two.y - 100) < 0.5f);
    pointer(ui, View::Pointer::move, header.x + 2000, header.y + 25);
    draw();
    CHECK(std::abs(two.x - (400 - 200)) < 0.5f);
    pointer(ui, View::Pointer::up, header.x + 2000, header.y + 25);
    draw();
    // Resized from its corner, no smaller than its minimum.
    const Clay_BoundingBox grip = box("WindowGrip", Clay_GetElementIdWithIndex(CLAY_STRING("Two"), 0).id);
    pointer(ui, View::Pointer::down, grip.x + 6, grip.y + 6);
    draw();
    pointer(ui, View::Pointer::move, grip.x - 500, grip.y - 500);
    draw();
    pointer(ui, View::Pointer::up, grip.x - 500, grip.y - 500);
    draw();
    CHECK(two.w == 160 && two.h == 100);
    // A press on One brings it to the front; a press on a window doesn't
    // reach the knob under it.
    const float knobValue = float(a->value);
    pointer(ui, View::Pointer::down, 50, 60);
    draw();
    pointer(ui, View::Pointer::move, 50, 20);
    draw();
    pointer(ui, View::Pointer::up, 50, 20);
    draw();
    CHECK(a->value == knobValue);
    // The button in it works.
    clickAt(ui, box("Inside"), draw);
    CHECK(clicked);
    // For screen readers: a dialog named by its title, with its button inside.
    const gpu::Accessible *dialog = nullptr;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::dialog && n.name == "One") dialog = &n;
    CHECK(dialog && dialog->children.size() == 2); // its close button and "Inside"
    // Its header's keys: the arrows move it, Alt and the arrows resize it,
    // Escape closes it.
    ui.accessAction(dialog->id, gpu::Accessible::focus, 0);
    const gpu::Rect before = one;
    ui.key(View::Key::right, 0);
    draw();
    CHECK(one.x == before.x + 16 && one.w == before.w);
    ui.key(View::Key::down, View::alt);
    draw();
    CHECK(one.h == before.h + 16 && one.y == before.y);
    ui.key(View::Key::escape, 0);
    draw();
    CHECK(!openOne);
    // The close button closes it.
    clickAt(ui, box("WindowClose", Clay_GetElementIdWithIndex(CLAY_STRING("Two"), 0).id), draw);
    CHECK(!openTwo);
    draw(); // the frame after: the page under it is the page's again
}

// Whether a node named `parent` has one named `child` among its children.
static bool hasChild(const Ui &ui, const std::string &parent, const std::string &child)
{
    for (const gpu::Accessible &p : ui.accessibility()->nodes)
        if (p.name == parent)
            for (uint64_t id : p.children)
                for (const gpu::Accessible &c : ui.accessibility()->nodes)
                    if (c.id == id && c.name == child) return true;
    return false;
}

// A window declared inside an id scope (as a widget of one's own makes
// them): what's in it is still its dialog's.
static void windowInScope()
{
    Ui ui;
    gpu::Rect frame1{40, 40, 200, 150};
    bool open = true;
    int scope = 0;
    auto draw = [&] {
        frame(ui, [&] {
            ui.pushId(&scope);
            ui.window("Scoped", frame1, open, [&] { ui.button("Inside"); });
            ui.popId();
        });
        CHECK(everyNodeHasParent(ui));
    };
    draw();
    draw();
    CHECK(hasChild(ui, "Scoped", "Inside") && hasChild(ui, "Scoped", "Close Scoped"));
}

// Tabs: a click shows one; focused, the arrows wrap, Home and End go to the
// ends, and a key with Alt is the caller's; screen readers get a tab list.
static void tabStrip()
{
    Ui ui;
    static const char *const names[] = {"Mix", "Edit", "Notes"};
    int shown = 0;
    Ui::Tabs last;
    auto draw = [&] { frame(ui, [&] { last = ui.tabs(names, 3, shown); }); };
    draw();
    draw();
    const Clay_BoundingBox second = box("Tab", 1);
    pointer(ui, View::Pointer::down, second.x + 5, second.y + 5);
    draw();
    CHECK(shown == 0); // not on the press: on letting go over it
    pointer(ui, View::Pointer::up, second.x + 5, second.y + 5);
    draw();
    CHECK(shown == 1);
    const Clay_BoundingBox third = box("Tab", 2);
    pointer(ui, View::Pointer::down, third.x + 5, third.y + 5);
    draw();
    pointer(ui, View::Pointer::move, 300, 250); // let go elsewhere: nothing chosen
    draw();
    pointer(ui, View::Pointer::up, 300, 250);
    draw();
    CHECK(shown == 1);
    auto press = [&](View::Key key, unsigned modifiers = 0) { ui.key(key, modifiers); draw(); };
    press(View::Key::right);
    CHECK(shown == 2);
    press(View::Key::right);
    CHECK(shown == 0); // wrapped
    press(View::Key::end);
    CHECK(shown == 2);
    press(View::Key::home);
    CHECK(shown == 0);
    ui.key(View::Key::left, View::alt);
    frame(ui, [&] { last = ui.tabs(names, 3, shown); });
    CHECK(last.altKey && last.key == View::Key::left && shown == 0);
    const gpu::Accessible *list = nullptr;
    int tabs = 0, selected = 0;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
    {
        if (n.role == gpu::Accessible::Role::tabList) list = &n;
        if (n.role == gpu::Accessible::Role::tab) ++tabs, selected += n.selected && n.name == "Mix";
    }
    CHECK(list && list->children.size() == 3 && tabs == 3 && selected == 1);
}

// Dockable windows, as ImGui's: each panel starts as a window; a window let
// go on another's middle target joins it as tabs (no title then: its tab bar
// is), on a side splits it; the bar's empty part moves it; a tab moves along
// the bar, or out of it into a window of its own. A dock space takes them
// too, on its groups' targets and its edges; Alt and Enter pull a panel out;
// closing a lone window docks it into the space.
static void dockableWindows()
{
    Ui ui;
    gpu::Dock dock;
    bool withSpace = false;
    const std::vector<gpu::Dock::Panel> panels = {{"Mix", [&] { ui.button("Solo"); }}, {"Edit", [] {}}, {"Notes", [] {}}};
    auto draw = [&] {
        ui.begin(800, 600, 1);
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600)}}})
        {
            if (withSpace)
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(400), CLAY_SIZING_FIXED(300)}}}) { dock.space(ui, "Main", panels); }
        }
        dock.windows(ui, panels);
        ui.end(wgpu::RenderPassEncoder{});
        CHECK(everyNodeHasParent(ui));
    };
    using Tabs = std::vector<std::string>;
    gpu::Dock::Layout &layout = dock.layout;
    auto press = [&](float x, float y) { pointer(ui, View::Pointer::move, x, y); draw(); pointer(ui, View::Pointer::down, x, y); draw(); };
    auto moveTo = [&](float x, float y) { pointer(ui, View::Pointer::move, x, y); draw(); draw(); };
    auto release = [&](float x, float y) { pointer(ui, View::Pointer::up, x, y); draw(); draw(); };
    auto drag = [&](float x0, float y0, float x1, float y1) { press(x0, y0); moveTo((x0 + x1) / 2, (y0 + y1) / 2); moveTo(x1, y1); release(x1, y1); };
    // Its elements, found inside its id scope.
    auto in = [&](const char *name, int index = 0) { return Ui::idIn(&dock, name, index); };
    auto at = [](Clay_ElementId id) { return Clay_GetElementData(id).boundingBox; };
    auto title = [&](const char *name) { return at(in("WindowHeader", int(in(name).id))); };
    dock.options.dockIntoWindows = true, dock.options.popOut = true; // ImGui's way, and the button
    // Told once for each edit, to save it: once a drag ends, or for the key.
    int changes = 0;
    dock.changed = [&] { ++changes; };
    auto once = [&] { return std::exchange(changes, 0) == 1; };
    draw();
    CHECK(layout.windows.size() == 3 && layout.space.tabs.empty());
    layout.windows[0].frame = {20, 20, 240, 180}, layout.windows[1].frame = {300, 20, 240, 180}, layout.windows[2].frame = {20, 300, 240, 180};
    draw();
    draw();
    changes = 0;

    // Edit's title onto the middle of Mix's window: one window of tabs.
    Clay_BoundingBox t = title("Edit");
    drag(t.x + 30, t.y + 6, 20 + 120, 20 + 90);
    CHECK(layout.windows.size() == 2 && layout.windows[0].root.tabs == (Tabs{"Mix", "Edit"}));
    CHECK(once()); // moved, then docked: one edit
    CHECK(hasChild(ui, "Mix, Edit", "Panels")); // what's in a dock's window (its tab list) is its dialog's
    auto popOuts = [&] {
        int n = 0;
        for (const gpu::Accessible &node : ui.accessibility()->nodes) n += node.name.rfind("Pop out ", 0) == 0;
        return n;
    };
    CHECK(popOuts() == 1); // its tab bar's pop-out button
    dock.options.popOut = false;
    draw();
    draw();
    CHECK(popOuts() == 0);
    dock.options.popOut = true;
    draw();
    draw();
    // Notes' title near that window's right edge: a split in it.
    const gpu::Rect &both = layout.windows[0].frame;
    const Clay_BoundingBox g = at(in("DockGroup", 0)); // its group, under the bar
    t = title("Notes");
    drag(t.x + 30, t.y + 6, g.x + g.width - 10, g.y + g.height / 2);
    CHECK(layout.windows.size() == 1 && layout.windows[0].root.children.size() == 2 && !layout.windows[0].root.stacked);
    CHECK(layout.windows[0].root.children.size() == 2 && layout.windows[0].root.children[1].tabs == Tabs{"Notes"});
    CHECK(std::abs(layout.windows[0].root.size - (g.width / 2 - 7)) < 1); // the half the outline showed, on the right
    CHECK(once());

    // The bar's empty part moves the window.
    const float x0 = both.x;
    const Clay_BoundingBox bar = at(in("TabList", 0));
    drag(bar.x + bar.width - 6, bar.y + 6, bar.x + bar.width - 6 + 50, bar.y + 6 + 30);
    CHECK(std::abs(layout.windows[0].frame.x - (x0 + 50)) < 1);
    CHECK(once());

    // Along the bar: Mix onto Edit's place. Out of it: Edit in a window of its own.
    Clay_BoundingBox mix = at(in("Tab", 0)), edit = at(in("Tab", 1));
    drag(mix.x + mix.width / 2, mix.y + 6, edit.x + edit.width / 2, edit.y + 6);
    CHECK(layout.windows[0].root.children.size() == 2 && layout.windows[0].root.children[0].tabs == (Tabs{"Edit", "Mix"}));
    CHECK(once()); // a tab moved along its bar
    edit = at(in("Tab", 0));
    drag(edit.x + edit.width / 2, edit.y + 6, 450, 350); // let go on nothing: it stays where it's carried
    CHECK(layout.windows.size() == 2 && layout.windows[1].root.tabs == Tabs{"Edit"});
    CHECK(std::abs(layout.windows[1].frame.x - (450 - 60)) < 1);
    CHECK(once()); // pulled out and carried
    // A split dragged: once.
    const gpu::Accessible *divider = nullptr;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::splitter) divider = &n;
    CHECK(divider != nullptr);
    if (divider)
    {
        const float before = layout.windows[0].root.size, dx = divider->x + divider->width / 2, dy = divider->y + divider->height / 2;
        drag(dx, dy, dx - 20, dy);
        CHECK(std::abs(layout.windows[0].root.size - (before - 20)) < 1 && once());
    }
    // A lone window resized by its corner, then moved by its title: once each.
    const Clay_BoundingBox corner = at(in("WindowGrip", int(in("Edit").id)));
    drag(corner.x + 6, corner.y + 6, corner.x + 26, corner.y + 16);
    CHECK(layout.windows[1].frame.w == 300 + 20 && once());
    t = title("Edit");
    drag(t.x + 30, t.y + 6, t.x + 10, t.y + 26);
    CHECK(std::abs(layout.windows[1].frame.x - (450 - 60 - 20)) < 1 && once());

    // A dock space: Edit's title onto its middle, then Alt and Enter pull it out again.
    withSpace = true;
    layout.windows[0].frame = {450, 20, 300, 220}; // clear of the space
    draw();
    draw();
    t = title("Edit");
    drag(t.x + 30, t.y + 6, 200, 150); // the middle of the 400 x 300 space
    CHECK(layout.windows.size() == 1 && layout.space.tabs == Tabs{"Edit"});
    CHECK(once());
    clickAt(ui, at(in("Tab", 0)), draw); // the space's group is drawn first (its one tab: no change)
    ui.key(View::Key::enter, View::alt);
    draw();
    draw();
    CHECK(layout.space.tabs.empty() && layout.windows.size() == 2);
    CHECK(once()); // the key
    // Closing it docks it into the space.
    clickAt(ui, at(in("WindowClose", int(in("Edit").id))), draw);
    draw();
    CHECK(layout.space.tabs == Tabs{"Edit"} && layout.windows.size() == 1);
    CHECK(once());
    // The other window to the space's own left edge: a side of all of it.
    gpu::Rect &w = layout.windows[0].frame;
    w = {450, 320, 300, 220};
    draw();
    draw();
    const Clay_BoundingBox wb = at(in("TabList", 1));
    drag(wb.x + wb.width - 6, wb.y + 6, 4 + 12, 150);
    CHECK(layout.windows.empty() && layout.space.children.size() == 2 && !layout.space.stacked);
    CHECK(layout.space.children.size() == 2 && layout.space.children[1].tabs == Tabs{"Edit"});
    CHECK(std::abs(layout.space.size - 400.0f / 3) < 1); // the third the outline showed, on the left
    CHECK(once());
}

// As the gallery's Dock page: a page button above a dock space holding one
// panel, three windows over it; two dropped into the space's middle, then
// the page button still takes a click.
static void clicksAfterDocking()
{
    Ui ui;
    gpu::Dock dock;
    int clicks = 0;
    const std::vector<gpu::Dock::Panel> panels = {{"Spectrogram", [] {}}, {"Scope", [] {}}, {"Waveform", [] {}}, {"Controls", [&] { ui.button("Reset"); }}};
    dock.layout.space.tabs = {"Spectrogram"};
    auto draw = [&] {
        ui.begin(1600, 1200, 2);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            if (ui.button("Page")) ++clicks;
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) { dock.space(ui, "Gallery", panels); }
        }
        dock.windows(ui, panels);
        ui.end(wgpu::RenderPassEncoder{});
        CHECK(everyNodeHasParent(ui));
    };
    auto in = [&](const char *name, int index = 0) { return Ui::idIn(&dock, name, index); };
    auto title = [&](const char *name) { return Clay_GetElementData(in("WindowHeader", int(in(name).id))).boundingBox; };
    auto drag = [&](float x0, float y0, float x1, float y1) { // pixels: scale 2
        pointer(ui, View::Pointer::move, x0, y0), draw(), pointer(ui, View::Pointer::down, x0, y0), draw();
        for (int i = 1; i <= 6; ++i) pointer(ui, View::Pointer::move, x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6), draw();
        pointer(ui, View::Pointer::up, x1, y1), draw(), draw();
    };
    draw();
    draw();
    const Clay_BoundingBox space = Clay_GetElementData(in("Gallery")).boundingBox;
    const float mx = (space.x + space.width * 0.75f) * 2, my = (space.y + space.height / 2) * 2; // clear of the windows
    for (const char *name : {"Controls", "Waveform"})
    {
        const Clay_BoundingBox t = title(name);
        drag((t.x + 30) * 2, (t.y + 6) * 2, mx, my);
    }
    CHECK(dock.layout.space.tabs.size() == 3 && dock.layout.windows.size() == 1);
    const Clay_BoundingBox page = box("Page");
    pointer(ui, View::Pointer::move, (page.x + 5) * 2, (page.y + 5) * 2), draw();
    pointer(ui, View::Pointer::down, (page.x + 5) * 2, (page.y + 5) * 2), draw();
    pointer(ui, View::Pointer::up, (page.x + 5) * 2, (page.y + 5) * 2), draw();
    CHECK(clicks == 1);
}

// The accessibility tree keeps its shape in odd cases: two windows in the
// same place, a hidden window, and a screen reader stepping or setting a
// splitter (taken when it's next drawn; increment makes its first area larger).
static void accessibilityEdgeCases()
{
    Ui ui;
    gpu::Rect one{40, 40, 200, 150}, two{40, 40, 200, 150};
    bool open1 = true, open2 = true, hideTwo = false;
    float size = 100;
    auto draw = [&] {
        frame(ui, [&] {
            ui.split("Sides", size, [] {}, [] {});
            ui.window("One", one, open1, [&] { ui.button("A"); });
            ui.window("Two", two, open2, [&] { ui.button("B"); });
            if (hideTwo) ui.hidden();
        });
        CHECK(everyNodeHasParent(ui));
    };
    draw();
    draw();
    hideTwo = true;
    draw();
    draw();
    bool b = false;
    for (const gpu::Accessible &n : ui.accessibility()->nodes) b |= n.name == "B" || n.name == "Two";
    CHECK(!b); // hidden, with what's in it
    uint64_t splitter = 0;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::splitter) splitter = n.id;
    CHECK(splitter != 0);
    ui.accessAction(splitter, gpu::Accessible::increment, 0);
    draw();
    CHECK(size == 116);
    ui.accessAction(splitter, gpu::Accessible::setValue, 60);
    draw();
    CHECK(size == 60);
}

// The dock's defaults: only a dock space takes a window; let go anywhere
// else (on another window too), it just moves. popOut and dockBack do it in
// code, for a button of one's own.
static void dockSpaceOnly()
{
    Ui ui;
    gpu::Dock dock;
    const std::vector<gpu::Dock::Panel> panels = {{"Mix", [] {}}, {"Edit", [] {}}};
    auto draw = [&] {
        ui.begin(800, 600, 1);
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(800), CLAY_SIZING_FIXED(600)}}})
        {
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(400), CLAY_SIZING_FIXED(300)}}}) { dock.space(ui, "Main", panels); }
        }
        dock.windows(ui, panels);
        ui.end(wgpu::RenderPassEncoder{});
        CHECK(everyNodeHasParent(ui));
    };
    using Tabs = std::vector<std::string>;
    gpu::Dock::Layout &layout = dock.layout;
    auto in = [&](const char *name, int index = 0) { return Ui::idIn(&dock, name, index); };
    auto title = [&](const char *name) { return Clay_GetElementData(in("WindowHeader", int(in(name).id))).boundingBox; };
    auto drag = [&](float x0, float y0, float x1, float y1) {
        pointer(ui, View::Pointer::move, x0, y0), draw(), pointer(ui, View::Pointer::down, x0, y0), draw();
        pointer(ui, View::Pointer::move, (x0 + x1) / 2, (y0 + y1) / 2), draw(), draw();
        pointer(ui, View::Pointer::move, x1, y1), draw(), draw();
        pointer(ui, View::Pointer::up, x1, y1), draw(), draw();
    };
    draw();
    layout.windows[0].frame = {420, 20, 240, 180}, layout.windows[1].frame = {420, 300, 240, 180};
    draw();
    draw();
    // Onto the other window's middle: it moves, nothing docks.
    Clay_BoundingBox t = title("Edit");
    drag(t.x + 30, t.y + 6, 540, 110);
    CHECK(layout.windows.size() == 2 && layout.space.tabs.empty());
    // Onto the space: docked.
    t = title("Edit");
    drag(t.x + 30, t.y + 6, 200, 150);
    CHECK(layout.windows.size() == 1 && layout.space.tabs == Tabs{"Edit"});
    // The pop-out button, clicked by a screen reader as a mouse would.
    dock.options.popOut = true;
    draw();
    draw();
    uint64_t button = 0;
    for (const gpu::Accessible &node : ui.accessibility()->nodes)
        if (node.name == "Pop out Edit") button = node.id;
    ui.accessAction(button, gpu::Accessible::click, 0);
    draw();
    draw();
    CHECK(button != 0 && layout.space.tabs.empty() && layout.windows.size() == 2);
    dock.dockBack("Edit");
    draw();
    dock.options.popOut = false;
    draw();
    CHECK(layout.space.tabs == Tabs{"Edit"});
    // popOut takes the panel named, not the shown one; showing another tab is a change.
    dock.dockBack("Mix");
    draw();
    draw(); // (it moves once a frame's drawn; its tab is there the frame after)
    CHECK(layout.space.tabs == (Tabs{"Edit", "Mix"}) && layout.space.shown == 0);
    int changes = 0;
    dock.changed = [&] { ++changes; };
    clickAt(ui, Clay_GetElementData(in("Tab", 1)).boundingBox, draw); // Mix's tab
    CHECK(layout.space.shown == 1 && changes == 1);
    clickAt(ui, Clay_GetElementData(in("Tab", 0)).boundingBox, draw); // back to Edit
    dock.popOut("Mix");
    draw();
    draw();
    CHECK(layout.space.tabs == Tabs{"Edit"} && layout.windows.size() == 1 && layout.windows[0].root.tabs == Tabs{"Mix"});
    dock.changed = nullptr;
    dock.dockBack("Mix");
    draw();
    // popOut and dockBack, as a button of one's own would.
    dock.popOut("Edit");
    draw();
    CHECK(layout.space.tabs == Tabs{"Mix"} && layout.windows.size() == 1);
    dock.dockBack("Edit");
    draw();
    CHECK(layout.space.tabs == (Tabs{"Mix", "Edit"}) && layout.windows.empty());
}

// Id scopes: alone, a scope is its owner (as idIn finds it); nested, it mixes
// with its parent, so one owner inside two others gives two ids.
static void idScopes()
{
    Ui ui;
    int a = 0, b = 0, c = 0;
    Clay_ElementId alone{}, inA{}, inC{};
    ui.begin(400, 300, 1);
    ui.pushId(&b), alone = ui.id("X"), ui.popId();
    ui.pushId(&a), ui.pushId(&b), inA = ui.id("X"), ui.popId(), ui.popId();
    ui.pushId(&c), ui.pushId(&b), inC = ui.id("X"), ui.popId(), ui.popId();
    ui.end(wgpu::RenderPassEncoder{});
    CHECK(alone.id == Ui::idIn(&b, "X").id && inA.id != inC.id && inA.id != alone.id);
}

// The views: laid out, each an image for screen readers saying what it
// shows; their boxes paint themselves.
static void signalViews()
{
    Ui ui;
    const std::vector<std::vector<gpu::Peak>> stereo = {{{-0.5f, 0.5f}, {-1, 1}}, {{0, 0.2f}}}, none;
    const float wave[] = {0, 1, 0, -1, 0};
    gpu::Spectrogram spectrogram;
    const float frequencies[] = {100, 1000}, decibels[] = {-20, -60};
    spectrogram.add(decibels, frequencies, 2);
    frame(ui, [&] {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            gpu::waveform(ui, "Input", stereo, {}, 40);
            gpu::waveform(ui, "Nothing", none, {}, 20);
            gpu::scope(ui, "Output", wave, 5, {.range = 2}, 40);
            spectrogram.draw(ui, "Spectrum", 60);
            gpu::spectrum(ui, "Analyzer", decibels, frequencies, 2, {.tilt = 4.5f}, 60);
        }
    });
    std::vector<std::pair<std::string, std::string>> read;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::image) read.push_back({n.name, n.description});
    CHECK(read.size() == 5);
    if (read.size() == 5)
    {
        CHECK(read[0].first == "Input" && read[0].second == "Stereo waveform, left above right");
        CHECK(read[1].second == "Empty waveform");
        CHECK(read[2].first == "Output" && read[2].second == "One-channel waveform, from -2 to 2");
        CHECK(read[3].first == "Spectrum" && read[3].second.find("log scale") != std::string::npos);
        CHECK(read[4].first == "Analyzer"
              && read[4].second == "Spectrum: 20 Hz to 20000 Hz on a log scale, -100 to 0 dB, tilted 4.5 dB per octave around 1000 Hz");
    }
    // Scrolling smoothly, it asks for frames while columns arrive, and not
    // once they've stopped.
    auto draw = [&] { frame(ui, [&] { spectrogram.draw(ui, "Spectrum", 60); }); };
    spectrogram.add(decibels, frequencies, 2);
    draw();
    CHECK(ui.animating());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    draw();
    draw();
    CHECK(!ui.animating());
}

// A docked drawer: its header opens and closes it; its handle resizes it,
// growing away from its edge, within its limits; screen readers see the
// handle as "Resize <title>".
static void dockedDrawers()
{
    Ui ui;
    bool topOpen = true, bottomOpen = true;
    float top = 60, bottom = 60;
    auto draw = [&] {
        frame(ui, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                ui.drawer("Top", topOpen, top, [] {}, {.min = 40, .max = 100});
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) {}
                ui.drawer("Bottom", bottomOpen, bottom, [] {}, {.edge = gpu::DrawerOptions::Edge::bottom, .min = 40, .max = 100}); // no index needed
            }
        });
    };
    draw();
    draw();
    auto drag = [&](const char *title, int index, float dy) {
        const Clay_BoundingBox h = box("DrawerHandle", Clay_GetElementIdWithIndex({false, int32_t(std::strlen(title)), title}, uint32_t(index)).id);
        pointer(ui, View::Pointer::down, h.x + 20, h.y + 3);
        draw();
        pointer(ui, View::Pointer::move, h.x + 20, h.y + 3 + dy);
        draw();
        pointer(ui, View::Pointer::up, h.x + 20, h.y + 3 + dy);
        draw();
    };
    drag("Top", 0, 20); // down: the top drawer grows
    CHECK(std::abs(top - 80) < 0.5f);
    drag("Bottom", 0, -20); // up: the bottom drawer grows
    CHECK(std::abs(bottom - 80) < 0.5f);
    drag("Top", 0, 500); // no further than its max
    CHECK(top == 100);
    bool named = false;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        named |= n.role == gpu::Accessible::Role::splitter && n.name == "Resize Top" && n.max == 100;
    CHECK(named);
    clickAt(ui, box("DrawerHeader", Clay_GetElementIdWithIndex(CLAY_STRING("Top"), 0).id), draw); // its header closes it
    CHECK(!topOpen && bottomOpen);
}

// An overlay drawer: closed, a tab 24 by 48 points centred on its box's
// edge, named by its title (no description repeating it); open, a panel over
// the box (which keeps its size) with the tab on its inner edge. Tab goes
// from the tab into the panel; Escape there closes it and focuses the tab.
static void overlayDrawer()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    bool open = false, inside = false, declared = false;
    float width = 120;
    auto draw = [&] {
        declared = false;
        frame(ui, [&] {
            CLAY({.id = CLAY_IDI("Plot", 0), .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
            {
                ui.drawer("Settings", open, width, [&] {
                    declared = true;
                    ui.toggle("Inside", inside);
                    ui.readout(params, a, "Amount", "%.2f");
                },
                          {.edge = gpu::DrawerOptions::Edge::right, .min = 80, .max = 200, .overlay = true});
            }
        });
    };
    draw();
    draw();
    const Clay_ElementId settings = Clay_GetElementIdWithIndex(CLAY_STRING("Settings"), 0);
    auto tab = [&] { return box("DrawerHeader", settings.id); };
    const Clay_BoundingBox plot = box("Plot");
    CHECK(tab().width == 24 && tab().height == 48);
    CHECK(tab().x + 24 == plot.x + plot.width && tab().y + 24 == plot.y + plot.height / 2); // on the right edge, centred
    auto node = [&]() -> const gpu::Accessible * {
        for (const gpu::Accessible &n : ui.accessibility()->nodes)
            if (n.role == gpu::Accessible::Role::button && n.name == "Settings") return &n;
        return nullptr;
    };
    CHECK(node() && node()->expanded == 0 && node()->description.empty());
    clickAt(ui, tab(), draw);
    draw();
    CHECK(open && declared && node() && node()->expanded == 1);
    const Clay_BoundingBox panel = Clay_GetElementData(settings).boundingBox, after = box("Plot");
    CHECK(after.width == plot.width && after.height == plot.height); // nothing under it moved
    CHECK(panel.x + panel.width == plot.x + plot.width && panel.width == 120 + 7 && panel.height == plot.height);
    CHECK(tab().x + 24 == panel.x && tab().width == 24); // riding on its inner edge
    // A readout in it is 24 points tall (WCAG 2.5.8), and the wheel over it
    // scrolls the panel rather than turning it, until it's focused.
    const Clay_BoundingBox amount = box("Readout", int(a->id));
    CHECK(amount.height == 24);
    const double was = a->value;
    ui.pointer({View::Pointer::wheel, amount.x + 4, amount.y + 4, 0, 1});
    draw();
    CHECK(a->value == was);
    ui.key(View::Key::tab, 0); // from the tab (focused by the click) into the panel: its edge's handle,
    draw();
    ui.key(View::Key::tab, 0); // then what's in it
    draw();
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(inside);
    CHECK(ui.key(View::Key::escape, 0)); // the toggle has no use for it: it closes the drawer
    draw();
    CHECK(!open);
    ui.key(View::Key::enter, 0); // focus went back to the tab, which opens it again
    draw();
    draw(); // (the tab moves out with the panel the frame after)
    CHECK(open);
    for (int i = 0; i < 3; ++i) ui.key(View::Key::tab, 0), draw(); // the handle, the toggle, the readout
    const Clay_BoundingBox focused = box("Readout", int(a->id));
    ui.pointer({View::Pointer::wheel, focused.x + 4, focused.y + 4, 0, 1}); // focused: the wheel turns it
    draw();
    CHECK(a->value != was);
    a->value = 0.8;
    CHECK(ui.key(View::Key::escape, 0)); // from a readout too: it has no other use for Escape
    draw();
    CHECK(!open && a->value == 0.8);
    ui.key(View::Key::enter, 0);
    draw();
    draw();
    CHECK(open);
    clickAt(ui, tab(), draw); // the same tab closes it
    CHECK(!open);
}

// The same drag at 2 pixels a point (a Retina or 2x desktop).
static void splitAtScale()
{
    Ui ui;
    float size = 100;
    auto draw = [&] {
        ui.begin(800, 600, 2);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(10)}})
        {
            ui.split("Scaled", size, [] {}, [] {}, {.min = 40, .secondMin = 40});
        }
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    draw();
    const Clay_BoundingBox line = box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Scaled"), 0).id); // points
    const float x = (line.x + 3) * 2, y = (line.y + 20) * 2; // pixels
    pointer(ui, View::Pointer::down, x, y);
    draw();
    pointer(ui, View::Pointer::move, x + 80, y); // 40 points right
    draw();
    pointer(ui, View::Pointer::up, x + 80, y);
    draw();
    CHECK(std::abs(size - 140) < 0.5f);

    // A drag that ends outside the view, as X11 reports it: the release,
    // then a leave, before the next frame. The release counts where it was.
    {
        const Clay_BoundingBox at = box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Scaled"), 0).id);
        const float px = (at.x + 3) * 2, py = (at.y + 20) * 2;
        pointer(ui, View::Pointer::down, px, py);
        draw();
        pointer(ui, View::Pointer::move, px + 40, py);
        draw();
        pointer(ui, View::Pointer::move, 2000, py);
        draw();
        const float dragged = size;
        pointer(ui, View::Pointer::up, 2000, py);
        pointer(ui, View::Pointer::leave, 2000, py);
        draw();
        CHECK(size == dragged && size > 140);
    }

    // A room that shrinks for a moment draws it smaller, but keeps its size.
    float kept = 200, room = 400;
    auto inRoom = [&] {
        ui.begin(800, 600, 2);
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(room), CLAY_SIZING_FIXED(100)}}})
        {
            ui.split("Room", kept, [] {}, [] {}, {.min = 40, .secondMin = 40});
        }
        ui.end(wgpu::RenderPassEncoder{});
    };
    inRoom();
    inRoom();
    room = 150;
    inRoom();
    inRoom();
    CHECK(kept == 200 && box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Room"), 0).id).x + 7 <= 150 + 0.5f);
    room = 400;
    inRoom();
    inRoom();
    CHECK(kept == 200 && std::abs(box("SplitDivider", Clay_GetElementIdWithIndex(CLAY_STRING("Room"), 0).id).x - 200) < 1);
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
    CHECK(std::abs(a->value - 1.0) < 1e-9); // the slider of A, not dragged, leaves it alone
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
    CHECK(ui.textCaret().y >= 100 && ui.textCaret().y < 140 && ui.textCaret().height > 0); // its caret, under the dial
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(std::abs(a->value - 0.25) < 1e-9);
    CHECK(ui.key(View::Key::up, 0)); // the knob has the keys again
    draw();
    CHECK(std::abs(a->value - 0.26) < 1e-9);
    ui.text("0.4"); // digits and Enter before a frame: the digits are set
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(std::abs(a->value - 0.4) < 1e-9 && !ui.editingText());
    a->value = 0.26;
    // The slider's value too; Escape abandons the typing, and only that.
    pointer(ui, View::Pointer::down, 290, 205);
    draw();
    pointer(ui, View::Pointer::up, 290, 205);
    draw();
    CHECK(ui.editingText());
    ui.text("0.9");
    ui.key(View::Key::escape, 0);
    draw();
    CHECK(!ui.editingText() && std::abs(a->value - 0.26) < 1e-9 && ui.key(View::Key::down, 0));
    draw();
    CHECK(std::abs(a->value - 0.25) < 1e-9);
    // Where the host set a decimal-comma locale, both separators still read
    // as a decimal point (if this system has such a locale to try).
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE"))
    {
        for (const char *typed : {"0,5", "0.75"})
        {
            pointer(ui, View::Pointer::down, 380, 280); // somewhere else first: the next click isn't a double-click
            pointer(ui, View::Pointer::up, 380, 280);
            draw();
            pointer(ui, View::Pointer::down, 50, 110);
            draw();
            pointer(ui, View::Pointer::up, 50, 110);
            draw();
            ui.text(typed);
            ui.key(View::Key::enter, 0);
            draw();
            CHECK(std::abs(a->value - (typed[2] == '5' ? 0.5 : 0.75)) < 1e-9);
        }
        std::setlocale(LC_NUMERIC, "C");
    }
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
    clickAt(ui, box("TextField"), draw); // soon after the last click: a double-click, which selects all
    ui.key(View::Key::end, 0);
    ui.text("é");
    ui.key(View::Key::backspace, 0);
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "xy" && commits == 1); // unchanged, so not a commit
    // Escape drops the edit.
    clickAt(ui, box("TextField"), draw);
    ui.key(View::Key::end, 0);
    ui.text("q");
    ui.key(View::Key::escape, 0);
    draw();
    CHECK(value == "xy" && commits == 1);
    // Enter or Escape leaves it focused, as a page's field: Enter edits it
    // again, typing goes on at its end, Tab moves on from it.
    CHECK(!ui.editingText() && ui.key(View::Key::enter, 0));
    draw();
    CHECK(ui.editingText());
    ui.text("w"); // over the selection: all of it
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "w" && commits == 2 && !ui.editingText());
    CHECK(ui.text("v"));
    draw();
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "wv" && commits == 3);
}

static void pianoNotes()
{
    // C4 to B4: seven white keys 20 pixels wide, as compost's.
    Ui ui;
    Ui::Notes notes;
    auto draw = [&] {
        ui.begin(400, 300, 1);
        notes = ui.piano(60, 71, gpu::Rect{0, 0, 140, 60});
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    pointer(ui, View::Pointer::down, 10, 50); // C, below the black keys
    draw();
    CHECK(notes.on == 60 && notes.off == -1);
    pointer(ui, View::Pointer::move, 30, 50); // slid to D
    draw();
    CHECK(notes.on == 62 && notes.off == 60);
    pointer(ui, View::Pointer::up, 30, 50);
    draw();
    CHECK(notes.on == -1 && notes.off == 62);
    pointer(ui, View::Pointer::down, 19, 10); // C sharp, on top of C and D
    draw();
    CHECK(notes.on == 61);
    pointer(ui, View::Pointer::up, 19, 10);
    draw();
    CHECK(notes.off == 61);
}

// Two pianos on a page: each holds its own note and reports its own notes;
// one taken away while a key is held owes its note-off, sent by its next call.
static void twoPianos()
{
    Ui ui;
    Ui::Notes top, bottom;
    bool showTop = true;
    auto draw = [&] {
        frame(ui, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (showTop) top = ui.piano(60, 71, 60, {}, 0);
                bottom = ui.piano(60, 71, 60, {}, 1);
            }
        });
    };
    draw();
    draw();
    const Clay_BoundingBox upper = box("Piano", 0), lower = box("Piano", 1);
    CHECK(upper.height == 60 && lower.y >= upper.y + upper.height);
    pointer(ui, View::Pointer::down, upper.x + upper.width / 2, upper.y + 50);
    draw();
    draw(); // (a laid-out piano's notes come from the next frame's call)
    const int held = top.on;
    CHECK(held >= 60 && bottom.on == -1 && bottom.off == -1);
    pointer(ui, View::Pointer::up, upper.x + upper.width / 2, upper.y + 50);
    draw();
    draw();
    CHECK(top.off == held && bottom.off == -1);
    // The upper one goes while a key is held: its note ends when it's back.
    pointer(ui, View::Pointer::down, upper.x + upper.width / 2, upper.y + 50);
    draw();
    draw();
    CHECK(top.on == held);
    showTop = false;
    draw();
    pointer(ui, View::Pointer::up, upper.x + upper.width / 2, upper.y + 50);
    draw();
    CHECK(bottom.on == -1 && bottom.off == -1);
    showTop = true;
    top = {};
    draw();
    draw();
    CHECK(top.off == held && top.on == -1);
}

static void editors()
{
    Ui ui;
    std::vector<Ui::Point> points = {{0, 0}, {0.5f, 1}, {1, 0}};
    std::vector<float> values(8, 0.0f);
    bool envelopeChanged = false, stepsChanged = false;
    auto draw = [&] {
        ui.begin(400, 300, 1);
        envelopeChanged |= ui.envelope(points, gpu::Rect{0, 0, 200, 100});
        stepsChanged |= ui.steps(values, gpu::Rect{0, 200, 160, 100});
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    // Drag the middle point down and right; it stays between its neighbours.
    pointer(ui, View::Pointer::down, 100, 0);
    draw();
    pointer(ui, View::Pointer::move, 150, 50);
    draw();
    pointer(ui, View::Pointer::up, 150, 50);
    draw();
    CHECK(std::abs(points[1].x - 0.75f) < 1e-4 && std::abs(points[1].y - 0.5f) < 1e-4 && envelopeChanged);
    // Double-click empty space adds a point there, in order.
    for (int i = 0; i < 2; ++i) pointer(ui, View::Pointer::down, 40, 80), pointer(ui, View::Pointer::up, 40, 80);
    draw();
    CHECK(points.size() == 4 && std::abs(points[1].x - 0.2f) < 1e-4);
    // An end can't move sideways.
    pointer(ui, View::Pointer::down, 0, 100);
    draw();
    pointer(ui, View::Pointer::move, 60, 20);
    draw();
    pointer(ui, View::Pointer::up, 60, 20);
    draw();
    CHECK(points[0].x == 0 && std::abs(points[0].y - 0.8f) < 1e-4);
    // A drag across the steps paints every bar it passes, even skipped ones.
    pointer(ui, View::Pointer::down, 10, 250);
    draw();
    pointer(ui, View::Pointer::move, 150, 250);
    draw();
    pointer(ui, View::Pointer::up, 150, 250);
    draw();
    CHECK(stepsChanged);
    for (float v : values) CHECK(std::abs(v - 0.5f) < 1e-4);
}

// Two step editors and two envelopes side by side: a drag on one leaves the
// other alone.
static void twoEditorsAtOnce()
{
    Ui ui;
    std::vector<float> left(4, 0.0f), right(4, 0.0f);
    std::vector<Ui::Point> first = {{0, 0}, {0.5f, 1}, {1, 0}}, second = first;
    auto draw = [&] {
        ui.begin(400, 300, 1);
        ui.steps(left, gpu::Rect{0, 0, 160, 100});
        ui.steps(right, gpu::Rect{200, 0, 160, 100});
        ui.envelope(first, gpu::Rect{0, 150, 160, 100});
        ui.envelope(second, gpu::Rect{200, 150, 160, 100});
        ui.end(wgpu::RenderPassEncoder{});
    };
    auto drag = [&](float x0, float y0, float x1, float y1) {
        pointer(ui, View::Pointer::down, x0, y0), draw();
        pointer(ui, View::Pointer::move, x1, y1), draw();
        pointer(ui, View::Pointer::up, x1, y1), draw();
    };
    draw();
    drag(250, 50, 340, 50); // across the right steps
    for (float v : left) CHECK(v == 0);
    CHECK(right[0] == 0 && std::abs(right[1] - 0.5f) < 1e-4 && std::abs(right[3] - 0.5f) < 1e-4);
    drag(280, 150, 280, 200); // the second envelope's middle point, down
    CHECK(first[1].y == 1 && std::abs(second[1].y - 0.5f) < 1e-4);
    drag(80, 150, 80, 225); // the first's
    CHECK(std::abs(first[1].y - 0.25f) < 1e-4 && std::abs(second[1].y - 0.5f) < 1e-4);
}

// Steps, envelope and piano take the keyboard and tell screen readers what
// they are: a group of sliders or buttons, one for each part.
static void editorKeysAndScreenReader()
{
    Ui ui;
    std::vector<Ui::Point> points = {{0, 0}, {0.5f, 1}, {1, 0}};
    std::vector<float> values(8, 0.5f);
    bool envelopeChanged = false, stepsChanged = false;
    Ui::Notes notes;
    auto draw = [&] {
        ui.begin(400, 300, 1);
        envelopeChanged |= ui.envelope(points, gpu::Rect{0, 0, 200, 100});
        stepsChanged |= ui.steps(values, gpu::Rect{0, 200, 160, 100});
        notes = ui.piano(60, 71, gpu::Rect{200, 0, 140, 60});
        ui.end(wgpu::RenderPassEncoder{});
    };
    auto find = [&](const char *name) -> const gpu::Accessible * {
        for (const gpu::Accessible &n : ui.accessibility()->nodes)
            if (n.name == name) return &n;
        return nullptr;
    };
    auto childrenOf = [&](const char *name) {
        std::vector<const gpu::Accessible *> parts;
        if (const gpu::Accessible *group = find(name))
            for (uint64_t id : group->children)
                for (const gpu::Accessible &n : ui.accessibility()->nodes)
                    if (n.id == id) parts.push_back(&n);
        return parts;
    };
    using Role = gpu::Accessible::Role;
    draw();

    // Steps: a group of eight sliders, 0 to 100 %.
    const gpu::Accessible *group = find("Steps");
    CHECK(group && group->role == Role::group);
    std::vector<const gpu::Accessible *> bars = childrenOf("Steps");
    CHECK(bars.size() == 8 && bars[0]->name == "Step 1" && bars[7]->name == "Step 8");
    CHECK(bars[0]->role == Role::slider && bars[0]->value == "50%" && bars[0]->numeric && bars[0]->min == 0 && bars[0]->max == 1);
    CHECK(bars[0]->actions & gpu::Accessible::increment && bars[0]->actions & gpu::Accessible::setValue);
    // A click chooses a bar; Right moves to the next, Up and Down change it.
    pointer(ui, View::Pointer::down, 10, 250);
    draw();
    pointer(ui, View::Pointer::up, 10, 250);
    draw();
    ui.key(View::Key::right, 0);
    ui.key(View::Key::up, 0);
    draw();
    CHECK(stepsChanged && std::abs(values[1] - 0.55f) < 1e-4 && values[0] == 0.5f && values[2] == 0.5f);
    CHECK(find("Step 2")->value == "55%" && ui.accessibility()->focus == find("Step 2")->id); // the chosen bar has the focus
    ui.key(View::Key::down, View::shift);
    ui.key(View::Key::pageDown, 0);
    draw();
    CHECK(std::abs(values[1] - 0.29f) < 1e-4);
    ui.key(View::Key::end, 0);
    ui.key(View::Key::deleteForward, 0);
    draw();
    CHECK(values[7] == 0 && std::abs(values[1] - 0.29f) < 1e-4 && ui.accessibility()->focus == find("Step 8")->id);
    ui.key(View::Key::home, 0);
    ui.key(View::Key::down, 0);
    for (int i = 0; i < 20; ++i) ui.key(View::Key::up, 0);
    draw();
    CHECK(values[0] == 1); // stops at the top
    // A screen reader steps and sets any bar, and the control has the focus.
    ui.accessAction(find("Step 4")->id, gpu::Accessible::increment, 0);
    ui.accessAction(find("Step 5")->id, gpu::Accessible::setValue, 0.2);
    ui.accessAction(find("Step 6")->id, gpu::Accessible::decrement, 0);
    draw();
    CHECK(std::abs(values[3] - 0.55f) < 1e-4 && std::abs(values[4] - 0.2f) < 1e-4 && std::abs(values[5] - 0.45f) < 1e-4);
    ui.accessAction(find("Step 3")->id, gpu::Accessible::focus, 0);
    draw();
    CHECK(ui.accessibility()->focus == find("Step 3")->id);
    ui.key(View::Key::up, 0);
    draw();
    CHECK(std::abs(values[2] - 0.55f) < 1e-4);

    // Envelope: a slider for each point's level, with its time in the value.
    group = find("Envelope");
    CHECK(group && group->role == Role::group);
    std::vector<const gpu::Accessible *> dots = childrenOf("Envelope");
    CHECK(dots.size() == 3 && dots[1]->name == "Point 2" && dots[1]->role == Role::slider);
    CHECK(dots[1]->value == "time 50%, level 100%" && dots[1]->number == 1);
    pointer(ui, View::Pointer::down, 100, 0); // the middle point
    draw();
    pointer(ui, View::Pointer::up, 100, 0);
    draw();
    ui.key(View::Key::down, 0);
    ui.key(View::Key::left, View::alt);
    draw();
    CHECK(envelopeChanged && std::abs(points[1].y - 0.95f) < 1e-4 && std::abs(points[1].x - 0.49f) < 1e-4);
    CHECK(find("Point 2")->value == "time 49%, level 95%" && ui.accessibility()->focus == find("Point 2")->id);
    ui.key(View::Key::enter, 0); // a point after it, on the line
    draw();
    CHECK(points.size() == 4 && std::abs(points[2].x - 0.745f) < 1e-4 && std::abs(points[2].y - 0.475f) < 1e-4);
    CHECK(ui.accessibility()->focus == find("Point 3")->id);
    for (int i = 0; i < 100; ++i) ui.key(View::Key::right, View::alt); // can't pass its neighbour
    draw();
    CHECK(points[2].x == 1 && points[3].x == 1); // stops at its neighbour
    ui.key(View::Key::deleteForward, 0);
    draw();
    CHECK(points.size() == 3 && ui.accessibility()->focus == find("Point 2")->id);
    ui.key(View::Key::home, 0);
    ui.key(View::Key::deleteForward, 0); // not an end
    draw();
    CHECK(points.size() == 3);
    ui.accessAction(find("Point 3")->id, gpu::Accessible::setValue, 0.4);
    ui.accessAction(find("Point 1")->id, gpu::Accessible::increment, 0);
    draw();
    CHECK(std::abs(points[2].y - 0.4f) < 1e-4 && std::abs(points[0].y - 0.05f) < 1e-4 && points[0].x == 0 && points[2].x == 1);

    // Piano: one picture to screen readers, with its range; not a Tab stop.
    const gpu::Accessible *piano = find("Piano");
    CHECK(piano && piano->role == Role::image && piano->description == "C4 to B4" && piano->children.empty());
    pointer(ui, View::Pointer::down, 210, 50); // played with the pointer, as before
    draw();
    CHECK(notes.on == 60);
    pointer(ui, View::Pointer::up, 210, 50);
    draw();
    CHECK(notes.off == 60);

    // In a layout, the caller's label names the group.
    Ui laid;
    std::vector<float> bars4(4, 0.25f);
    auto drawLaid = [&] { frame(laid, [&] { laid.steps(bars4); laid.label("Cutoff steps"); }); };
    drawLaid();
    drawLaid();
    bool named = false;
    for (const gpu::Accessible &n : laid.accessibility()->nodes) named |= n.role == Role::group && n.name == "Cutoff steps" && n.children.size() == 4;
    CHECK(named);
}

// Seeded random pointer, wheel, key and text input over every control at once.
// Whatever arrives, values stay in range, editors stay well formed, and every
// gesture that begins ends.
static void randomInput(uint32_t seed)
{
    Ui ui;
    gpu::Params params;
    std::vector<int> open(3, 0);
    bool unbalanced = false;
    params.send = [&](const core::Value &m) {
        const std::string type(m["type"].text());
        const int id = int(m["id"].number());
        if (type == "begin") unbalanced |= open[id]++ != 0;
        if (type == "end") unbalanced |= --open[id] != 0;
        if (type == "change") unbalanced |= open[id] != 1;
    };
    core::Value::Array list;
    list.push_back(core::Value::Map{{"id", 0.0}, {"name", "A"}, {"min", 0.0}, {"max", 1.0}, {"default", 0.5}});
    list.push_back(core::Value::Map{{"id", 1.0}, {"name", "B"}, {"min", -24.0}, {"max", 24.0}, {"default", 0.0}});
    list.push_back(core::Value::Map{{"id", 2.0}, {"name", "C"}, {"min", 20.0}, {"max", 20000.0}, {"default", 1000.0}});
    params.receive(core::Value::Map{{"type", "params"}, {"params", list}});
    gpu::Param *a = params.find("A"), *b = params.find("B"), *c = params.find("C");

    std::vector<Ui::Point> points = {{0, 0}, {0.5f, 1}, {1, 0}};
    std::vector<float> values(8, 0.5f);
    std::string field = "text";
    bool on = false, drawerOpen = true;
    int segment = 0, choice = 0;
    const char *items[] = {"One", "Two", "Three"};
    int sounding = 0; // pianos' notes on, less their offs
    auto sound = [&](Ui::Notes n) { sounding += (n.on >= 0) - (n.off >= 0); };
    auto draw = [&] {
        frame(ui, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(180), CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                ui.button("Go");
                ui.toggle("On", on);
                ui.segmented(items, 3, segment);
                ui.dropdown(items, 3, choice);
                ui.textField(field, 100);
                ui.numberBox(params, b, "%.1f");
                if (ui.drawer("More", drawerOpen))
                {
                    ui.knob(params, c, "%.0f", true);
                    sound(ui.piano(48, 60, 30, {}, 1)); // goes and comes back with its drawer, maybe with a key held
                }
                ui.slider(params, a, "%.2f");
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                ui.vslider(params, b, "%.1f", false, 60);
                ui.xyPad(params, a, c);
                ui.envelope(points);
                ui.steps(values);
                sound(ui.piano(48, 72, 40));
            }
        });
    };
    uint32_t state = seed;
    auto next = [&] { state = state * 1664525u + 1013904223u; return state >> 8; };
    auto coordinate = [&](float size) { return float(next() % 1000) / 1000 * (size + 100) - 50; };
    const char *texts[] = {"1", "-", ".", "e", "x", "é", "1e9", "-1e9", "nan", "inf", ""};
    bool down = false;
    float x = 0, y = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const unsigned kind = next() % 16;
        if (kind < 6) x = coordinate(400), y = coordinate(300), ui.pointer({View::Pointer::move, x, y, next() % 2 ? View::shift : 0u});
        else if (kind < 8 && !down) down = true, ui.pointer({View::Pointer::down, x, y});
        else if (kind < 10 && down) down = false, ui.pointer({View::Pointer::up, x, y});
        else if (kind == 10) ui.pointer({View::Pointer::wheel, x, y, 0, float(int(next() % 7) - 3)});
        else if (kind == 11) ui.pointer({View::Pointer::leave, x, y});
        else if (kind < 14) ui.key(View::Key(next() % (int(View::Key::copy) + 1)), next() % 4);
        else ui.text(texts[next() % std::size(texts)]);
        if (next() % 3 == 0) draw();

        for (gpu::Param *p : {a, b, c}) CHECK(std::isfinite(p->value) && p->value >= p->min && p->value <= p->max);
        CHECK(points.size() >= 2 && points.front().x == 0 && points.back().x == 1);
        for (size_t k = 0; k < points.size(); ++k)
        {
            CHECK(points[k].y >= 0 && points[k].y <= 1);
            if (k) CHECK(points[k].x >= points[k - 1].x);
        }
        for (float v : values) CHECK(v >= 0 && v <= 1);
        CHECK(segment >= 0 && segment < 3 && choice >= 0 && choice < 3);
        CHECK(!unbalanced && sounding >= 0 && sounding <= 2);
        if (failures) return (void)std::fprintf(stderr, "random input: seed %u, event %d\n", seed, i); // the first is the useful one
    }
    if (down) ui.pointer({View::Pointer::up, x, y});
    ui.pointer({View::Pointer::leave, x, y});
    ui.key(View::Key::escape, 0);
    drawerOpen = true; // a piano that went with its drawer sends what it owes
    draw();
    draw();
    draw();
    for (int n : open) CHECK(n == 0);
    CHECK(sounding == 0);
}

// Selection, word moves, undo and the clipboard inside a field (stb_textedit).
// With no fonts in this test, text has no width, so only keys move the caret.
static void textFieldShortcuts()
{
    Ui ui;
    std::string value = "hello world", copied;
    ui.copyText = [&](const std::string &text) { copied = text; };
    auto draw = [&] { frame(ui, [&] { ui.textField(value, 100); }); };
    draw();
    CHECK(!ui.key(View::Key::undo, View::command)); // not editing: the host's undo
    clickAt(ui, box("TextField"), draw);
    ui.key(View::Key::end, 0);
    ui.key(View::Key::left, View::shift | View::alt); // select the last word
    ui.key(View::Key::copy, View::command);
    for (const char *c : {"t", "h", "e", "r", "e"}) ui.text(c); // one key at a time, replacing the selection
    draw();
    CHECK(copied == "world");
    ui.key(View::Key::undo, View::command); // the whole run, and the selection it replaced
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "hello world");
    clickAt(ui, box("TextField"), draw);
    ui.key(View::Key::selectAll, View::command);
    ui.key(View::Key::cut, View::command);
    ui.text("a\nb"); // pasted: one line
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(copied == "hello world" && value == "ab");
    clickAt(ui, box("TextField"), draw);
    ui.key(View::Key::end, 0);
    ui.text("c");
    ui.key(View::Key::left, 0); // moving the caret ends the run
    ui.text("d");
    ui.key(View::Key::undo, View::command);
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(value == "abc");
}

// A focused control takes only the keys it uses; the rest, and every key
// after a click on empty space, are the host's.
// Tab past the last control (Shift+Tab before the first) leaves the view:
// the host gets it, to move on to its next control, so the keyboard can't
// be trapped in here (WCAG 2.1.2). The next Tab comes in at the first.
static void tabLeavesTheView()
{
    Ui ui;
    std::string name = "One";
    int clicks = 0;
    bool on = false;
    auto draw = [&] {
        frame(ui, [&] {
            if (ui.button("Go")) ++clicks;
            ui.toggle("Sync", on);
            ui.textField(name);
        });
    };
    draw();
    CHECK(ui.key(View::Key::tab, 0)); // in, at the first
    draw();
    draw(); // (the frame after a key, which the presenter draws: the newly focused control says which keys it takes)
    CHECK(ui.key(View::Key::enter, 0));
    draw();
    CHECK(clicks == 1);
    CHECK(!ui.key(View::Key::tab, View::shift)); // before the first: out
    draw();
    CHECK(!ui.key(View::Key::enter, 0)); // nothing has focus
    CHECK(ui.key(View::Key::tab, View::shift)); // back in, at the last: the field
    draw();
    draw();
    CHECK(ui.editingText());
    ui.text("Two");
    CHECK(!ui.key(View::Key::tab, 0)); // past the last: out, keeping the edit
    draw();
    draw();
    CHECK(!ui.editingText() && name == "Two" && !ui.key(View::Key::enter, 0));
    CHECK(ui.key(View::Key::tab, 0) && ui.key(View::Key::tab, 0)); // in again, and on to the toggle
    draw();
    draw();
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(on && clicks == 1);
    Ui empty; // nothing to focus: Tab is the host's at once
    frame(empty, [] {});
    CHECK(!empty.key(View::Key::tab, 0));
}

static void keysBackToHost()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    int clicks = 0;
    auto draw = [&] {
        ui.begin(400, 300, 1);
        ui.knob(params, a, gpu::Rect{0, 0, 100, 140}, "%.2f");
        CLAY({.layout = {.padding = {200, 0, 200, 0}}}) { if (ui.button("Go")) ++clicks; }
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    pointer(ui, View::Pointer::down, 50, 50);
    draw();
    pointer(ui, View::Pointer::up, 50, 50);
    draw();
    // A focused knob takes its keys, as compost's does: the arrows step,
    // Delete and Backspace reset it, Enter opens its value to type; a key it
    // doesn't use goes back to the host, Escape among them (it's for closing
    // what the knob is in).
    CHECK(ui.key(View::Key::up, 0) && !ui.key(View::Key::escape, 0) && ui.key(View::Key::deleteForward, 0)
          && !ui.key(View::Key::undo, 0));
    draw();
    CHECK(a->value == a->defaultValue);
    ui.key(View::Key::up, View::alt); // a page's step
    draw();
    CHECK(std::abs(a->value - (a->defaultValue + 0.1)) < 1e-9);
    a->value = 0.8;
    CHECK(ui.key(View::Key::backspace, 0));
    draw();
    CHECK(a->value == a->defaultValue);
    a->value = 0.8;
    CHECK(ui.key(View::Key::enter, 0));
    draw();
    CHECK(ui.editingText());
    ui.key(View::Key::left, 0); // moves the caret: the knob stays put
    ui.key(View::Key::escape, 0); // leaves the typing, and only that
    draw();
    CHECK(!ui.editingText() && a->value == 0.8);
    clickAt(ui, box("Go"), draw);
    CHECK(clicks == 1 && ui.key(View::Key::enter, 0));
    draw();
    CHECK(clicks == 2);
    pointer(ui, View::Pointer::move, 390, 290);
    draw();
    pointer(ui, View::Pointer::down, 390, 290); // empty space
    draw();
    pointer(ui, View::Pointer::up, 390, 290);
    draw();
    CHECK(!ui.key(View::Key::enter, 0) && !ui.key(View::Key::up, 0));
}

// Long pastes over selections, undone and redone over and over: stb_textedit's
// undo history fills and discards records, the path chardraw.cmake patches. (Its
// overrun lands inside the same struct, which ASan doesn't see.)
static void longUndoHistory()
{
    Ui ui;
    std::string value = "start";
    auto draw = [&] { frame(ui, [&] { ui.textField(value, 100); }); };
    draw();
    clickAt(ui, box("TextField"), draw);
    uint32_t seed = 7;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u, seed >> 8; };
    for (int i = 0; i < 400; ++i)
    {
        ui.key(View::Key::selectAll, View::command);
        ui.text(std::string(100 + next() % 300, char('a' + next() % 26)).c_str());
        for (uint32_t n = next() % 4; n > 0; --n) ui.key(View::Key::undo, View::command);
        for (uint32_t n = next() % 3; n > 0; --n) ui.key(View::Key::redo, View::command);
        draw();
    }
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(!value.empty());
}

// Number boxes as compost's: a drag right or up past a dead zone, a
// double-click resets, typing a number while focused edits it, and keys
// step, reset and edit.
static void numberBoxLikeCompost()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    auto draw = [&] { frame(ui, [&] { ui.numberBox(params, a, "%.2f"); }); };
    draw();
    const Clay_BoundingBox b = box("Number", int(a->id));
    const float x = b.x + b.width / 2, y = b.y + b.height / 2;
    pointer(ui, View::Pointer::move, x, y);
    draw();
    pointer(ui, View::Pointer::down, x, y);
    draw();
    pointer(ui, View::Pointer::move, x + 3, y); // inside the dead zone
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    pointer(ui, View::Pointer::move, x + 36, y); // a fifth of 180 points
    draw();
    CHECK(std::abs(a->value - 0.7) < 1e-6);
    pointer(ui, View::Pointer::up, x + 36, y);
    draw();
    for (int i = 0; i < 2; ++i) // double-click: back to the default
    {
        pointer(ui, View::Pointer::down, x, y);
        pointer(ui, View::Pointer::up, x, y);
    }
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    CHECK(ui.text("0") && ui.text(".") && ui.text("2")); // focused: typing edits it
    draw();
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(std::abs(a->value - 0.2) < 1e-9);
    CHECK(ui.key(View::Key::up, 0)); // still focused after the edit
    draw();
    CHECK(std::abs(a->value - 0.21) < 1e-9);
    CHECK(!ui.key(View::Key::escape, 0) && ui.key(View::Key::deleteForward, 0)); // Delete resets, not Escape
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    CHECK(ui.key(View::Key::up, View::shift)); // Shift: a fine step, as a knob's
    draw();
    CHECK(std::abs(a->value - 0.501) < 1e-9);
    // Being edited, its caret is where an input method's window goes.
    ui.key(View::Key::enter, 0);
    draw();
    const View::Caret caret = ui.textCaret();
    CHECK(ui.editingText() && caret.x > b.x && caret.x < b.x + b.width && caret.y == b.y && caret.height == b.height);
    ui.key(View::Key::escape, 0);
    draw();
    CHECK(ui.text("0.3")); // digits and Enter before a frame: the digits are set
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(std::abs(a->value - 0.3) < 1e-9 && !ui.editingText());
}

// Readouts, as char-spectrum's: a number drags (the range in 200 points) and
// resets on a double-click; one of items steps on a click and on the wheel;
// both are spin buttons to screen readers, stepped by the arrows.
static void readouts()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    static const char *const onOff[] = {"Off", "On"};
    auto draw = [&] { frame(ui, [&] { ui.readout(params, a, "Amount", "%.2f"); ui.readout(params, b, "Bypass", onOff, 2, 120); }); };
    draw();
    const Clay_BoundingBox ra = box("Readout", int(a->id)), rb = box("Readout", int(b->id));
    CHECK(rb.width == 120 && ra.width > 0);
    const float x = ra.x + ra.width / 2, y = ra.y + ra.height / 2;
    pointer(ui, View::Pointer::move, x, y);
    draw();
    pointer(ui, View::Pointer::down, x, y);
    draw();
    pointer(ui, View::Pointer::move, x, y - 40); // up a fifth of 200 points
    draw();
    CHECK(std::abs(a->value - 0.7) < 1e-6);
    pointer(ui, View::Pointer::up, x, y - 40);
    draw();
    for (int i = 0; i < 2; ++i) // double-click: back to the default
    {
        pointer(ui, View::Pointer::down, x, y);
        draw();
        pointer(ui, View::Pointer::up, x, y);
        draw();
    }
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    CHECK(ui.key(View::Key::up, 0)); // focused by the click: the arrows step it
    draw();
    CHECK(std::abs(a->value - 0.51) < 1e-9);
    // Delete resets it. Escape is left to what it's in (a menu, a drawer),
    // except during a drag, which it cancels.
    CHECK(!ui.key(View::Key::escape, 0) && ui.key(View::Key::deleteForward, 0));
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    pointer(ui, View::Pointer::down, x, y);
    draw();
    pointer(ui, View::Pointer::move, x, y - 40);
    draw();
    CHECK(std::abs(a->value - 0.7) < 1e-6 && ui.key(View::Key::escape, 0));
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    pointer(ui, View::Pointer::up, x, y - 40);
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
    // B's default 0.5 is item 1, "On": a click steps (wrapping) to "Off", the wheel back.
    clickAt(ui, rb, draw);
    CHECK(b->value == 0);
    ui.pointer({View::Pointer::wheel, rb.x + 4, rb.y + 4, 0, 0.5f}); // half a notch: not yet
    draw();
    CHECK(b->value == 0);
    ui.pointer({View::Pointer::wheel, rb.x + 4, rb.y + 4, 0, 0.5f});
    draw();
    CHECK(b->value == 1);
    draw(); // described as it was drawn
    int spins = 0;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::spinButton)
        {
            ++spins;
            CHECK(n.name == "A" || (n.name == "B" && n.value == "On"));
        }
    CHECK(spins == 2);
}

// A spectrogram across time: columns fill points of time at its rate, newest
// on the right; probe reads what's under a point and snaps to a peak near it;
// fewer columns keep the newest.
// A spectrogram's frequency marks: round numbers, as many as fit the gap,
// more as the range narrows; a decade's start, then 1, 2 and 5, then steps.
static void spectrogramMarks()
{
    auto marks = [](float low, float high, gpu::Spectrogram::Scale scale, float height, float gap) {
        gpu::Spectrogram::Options o;
        o.minFrequency = low, o.maxFrequency = high, o.scale = scale;
        gpu::Spectrogram view(o);
        const gpu::Rect r{0, 0, 100, height};
        std::vector<float> m = view.frequencyMarks(r, gap);
        for (size_t i = 1; i < m.size(); ++i)
            CHECK(m[i] > m[i - 1] && std::abs(view.yAt(r, m[i]) - view.yAt(r, m[i - 1])) >= gap); // in order, and apart
        for (float f : m) CHECK(f >= low && f <= high);
        return m;
    };
    using S = gpu::Spectrogram::Scale;
    const std::vector<float> wide = marks(20, 24000, S::log, 430, 28);
    CHECK((wide == std::vector<float>{20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000}));
    const std::vector<float> bass = marks(30, 100, S::log, 430, 28); // every 10 Hz
    CHECK((bass == std::vector<float>{30, 40, 50, 60, 70, 80, 90, 100}));
    const std::vector<float> narrow = marks(40, 46, S::log, 430, 28);
    CHECK(narrow.size() >= 6 && narrow.front() == 40 && narrow.back() == 46); // a hertz apart
    const std::vector<float> linear = marks(0, 1000, S::linear, 400, 18);
    CHECK(linear.size() == 21 && linear[1] == 50); // 50 Hz is 20 pixels
    CHECK(marks(20, 24000, S::mel, 430, 28).size() >= 5);
}

// A spectrum's curve: a pixel apart, through each bin's level where it
// falls, never past the levels either side between bins, the loudest of
// many bins in a pixel; tilted about its pivot; silence out of sight below.
static void spectrumCurve()
{
    const gpu::SpectrumView view{.minFrequency = 10, .maxFrequency = 10000, .scales = false};
    const gpu::Rect plot{0, 0, 300, 100}; // 100 pixels a decade, a pixel a dB
    const float hz[] = {10, 100, 1000, 10000}, db[] = {-60, -60, 0, -60};
    std::vector<float> xy;
    view.curve(db, hz, 4, plot, xy);
    CHECK(xy.size() == 301 * 2 && xy[0] == 0 && xy[600] == 300);
    auto yAt = [&](float x) { return xy[size_t(std::lround(x)) * 2 + 1]; };
    CHECK(std::abs(yAt(view.x(plot, 1000))) < 1e-3f && std::abs(yAt(0) - 60) < 1e-3f && std::abs(yAt(100) - 60) < 1e-3f);
    bool between = true, flat = true;
    for (size_t i = 0; i < xy.size(); i += 2)
        between &= xy[i + 1] >= -1e-3f && xy[i + 1] <= 60 + 1e-3f, flat &= xy[i] > 100 || std::abs(xy[i + 1] - 60) < 1e-3f;
    CHECK(between && flat); // no overshoot, and level where the bins either side are
    // Tilted 3 dB an octave about 1 kHz: a decade up about 10 dB louder, two down about 20 dB quieter.
    gpu::SpectrumView tilted = view;
    tilted.tilt = 3;
    view.curve(db, hz, 4, plot, xy);
    std::vector<float> before = xy;
    tilted.curve(db, hz, 4, plot, xy);
    CHECK(std::abs(xy[401] - before[401]) < 1e-2f);                         // 1 kHz
    CHECK(std::abs(before[601] - xy[601] - 3 * std::log2(10.0f)) < 1e-2f); // 10 kHz
    CHECK(std::abs(xy[1] - before[1] - 6 * std::log2(10.0f)) < 1e-2f);     // 10 Hz, two decades down
    // Many bins to a pixel (an FFT's highs): the loudest of them.
    std::vector<float> manyHz, manyDb;
    for (int i = 0; i < 200; ++i) manyHz.push_back(9000 + 5.0f * i), manyDb.push_back(i == 37 ? -6.0f : -80.0f);
    view.curve(manyDb.data(), manyHz.data(), manyHz.size(), plot, xy);
    CHECK(std::abs(yAt(view.x(plot, 9185)) - 6) < 1e-3f);
    // Silence (and NaN) a whole range below the floor, out of sight.
    const float quiet[] = {-INFINITY, NAN};
    view.curve(quiet, hz, 2, plot, xy);
    CHECK(yAt(0) == 200 && yAt(150) == 200);
    view.curve(nullptr, nullptr, 0, plot, xy);
    CHECK(xy.size() == 602 && yAt(10) == 200);
}

static void spectrogramTime()
{
    Ui ui;
    gpu::Spectrogram spectrogram({.minFrequency = 0, .maxFrequency = 1000, .scale = gpu::Spectrogram::Scale::linear, .columns = 400,
                                  .rows = 60, .pixelsPerSecond = 100, .smooth = false});
    float hz[11], at500[11], at300[11];
    for (int k = 0; k < 11; ++k) hz[k] = 100.0f * k, at500[k] = at300[k] = -100;
    at500[4] = at500[6] = -20, at500[5] = 0;  // a peak on 500 Hz, even either side
    at300[2] = -20, at300[3] = 0, at300[4] = -10; // one between 300 and 400 Hz
    spectrogram.add(at500, hz, 11, 0.1f); // ten points
    for (int i = 0; i < 4; ++i) spectrogram.add(at300, hz, 11, 0.0125f); // five, a column at a time
    const gpu::Rect r{10, 10, 380, 60}; // the box the layout gives it
    auto draw = [&] { frame(ui, [&] { spectrogram.draw(ui, "Spectrum", 60); }); };
    draw();
    auto yOf = [&](float f) { return r.y + (1 - f / 1000) * r.h; };
    gpu::Spectrogram::Probe p;
    CHECK(spectrogram.probe(r.x + r.w - 0.5f, yOf(480), false, p)); // the newest point: 300 Hz's column
    CHECK(std::abs(p.frequency - 480) < 0.5f && std::abs(p.decibels - (-10 + (-100 + 10) * 0.8f)) < 0.1f && !p.peak);
    CHECK(spectrogram.probe(r.x + r.w - 7.5f, yOf(480), false, p) && std::abs(p.decibels - (-4)) < 0.1f); // 500 Hz's
    CHECK(spectrogram.probe(r.x + r.w - 7.5f, yOf(480), true, p) && p.peak && std::abs(p.frequency - 500) < 1e-3f
          && std::abs(p.decibels) < 1e-3f && std::abs(p.y - yOf(500)) < 1e-3f);
    CHECK(spectrogram.probe(r.x + r.w - 0.5f, yOf(320), true, p) && p.peak && p.frequency > 300 && p.frequency < 350);
    CHECK(spectrogram.probe(r.x + r.w - 7.5f, yOf(700), true, p) && !p.peak); // 12 points from the peak: too far
    CHECK(!spectrogram.probe(r.x + r.w - 20.5f, yOf(500), false, p)); // before the first column
    // Fewer columns: the newest stay where they were.
    gpu::Spectrogram::Options o = spectrogram.settings();
    o.columns = 12;
    spectrogram.configure(o);
    draw();
    CHECK(spectrogram.probe(r.x + r.w - 7.5f, yOf(500), true, p) && p.peak && std::abs(p.frequency - 500) < 1e-3f);
    CHECK(!spectrogram.probe(r.x + r.w - 14.5f, yOf(500), false, p)); // kept no further back than 12
}

// While a knob is dragged nothing else is hovered, however far the pointer
// goes; after the release the button under it is.
static void noHoverWhileDragging()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    auto draw = [&] {
        ui.begin(400, 300, 1);
        ui.knob(params, a, gpu::Rect{0, 0, 100, 140}, "%.2f");
        CLAY({.layout = {.padding = {200, 0, 200, 0}}}) { ui.button("Go"); }
        ui.end(wgpu::RenderPassEncoder{});
    };
    const Clay_ElementId go = Clay_GetElementIdWithIndex({false, 2, "Go"}, 0);
    draw();
    const Clay_BoundingBox b = box("Go");
    pointer(ui, View::Pointer::down, 50, 50);
    draw();
    pointer(ui, View::Pointer::move, b.x + 5, b.y + 5);
    draw();
    CHECK(!Clay_PointerOver(go));
    pointer(ui, View::Pointer::up, b.x + 5, b.y + 5);
    draw();
    CHECK(Clay_PointerOver(go));
}

// A press and a quick flick before the next frame: the press lands where it
// was made, and the drag follows from there.
static void flickAfterPress()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A");
    auto draw = [&] {
        ui.begin(400, 300, 1);
        ui.knob(params, a, gpu::Rect{0, 0, 100, 140}, "%.2f");
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw();
    const double before = a->value;
    pointer(ui, View::Pointer::down, 50, 50);
    pointer(ui, View::Pointer::move, 50, -50); // off the knob already
    draw();
    CHECK(ui.dragging());
    draw();
    CHECK(a->value > before);
    pointer(ui, View::Pointer::up, 50, -50);
    draw();
}

// A knob that goes away mid-drag (a drawer closed, the page changed) lets
// go: its gesture ends, the pointer unlocks, and other controls work again.
static void dragOfVanishedControl()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params);
    gpu::Param *a = params.find("A"), *b = params.find("B");
    bool locked = false;
    ui.lockPointer = [&](bool on) { locked = on; };
    auto draw = [&](bool withA) {
        ui.begin(400, 300, 1);
        if (withA) ui.knob(params, a, gpu::Rect{0, 0, 100, 140}, "%.2f");
        ui.knob(params, b, gpu::Rect{200, 0, 100, 140}, "%.2f");
        ui.end(wgpu::RenderPassEncoder{});
    };
    draw(true);
    sent.clear();
    pointer(ui, View::Pointer::down, 50, 50);
    draw(true);
    CHECK(locked && ui.dragging());
    draw(false);
    CHECK(!locked && !ui.dragging() && !sent.empty() && sent.back() == "end");
    pointer(ui, View::Pointer::up, 50, 50);
    draw(false);
    const double before = b->value;
    pointer(ui, View::Pointer::down, 250, 50);
    draw(false);
    pointer(ui, View::Pointer::move, 250, 0);
    draw(false);
    CHECK(b->value > before);
    pointer(ui, View::Pointer::up, 250, 0);
    draw(false);
}

// Files dragged in land on the box under them; a press on a source that
// moves past 4 points drags its files out, once.
static void fileDrags()
{
    Ui ui;
    std::vector<std::string> dropped, out;
    int made = 0;
    ui.dragFiles = [&](const std::vector<std::string> &paths) { out = paths; };
    auto draw = [&] {
        frame(ui, [&] {
            const Clay_ElementId target = CLAY_IDI("Target", 0), source = CLAY_IDI("Source", 0);
            ui.dropTarget(target, dropped);
            CLAY({.id = target, .layout = {.sizing = {CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(40)}}}) {}
            ui.dragSource(source, [&] { ++made; return std::vector<std::string>{"/tmp/a.wav"}; });
            CLAY({.id = source, .layout = {.sizing = {CLAY_SIZING_FIXED(40), CLAY_SIZING_FIXED(40)}}}) {}
        });
    };
    draw();
    draw(); // the target is known from the frame before
    const Clay_BoundingBox t = box("Target"), src = box("Source");
    CHECK(!ui.files(View::FileDrag::over, t.x + t.width + 50, 300, {"/x.txt"})); // over nothing
    CHECK(ui.files(View::FileDrag::over, t.x + 10, t.y + 10, {"/x.txt"}));
    draw();
    CHECK(dropped.empty() && ui.fileOver(CLAY_IDI("Target", 0)));
    CHECK(ui.files(View::FileDrag::drop, t.x + 10, t.y + 10, {"/x.txt", "/y.txt"}));
    draw();
    CHECK(dropped.size() == 2 && dropped[1] == "/y.txt");
    dropped.clear();
    draw();
    CHECK(dropped.empty()); // a drop is seen by one frame
    pointer(ui, View::Pointer::move, src.x + 5, src.y + 5);
    draw();
    pointer(ui, View::Pointer::down, src.x + 5, src.y + 5);
    draw();
    pointer(ui, View::Pointer::move, src.x + 7, src.y + 5);
    draw();
    CHECK(out.empty()); // not far enough yet
    pointer(ui, View::Pointer::move, src.x + 30, src.y + 5);
    draw();
    pointer(ui, View::Pointer::move, src.x + 60, src.y + 5);
    draw();
    CHECK(out.size() == 1 && made == 1);
    pointer(ui, View::Pointer::up, src.x + 60, src.y + 5);
    draw();

    // Two views (two plugin windows), each its own layout: a drag over one
    // is judged by its own last frame, whichever was drawn last.
    Ui other;
    std::vector<std::string> elsewhere;
    auto drawOther = [&] {
        frame(other, [&] {
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(200), CLAY_SIZING_FIXED(200)}}}) {} // the target further along
            const Clay_ElementId target = CLAY_IDI("Target", 0);
            other.dropTarget(target, elsewhere);
            CLAY({.id = target, .layout = {.sizing = {CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(40)}}}) {}
        });
    };
    draw();
    drawOther();
    drawOther();
    CHECK(ui.files(View::FileDrag::over, t.x + 10, t.y + 10, {"/x.txt"}));
    CHECK(!other.files(View::FileDrag::over, t.x + 10, t.y + 10, {"/x.txt"}));
    CHECK(ui.bounds(CLAY_IDI("Target", 0)).x == t.x && other.bounds(CLAY_IDI("Target", 0)).x > t.x);
}

// A plot of the caller's as a tab stop: Tab reaches it, the Menu key opens
// its menu over it when it fills the view, and screen readers see one node,
// described by the view drawn in it.
static void tabStops()
{
    Ui ui;
    static const char *const items[] = {"Clear", "Pause"};
    std::vector<int> chosen;
    bool on = false, ring = false;
    auto draw = [&] {
        frame(ui, [&] {
            ui.toggle("Sync", on);
            const Clay_ElementId plot = ui.id("Plot");
            ring = ui.tabStop(plot, gpu::Accessible::Role::image, "Plot");
            void *view = ui.paint([&ui](gpu::Rect r) { ui.accessible(gpu::Accessible::Role::image, "Picture", nullptr, r, "What it shows"); });
            CLAY({.id = plot, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}, .custom = {.customData = view}}) {}
            if (const int i = ui.contextMenu(plot, items, 2); i >= 0) chosen.push_back(i);
        });
    };
    const gpu::Accessibility *tree = ui.accessibility();
    using Role = gpu::Accessible::Role;
    auto images = [&] {
        std::vector<gpu::Accessible> found;
        for (const gpu::Accessible &n : tree->nodes)
            if (n.role == Role::image) found.push_back(n);
        return found;
    };
    auto focused = [&] {
        for (const gpu::Accessible &n : tree->nodes)
            if (n.id == tree->focus) return n;
        return gpu::Accessible{};
    };
    draw();
    draw();
    const std::vector<gpu::Accessible> plots = images();
    CHECK(plots.size() == 1 && plots[0].name == "Plot" && plots[0].description == "What it shows");
    CHECK(plots.size() == 1 && plots[0].actions & gpu::Accessible::showMenu && plots[0].actions & gpu::Accessible::focus);
    // Tab: the toggle, then the plot, its ring shown; its arrows are the host's.
    CHECK(ui.key(View::Key::tab, 0));
    draw();
    CHECK(ui.key(View::Key::tab, 0));
    draw();
    draw();
    CHECK(ring && focused().name == "Plot");
    CHECK(!ui.key(View::Key::left, 0));
    // The Menu key opens its menu, inside the view though the plot fills it.
    CHECK(ui.key(View::Key::contextMenu, 0));
    draw();
    CHECK(focused().name == "Clear" && focused().role == Role::menuItem);
    const Clay_BoundingBox item = box("ContextMenuItem", 0);
    CHECK(item.y >= 0 && item.y + item.height <= 300);
    CHECK(ui.key(View::Key::enter, 0));
    draw();
    draw();
    CHECK(chosen.size() == 1 && chosen[0] == 0 && focused().name == "Plot");
    // A click on it doesn't focus it.
    clickAt(ui, box("Plot"), draw);
    CHECK(!ring && focused().name != "Plot");
}

// A context menu, as the WAI-ARIA menu: a secondary click or the Menu key
// opens it, focus goes into it and back, and nothing under it is touched.
static void contextMenus()
{
    Ui ui;
    gpu::Params params;
    setUpParams(params); // A and B: 0..1
    gpu::Param *a = params.find("A");
    static const char *const items[] = {"Reset", "Copy", nullptr, "Cut"}; // a separator before Cut
    static const char *const plotItems[] = {"Clear"};
    std::vector<int> chosen, plotChosen;
    int clicks = 0;
    bool on = false;
    auto draw = [&] {
        frame(ui, [&] {
            ui.knob(params, a, "%.2f");
            if (const int i = ui.contextMenu(items, 4); i >= 0) chosen.push_back(i);
            if (ui.button("Go")) ++clicks;
            ui.toggle("Sync", on);
            // A custom-painted box of the caller's, given a menu by its id.
            void *plot = ui.paint([](gpu::Rect) {});
            CLAY({.id = ui.id("Plot"), .layout = {.sizing = {CLAY_SIZING_FIXED(60), CLAY_SIZING_FIXED(60)}}, .custom = {.customData = plot}}) {}
            if (const int i = ui.contextMenu(ui.id("Plot"), plotItems, 1); i >= 0) plotChosen.push_back(i);
        });
    };
    auto secondary = [&](Clay_BoundingBox b) {
        const float x = b.x + b.width / 2, y = b.y + b.height / 2;
        pointer(ui, View::Pointer::move, x, y);
        draw();
        ui.pointer({View::Pointer::down, x, y, 0, 0, false, 1});
        draw();
        ui.pointer({View::Pointer::up, x, y, 0, 0, false, 1});
        draw();
    };
    const gpu::Accessibility *tree = ui.accessibility();
    auto find = [&](const char *name, gpu::Accessible::Role role) -> const gpu::Accessible * {
        for (const gpu::Accessible &n : tree->nodes)
            if (n.name == name && n.role == role) return &n;
        return nullptr;
    };
    using Role = gpu::Accessible::Role;
    auto open = [&] { // (Clay keeps a box after its element is gone: ask the tree)
        return std::any_of(tree->nodes.begin(), tree->nodes.end(), [](const gpu::Accessible &n) { return n.role == Role::menu; });
    };
    draw();
    draw();
    // Screen readers can open it from the knob, and the plot; not from the button.
    const gpu::Accessible *knob = find("A", Role::slider);
    CHECK(knob && knob->actions & gpu::Accessible::showMenu);
    CHECK(!(find("Go", Role::button)->actions & gpu::Accessible::showMenu));
    const uint64_t knobId = knob->id;

    // A secondary click opens it at the pointer, without touching the knob.
    a->value = 0.8;
    sent.clear();
    const Clay_BoundingBox k = box("Knob", a->id);
    secondary(k);
    CHECK(open() && sent.empty() && a->value == 0.8);
    CHECK(std::abs(box("ContextMenuItem", 0).x - (k.x + k.width / 2 + 1)) < 1.5f); // its border's inside the pointer
    const gpu::Accessible *menu = find("A", Role::menu); // named for its control
    CHECK(menu && menu->children.size() == 3); // the separator isn't an item
    CHECK(tree->focus == menu->id);            // focus is in it, nothing highlighted yet
    // Its first item, clicked: chosen once, and focus goes back to the knob.
    clickAt(ui, box("ContextMenuItem", 0), draw);
    CHECK(chosen.size() == 1 && chosen[0] == 0 && !open());
    draw();
    CHECK(chosen.size() == 1 && tree->focus == knobId);

    // The Menu key on the focused knob opens it below, on the first item;
    // the arrows wrap and skip the separator, Home, End and letters move.
    CHECK(ui.key(View::Key::contextMenu, 0));
    draw();
    CHECK(open() && tree->focus == find("Reset", Role::menuItem)->id);
    auto highlighted = [&] {
        for (const gpu::Accessible &n : tree->nodes)
            if (n.id == tree->focus) return n.name;
        return std::string();
    };
    const std::pair<View::Key, const char *> steps[] = {{View::Key::down, "Copy"}, {View::Key::down, "Cut"}, {View::Key::down, "Reset"},
                                                        {View::Key::up, "Cut"}, {View::Key::home, "Reset"}, {View::Key::end, "Cut"}};
    for (auto [key, name] : steps)
    {
        CHECK(ui.key(key, 0));
        draw();
        CHECK(highlighted() == name);
    }
    CHECK(ui.text("c"));
    draw();
    CHECK(highlighted() == "Copy");
    CHECK(ui.text("c"));
    draw();
    CHECK(highlighted() == "Cut");
    CHECK(ui.key(View::Key::enter, 0));
    draw();
    CHECK(chosen.size() == 2 && chosen[1] == 3 && !open());
    draw();
    CHECK(chosen.size() == 2 && tree->focus == knobId);
    // Space chooses too.
    ui.key(View::Key::contextMenu, View::shift); // Shift+F10 comes the same way
    draw();
    CHECK(ui.text(" "));
    draw();
    CHECK(chosen.size() == 3 && chosen[2] == 0);

    // Escape closes it and focus goes back to the knob, leaving its value;
    // Tab closes it too, not moving on.
    ui.key(View::Key::contextMenu, 0);
    draw();
    CHECK(ui.key(View::Key::escape, 0));
    draw();
    CHECK(!open() && tree->focus == knobId && a->value == 0.8);
    ui.key(View::Key::contextMenu, 0);
    draw();
    CHECK(ui.key(View::Key::tab, 0));
    draw();
    CHECK(!open() && tree->focus == knobId && chosen.size() == 3);

    // A press outside closes it without acting, and doesn't reach the button under it.
    secondary(k);
    CHECK(open());
    clickAt(ui, box("Go"), draw);
    CHECK(!open() && clicks == 0 && chosen.size() == 3);
    clickAt(ui, box("Go"), draw);
    CHECK(clicks == 1);
    // A control without a menu leaves the Menu key to the host.
    ui.accessAction(find("Sync", Role::checkBox)->id, gpu::Accessible::focus, 0);
    draw();
    CHECK(!ui.key(View::Key::contextMenu, 0));

    // A screen reader's showMenu opens it, as the Menu key does.
    ui.accessAction(knobId, gpu::Accessible::showMenu, 0);
    draw();
    CHECK(open() && tree->focus == find("Reset", Role::menuItem)->id);
    ui.accessAction(knobId, gpu::Accessible::showMenu, 0); // again (a page hears the key and the reader's request): it stays
    draw();
    CHECK(open() && tree->focus == find("Reset", Role::menuItem)->id);
    ui.accessAction(find("Copy", Role::menuItem)->id, gpu::Accessible::click, 0);
    draw();
    CHECK(chosen.size() == 4 && chosen[3] == 1 && !open());

    // The caller's own box: a secondary click on it opens its menu.
    secondary(box("Plot"));
    CHECK(open() && find("Clear", Role::menuItem));
    clickAt(ui, box("ContextMenuItem", 0), draw);
    CHECK(plotChosen.size() == 1 && chosen.size() == 4);
}

int main()
{
    layoutSizes();
    buttonClicks();
    toggleAndSegmented();
    quickClicksCount();
    dropdownSelectsAndBlocks();
    noItems();
    slidersAndGrip();
    laidOutSlider();
    contextMenus();
    tabStops();
    keyboardFocus();
    knobDragAndKeys();
    screenReader();
    screenReaderReading();
    screenReaderFocusesParts();
    numberBoxTabs();
    keyboardReachesEverything();
    doubleClickOrFineDrag();
    splitPanes();
    windows();
    windowInScope();
    dockedDrawers();
    overlayDrawer();
    signalViews();
    tabStrip();
    dockableWindows();
    dockSpaceOnly();
    idScopes();
    accessibilityEdgeCases();
    clicksAfterDocking();
    splitAtScale();
    parameterToggleAndSegmented();
    textFieldEdits();
    textFieldShortcuts();
    fileDrags();
    dragOfVanishedControl();
    longUndoHistory();
    keysBackToHost();
    tabLeavesTheView();
    flickAfterPress();
    noHoverWhileDragging();
    numberBoxLikeCompost();
    readouts();
    spectrogramMarks();
    spectrogramTime();
    spectrumCurve();
    pianoNotes();
    twoPianos();
    editors();
    twoEditorsAtOnce();
    editorKeysAndScreenReader();
    for (uint32_t seed = 1; seed <= 4 && !failures; ++seed) randomInput(seed);
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
