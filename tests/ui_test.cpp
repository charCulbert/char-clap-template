// The layout and the controls (libs/draw2d) without a GPU: synthetic pointer
// and key events in, element rectangles, values and parameter messages out.
// Draw2D records draws but never flushes them, having no device.
#include "../libs/draw2d/ui.h"
#include "../libs/draw2d/views.h"
#include "../libs/draw2d/dock.h"
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
    ui.announce("Preset loaded");
    draw();
    bool announced = false;
    for (const gpu::Accessible &n : tree->nodes) announced |= n.live && n.value == "Preset loaded";
    CHECK(announced);
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

// Tabs: a press shows one; focused, the arrows wrap, Home and End go to the
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
    clickAt(ui, box("Tab", 1), draw);
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
    auto title = [&](const char *name) { return box("WindowHeader", Clay_GetElementIdWithIndex(Clay_String{false, int32_t(std::strlen(name)), name}, 0).id); };
    draw();
    CHECK(layout.windows.size() == 3 && layout.space.tabs.empty());
    layout.windows[0].frame = {20, 20, 240, 180}, layout.windows[1].frame = {300, 20, 240, 180}, layout.windows[2].frame = {20, 300, 240, 180};
    draw();
    draw();

    // Edit's title onto the middle of Mix's window: one window of tabs.
    Clay_BoundingBox t = title("Edit");
    drag(t.x + 30, t.y + 6, 20 + 120, 20 + 90);
    CHECK(layout.windows.size() == 2 && layout.windows[0].root.tabs == (Tabs{"Mix", "Edit"}));
    // Notes' title near that window's right edge: a split in it.
    const gpu::Rect &both = layout.windows[0].frame;
    const Clay_BoundingBox g = box("DockGroup", 0); // its group, under the bar
    t = title("Notes");
    drag(t.x + 30, t.y + 6, g.x + g.width - 10, g.y + g.height / 2);
    CHECK(layout.windows.size() == 1 && layout.windows[0].root.children.size() == 2 && !layout.windows[0].root.stacked);
    CHECK(layout.windows[0].root.children.size() == 2 && layout.windows[0].root.children[1].tabs == Tabs{"Notes"});
    CHECK(std::abs(layout.windows[0].root.size - (g.width / 2 - 7)) < 1); // the half the outline showed, on the right

    // The bar's empty part moves the window.
    const float x0 = both.x;
    const Clay_BoundingBox bar = box("TabList", 900);
    drag(bar.x + bar.width - 6, bar.y + 6, bar.x + bar.width - 6 + 50, bar.y + 6 + 30);
    CHECK(std::abs(layout.windows[0].frame.x - (x0 + 50)) < 1);

    // Along the bar: Mix onto Edit's place. Out of it: Edit in a window of its own.
    Clay_BoundingBox mix = box("Tab", 900 * 64), edit = box("Tab", 900 * 64 + 1);
    drag(mix.x + mix.width / 2, mix.y + 6, edit.x + edit.width / 2, edit.y + 6);
    CHECK(layout.windows[0].root.children.size() == 2 && layout.windows[0].root.children[0].tabs == (Tabs{"Edit", "Mix"}));
    edit = box("Tab", 900 * 64);
    drag(edit.x + edit.width / 2, edit.y + 6, 450, 350); // let go on nothing: it stays where it's carried
    CHECK(layout.windows.size() == 2 && layout.windows[1].root.tabs == Tabs{"Edit"});
    CHECK(std::abs(layout.windows[1].frame.x - (450 - 60)) < 1);

    // A dock space: Edit's title onto its middle, then Alt and Enter pull it out again.
    withSpace = true;
    layout.windows[0].frame = {450, 20, 300, 220}; // clear of the space
    draw();
    draw();
    t = title("Edit");
    drag(t.x + 30, t.y + 6, 200, 150); // the middle of the 400 x 300 space
    CHECK(layout.windows.size() == 1 && layout.space.tabs == Tabs{"Edit"});
    clickAt(ui, box("Tab", 900 * 64), draw); // the space's group is drawn first
    ui.key(View::Key::enter, View::alt);
    draw();
    draw();
    CHECK(layout.space.tabs.empty() && layout.windows.size() == 2);
    // Closing it docks it into the space.
    clickAt(ui, box("WindowClose", Clay_GetElementIdWithIndex(CLAY_STRING("Edit"), 0).id), draw);
    draw();
    CHECK(layout.space.tabs == Tabs{"Edit"} && layout.windows.size() == 1);
    // The other window to the space's own left edge: a side of all of it.
    gpu::Rect &w = layout.windows[0].frame;
    w = {450, 320, 300, 220};
    draw();
    draw();
    const Clay_BoundingBox wb = box("TabList", 901);
    drag(wb.x + wb.width - 6, wb.y + 6, 4 + 12, 150);
    CHECK(layout.windows.empty() && layout.space.children.size() == 2 && !layout.space.stacked);
    CHECK(layout.space.children.size() == 2 && layout.space.children[1].tabs == Tabs{"Edit"});
    CHECK(std::abs(layout.space.size - 400.0f / 3) < 1); // the third the outline showed, on the left
}

// The views: laid out, each an image for screen readers saying what it
// shows; their boxes draw themselves and never reach onCustom.
static void signalViews()
{
    Ui ui;
    const std::vector<std::vector<gpu::Peak>> stereo = {{{-0.5f, 0.5f}, {-1, 1}}, {{0, 0.2f}}}, none;
    const float wave[] = {0, 1, 0, -1, 0};
    gpu::Spectrogram spectrogram;
    const float frequencies[] = {100, 1000}, decibels[] = {-20, -60};
    spectrogram.add(decibels, frequencies, 2);
    int customs = 0;
    frame(ui, [&] {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            gpu::waveform(ui, "Input", stereo, {}, 40);
            gpu::waveform(ui, "Nothing", none, {}, 20);
            gpu::scope(ui, "Output", wave, 5, {.range = 2}, 40);
            spectrogram.draw(ui, "Spectrum", 60);
        }
    }, [&](int, void *, gpu::Rect) { ++customs; });
    CHECK(customs == 0);
    std::vector<std::pair<std::string, std::string>> read;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        if (n.role == gpu::Accessible::Role::image) read.push_back({n.name, n.description});
    CHECK(read.size() == 4);
    if (read.size() == 4)
    {
        CHECK(read[0].first == "Input" && read[0].second == "Stereo waveform, left above right");
        CHECK(read[1].second == "Empty waveform");
        CHECK(read[2].first == "Output" && read[2].second == "One-channel waveform, from -2 to 2");
        CHECK(read[3].first == "Spectrum" && read[3].second.find("log scale") != std::string::npos);
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
                ui.drawer("Bottom", bottomOpen, bottom, [] {}, {.edge = gpu::DrawerOptions::Edge::bottom, .min = 40, .max = 100}, 1);
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
    drag("Bottom", 1, -20); // up: the bottom drawer grows
    CHECK(std::abs(bottom - 80) < 0.5f);
    drag("Top", 0, 500); // no further than its max
    CHECK(top == 100);
    bool named = false;
    for (const gpu::Accessible &n : ui.accessibility()->nodes)
        named |= n.role == gpu::Accessible::Role::splitter && n.name == "Resize Top" && n.max == 100;
    CHECK(named);
    clickAt(ui, box("Drawer", 0), draw); // its header closes it
    CHECK(!topOpen);
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
    ui.key(View::Key::enter, 0);
    draw();
    CHECK(std::abs(a->value - 0.25) < 1e-9);
    // Where the host set a decimal-comma locale, both separators still read
    // as a decimal point (if this system has such a locale to try).
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE"))
    {
        for (const char *typed : {"0,5", "0.75"})
        {
            pointer(ui, View::Pointer::down, 50, 110);
            draw();
            pointer(ui, View::Pointer::up, 50, 110);
            draw();
            ui.text(typed);
            ui.key(View::Key::enter, 0);
            draw();
        }
        CHECK(std::abs(a->value - 0.75) < 1e-9);
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
                if (ui.drawer("More", drawerOpen)) ui.knob(params, c, "%.0f", true);
                ui.slider(params, a, "%.2f");
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                ui.vslider(params, b, "%.1f", false, 60);
                ui.xyPad(params, a, c);
                ui.envelope(points);
                ui.steps(values);
                ui.piano(48, 72, 40);
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
        CHECK(!unbalanced);
        if (failures) return (void)std::fprintf(stderr, "random input: seed %u, event %d\n", seed, i); // the first is the useful one
    }
    if (down) ui.pointer({View::Pointer::up, x, y});
    ui.pointer({View::Pointer::leave, x, y});
    ui.key(View::Key::escape, 0);
    draw();
    draw();
    for (int n : open) CHECK(n == 0);
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
    // Escape, Delete and Backspace reset it, Enter opens its value to type;
    // a key it doesn't use goes back to the host.
    CHECK(ui.key(View::Key::up, 0) && ui.key(View::Key::escape, 0) && !ui.key(View::Key::undo, 0));
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
// undo history fills and discards records, the path draw2d.cmake patches. (Its
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
    CHECK(ui.key(View::Key::escape, 0)); // resets
    draw();
    CHECK(std::abs(a->value - 0.5) < 1e-9);
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
}

int main()
{
    layoutSizes();
    buttonClicks();
    toggleAndSegmented();
    quickClicksCount();
    dropdownSelectsAndBlocks();
    keyboardFocus();
    knobDragAndKeys();
    screenReader();
    screenReaderReading();
    numberBoxTabs();
    keyboardReachesEverything();
    doubleClickOrFineDrag();
    splitPanes();
    windows();
    dockedDrawers();
    signalViews();
    tabStrip();
    dockableWindows();
    splitAtScale();
    parameterToggleAndSegmented();
    textFieldEdits();
    textFieldShortcuts();
    fileDrags();
    dragOfVanishedControl();
    longUndoHistory();
    keysBackToHost();
    flickAfterPress();
    noHoverWhileDragging();
    numberBoxLikeCompost();
    pianoNotes();
    editors();
    for (uint32_t seed = 1; seed <= 4 && !failures; ++seed) randomInput(seed);
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
