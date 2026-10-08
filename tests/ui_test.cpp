// The layout and the controls (libs/draw2d) without a GPU: synthetic pointer
// and key events in, element rectangles, values and parameter messages out.
// Draw2D records draws but never flushes them, having no device.
#include "../libs/draw2d/ui.h"
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
    ui.key(View::Key::tab, 0);
    draw();
    ui.key(View::Key::right, 0);
    ui.key(View::Key::right, 0);
    draw();
    CHECK(selected == 2);
    ui.key(View::Key::tab, 0);
    draw();
    ui.key(View::Key::down, 0);
    draw();
    CHECK(shape == 1);
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
    CHECK(ui.key(View::Key::up, 0) && !ui.key(View::Key::enter, 0) && !ui.key(View::Key::escape, 0)
          && !ui.key(View::Key::backspace, 0));
    draw();
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
// undo history fills and discards records (under ASan in CI, an overrun of
// its record array would show here).
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
            const Clay_ElementId target = CLAY_ID("Target"), source = CLAY_ID("Source");
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
    CHECK(dropped.empty() && ui.fileOver(CLAY_ID("Target")));
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
    dropdownSelectsAndBlocks();
    keyboardFocus();
    knobDragAndKeys();
    textFieldEdits();
    textFieldShortcuts();
    fileDrags();
    dragOfVanishedControl();
    longUndoHistory();
    keysBackToHost();
    pianoNotes();
    editors();
    for (uint32_t seed = 1; seed <= 4 && !failures; ++seed) randomInput(seed);
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
