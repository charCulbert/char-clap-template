#include "ui.h"
#include <algorithm>
#include <functional>
#include <string_view>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

// stb_textedit does the editing (caret, selection, word moves, undo) on the
// text as codepoints, one line; Ui keeps draft and its offsets in step.
namespace gpu {
struct TextBuffer
{
    std::u32string text;
    std::function<float(char32_t)> width; // in points
};
}
#define STB_TEXTEDIT_CHARTYPE char32_t
#define STB_TEXTEDIT_STRING gpu::TextBuffer
#define STB_TEXTEDIT_UNDOSTATECOUNT 64
#define STB_TEXTEDIT_UNDOCHARCOUNT 1024
#include "stb_textedit.h"

static void layoutRow(StbTexteditRow *row, gpu::TextBuffer *buffer, int start)
{
    float width = 0;
    for (size_t i = size_t(start); i < buffer->text.size(); ++i) width += buffer->width(buffer->text[i]);
    *row = {};
    row->x1 = width, row->ymax = row->baseline_y_delta = 1;
    row->num_chars = int(buffer->text.size()) - start;
}
#define STB_TEXTEDIT_STRINGLEN(b) int((b)->text.size())
#define STB_TEXTEDIT_LAYOUTROW(row, b, start) layoutRow(row, b, start)
#define STB_TEXTEDIT_GETWIDTH(b, start, i) ((b)->width((b)->text[size_t((start) + (i))]))
#define STB_TEXTEDIT_KEYTOTEXT(key) -1 // text comes in through stb_textedit_paste
#define STB_TEXTEDIT_GETCHAR(b, i) ((b)->text[size_t(i)])
#define STB_TEXTEDIT_NEWLINE U'\n'
#define STB_TEXTEDIT_IS_SPACE(c) ((c) == U' ' || (c) == U'\t')
#define STB_TEXTEDIT_DELETECHARS(b, i, n) ((b)->text.erase(size_t(i), size_t(n)))
#define STB_TEXTEDIT_INSERTCHARS(b, i, chars, n) ((b)->text.insert(size_t(i), (chars), size_t(n)), 1)
#define STB_TEXTEDIT_K_LEFT 0x200000
#define STB_TEXTEDIT_K_RIGHT 0x200001
#define STB_TEXTEDIT_K_UP 0x200002
#define STB_TEXTEDIT_K_DOWN 0x200003
#define STB_TEXTEDIT_K_PGUP 0x200004
#define STB_TEXTEDIT_K_PGDOWN 0x200005
#define STB_TEXTEDIT_K_LINESTART 0x200006
#define STB_TEXTEDIT_K_LINEEND 0x200007
#define STB_TEXTEDIT_K_TEXTSTART 0x200008
#define STB_TEXTEDIT_K_TEXTEND 0x200009
#define STB_TEXTEDIT_K_DELETE 0x20000A
#define STB_TEXTEDIT_K_BACKSPACE 0x20000B
#define STB_TEXTEDIT_K_UNDO 0x20000C
#define STB_TEXTEDIT_K_REDO 0x20000D
#define STB_TEXTEDIT_K_WORDLEFT 0x20000E
#define STB_TEXTEDIT_K_WORDRIGHT 0x20000F
#define STB_TEXTEDIT_K_SHIFT 0x400000
#define STB_TEXTEDIT_IMPLEMENTATION
#include "stb_textedit.h"

namespace gpu {

static constexpr unsigned keyBit(View::Key key) { return 1u << unsigned(key); }
// The keys that step a value: arrows, Page Up and Down, Home and End.
static constexpr unsigned stepKeys = keyBit(View::Key::left) | keyBit(View::Key::right) | keyBit(View::Key::up)
                                   | keyBit(View::Key::down) | keyBit(View::Key::pageUp) | keyBit(View::Key::pageDown)
                                   | keyBit(View::Key::home) | keyBit(View::Key::end);

struct TextEdit
{
    TextBuffer buffer;
    STB_TexteditState state;
    bool selecting = false; // a press in the field is dragging out a selection
    int typing = -1;        // the undo record a run of typing started, so the run undoes as one step
};

// Folds the undo records from `first` on into one, while each continues where the last ended.
static void joinUndo(StbUndoState &u, int first)
{
    while (first >= 0 && u.undo_point - 1 > first)
    {
        StbUndoRecord &a = u.undo_rec[u.undo_point - 2], &b = u.undo_rec[u.undo_point - 1];
        if (b.insert_length != 0 || a.where + a.delete_length != b.where) return;
        a.delete_length += b.delete_length;
        --u.undo_point;
    }
}

Ui::Ui() : textEdit(std::make_unique<TextEdit>()) {}
Ui::~Ui() = default;

static std::u32string toCodepoints(std::string_view utf8)
{
    std::u32string out;
    for (size_t i = 0; i < utf8.size();)
    {
        const auto c = static_cast<unsigned char>(utf8[i]);
        const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        char32_t cp = extra ? c & (0x3F >> extra) : c;
        size_t j = i + 1;
        for (; j < utf8.size() && j <= i + size_t(extra) && (utf8[j] & 0xC0) == 0x80; ++j) cp = cp << 6 | (utf8[j] & 0x3F);
        out.push_back(cp);
        i = j;
    }
    return out;
}
static std::string toUtf8(std::u32string_view text)
{
    std::string out;
    for (char32_t c : text)
    {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        else out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

void Ui::pointer(const View::PointerEvent &event)
{
    // A move after a press the frame hasn't seen yet waits for it, so the
    // press lands where it was made, not where a quick flick ended.
    if (event.type == View::Pointer::move && mouse.pressed)
    {
        heldMove = event;
        return;
    }
    if (event.type == View::Pointer::up && mouse.pressed) // a quick click: let go where it was pressed too
        heldMove = View::PointerEvent{View::Pointer::move, event.x, event.y, event.modifiers};
    else
        heldMove.reset(), mouse.x = event.x, mouse.y = event.y;
    mouse.modifiers = event.modifiers;
    if (event.type == View::Pointer::down) mouse.down = true;
    if (event.type == View::Pointer::up) mouse.down = false;
    // While a control is dragged, nothing else is hovered (as in ImGui): Clay
    // keeps the pointer where the press was, however far the drag goes.
    if (active && mouse.down) layout.pointer(lastPressX, lastPressY, true);
    else layout.pointer(mouse.x, mouse.y, mouse.down);
    if (event.type == View::Pointer::leave && !mouse.down) // over nothing now
    {
        mouse.x = mouse.y = -1e9f;
        layout.pointer(mouse.x, mouse.y, false);
        return;
    }
    switch (event.type)
    {
    case View::Pointer::down:
    {
        // A second press soon after and near the first is a double-click.
        const auto now = std::chrono::steady_clock::now();
        const bool second = now - lastPress < std::chrono::milliseconds(400)
                         && std::abs(event.x - lastPressX) < 4 * s && std::abs(event.y - lastPressY) < 4 * s;
        mouse.doubleClicked |= second; // both presses may arrive before the next frame
        lastPress = second ? std::chrono::steady_clock::time_point{} : now;
        lastPressX = event.x, lastPressY = event.y;
        mouse.pressed = true;
        focusVisible = false;
        break;
    }
    case View::Pointer::up: mouse.released = true; break;
    case View::Pointer::wheel:
        mouse.wheel += event.wheel;
        mouse.scroll += event.inverted ? -event.wheel : event.wheel;
        break;
    case View::Pointer::move: case View::Pointer::leave: break;
    }
}

bool Ui::key(View::Key key, unsigned modifiers)
{
    // Tab moves between controls; the rest need a control or a text field to
    // act on, and the shortcuts a text field (else they're the host's: its undo).
    if (!focus && !editing && key != View::Key::tab) return false;
    if (!editing && key >= View::Key::undo) return false;
    // Keys the focused control has no use for are the host's: Space and
    // Delete, Enter on a knob, Escape with no menu open.
    using K = View::Key;
    const bool used = key == K::tab || (focusUses >> unsigned(key) & 1) || (key == K::escape && openMenu);
    if (!editing && !used) return false;
    keys.push_back({key, modifiers});
    if (editing) edits.push_back({{}, key, modifiers});
    focusVisible = !editing;
    return true;
}

bool Ui::text(const char *utf8)
{
    // A focused number box starts editing with what's typed, as compost's does.
    if (!editing && focus && focusTakesDigits && utf8[0] && std::strchr("0123456789.,+-", utf8[0]))
    {
        typedToFocus += utf8;
        return true;
    }
    if (!editing) return false;
    edits.push_back({utf8, {}, 0});
    return true;
}

bool Ui::files(View::FileDrag type, float x, float y, const std::vector<std::string> &paths)
{
    if (type == View::FileDrag::leave)
    {
        incoming = {};
        return false;
    }
    incoming = {type == View::FileDrag::over, type == View::FileDrag::drop, x, y, paths};
    return std::any_of(dropTargets.begin(), dropTargets.end(),
                       [&](uint32_t id) { return inside({id, 0, 0, {}}, x, y); });
}

bool Ui::inside(Clay_ElementId id, float x, float y) const
{
    const Clay_ElementData data = Clay_GetElementData(id);
    const Clay_BoundingBox &b = data.boundingBox;
    return data.found && x / s >= b.x && x / s < b.x + b.width && y / s >= b.y && y / s < b.y + b.height;
}

bool Ui::dropTarget(Clay_ElementId id, std::vector<std::string> &files)
{
    dropTargetsNow.push_back(id.id);
    if (!incoming.dropped || !inside(id, incoming.x, incoming.y)) return false;
    files = incoming.paths;
    return true;
}

bool Ui::fileOver(Clay_ElementId id) const { return incoming.over && inside(id, incoming.x, incoming.y); }

void Ui::dragSource(Clay_ElementId id, const std::function<std::vector<std::string>()> &make)
{
    const Area a = element(id, eachPress);
    if (a.pressed) dragArmed = true, dragFromX = mouse.x, dragFromY = mouse.y;
    if (a.held && dragArmed && dragFiles && std::hypot(mouse.x - dragFromX, mouse.y - dragFromY) > 4 * s)
    {
        dragArmed = false;
        const std::vector<std::string> paths = make();
        if (!paths.empty()) dragFiles(paths);
    }
    if (!a.held) dragArmed = false;
}

bool Ui::animating() const
{
    // Controls declared before a click was handled drew the old state, and
    // Clay hit-tests against the last layout: one more frame settles both.
    return tooltipWaiting || settle || scrollWaiting || draw.atlasRefilling();
}

void Ui::begin(float width, float height, float scale)
{
    s = scale;
    viewWidth = width / scale, viewHeight = height / scale;
    draw.begin(width, height);
    order.clear();
    placed.clear();
    described.clear();
    said.clear();
    layout.begin(draw, width, height, scale);
    overPopup = openMenu && Clay_PointerOver({openMenu, 0, 0, {}});
    menuSeen = false;
    lastElement = 0;
    settle = mouse.pressed || mouse.released || !keys.empty() || mouse.wheel != 0;
    tooltipWaiting = false;
}

void Ui::end(const wgpu::RenderPassEncoder &pass, const std::function<void(int, void *, Rect)> &onCustom)
{
    record(onCustom);
    draw.flush(pass);
}

bool Ui::record(const std::function<void(int, void *, Rect)> &onCustom)
{
    layout.end([&](int kind, void *data, Rect r) {
        const auto *c = static_cast<const Placed *>(data);
        if (kind == knobBox) knob(*c->params, c->p, r, c->format, c->log);
        else if (kind == sliderBox) slider(*c->params, c->p, r, c->format, c->log);
        else if (kind == vsliderBox) vslider(*c->params, c->p, r, c->format, c->log);
        else if (kind == xyBox) xyPad(*c->params, c->p, c->second, r);
        else if (kind == meterBox) meter(c->level, c->peak, r);
        else if (kind == imageKnobBox) imageKnob(*c->params, c->p, c->image, c->frames, r, c->log, c->across);
        else if (kind == pianoBox) pianoNotes = piano(c->lowest, c->highest, r, c->lit);
        else if (kind == envelopeBox) { if (envelope(*c->points, r)) changedNow.push_back(c->points); }
        else if (kind == stepsBox) { if (steps(*c->values, r)) changedNow.push_back(c->values); }
        else if (kind == chevronBox)
        {
            const float xy[] = {r.x, r.y + r.h * 0.3f, r.x + r.w / 2, r.y + r.h * 0.7f, r.x + r.w, r.y + r.h * 0.3f};
            draw.polyline(xy, 3, 1.2f * s, theme.dim);
        }
        else if (kind == chevronRightBox)
        {
            const float xy[] = {r.x + r.w * 0.3f, r.y, r.x + r.w * 0.7f, r.y + r.h / 2, r.x + r.w * 0.3f, r.y + r.h};
            draw.polyline(xy, 3, 1.2f * s, theme.dim);
        }
        else if (onCustom) onCustom(kind, data, r);
    });
    finishDescribing();
    drawGrip();
    grip = {};
    changedLast.swap(changedNow);
    changedNow.clear();
    dropTargets.swap(dropTargetsNow);
    dropTargetsNow.clear();
    incoming.dropped = false; // seen by this frame's dropTarget
    if (!menuSeen) openMenu = 0; // its dropdown is gone
    for (auto [key, modifiers] : keys)
    {
        if (key != View::Key::tab || order.empty()) continue;
        const auto at = std::find(order.begin(), order.end(), focus);
        const bool back = modifiers & View::shift;
        if (at == order.end()) focus = back ? order.back() : order.front();
        else if (back) focus = at == order.begin() ? order.back() : *(at - 1);
        else focus = at + 1 == order.end() ? order.front() : *(at + 1);
    }
    // The control being dragged wasn't declared this frame (it moved, its
    // drawer closed, the page changed): let go of it, or nothing else could
    // be pressed, its gesture would stay open and the pointer locked.
    if (active && !activeSeen)
    {
        if (held.params && held.first) held.params->end(*held.first);
        if (held.params && held.second) held.params->end(*held.second);
        if (held.locked && lockPointer) lockPointer(false);
        active = nullptr, numberDrag = 0, held = {};
    }
    activeSeen = false;
    // A press on nothing that takes focus (empty space, a label) drops it,
    // so the keys go back to the host.
    if (mouse.pressed && !pressFocused && !active) focus = nullptr;
    pressFocused = false;
    // A wheel turn no control took scrolls what's under the pointer, from the
    // next frame (Clay scrolls as a frame starts): not both a knob and its page.
    scrollWaiting = mouse.wheel != 0 && !wheelUsed;
    if (scrollWaiting) layout.wheel(mouse.scroll);
    wheelUsed = false;
    // Each press, release, wheel turn and key is seen by one frame; moves
    // held back behind a press go in now, for the next.
    mouse.pressed = mouse.released = mouse.doubleClicked = false;
    mouse.wheel = mouse.scroll = 0;
    if (const std::optional<View::PointerEvent> move = std::exchange(heldMove, std::nullopt)) pointer(*move), settle = true;
    keys.clear();
    edits.clear();
    typedToFocus.clear();
    accessClick = nullptr; // for a control no longer there
    return draw.changed();
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

static uint64_t accessId(const void *me) { return uint64_t(reinterpret_cast<uintptr_t>(me)); }
static constexpr uint64_t viewNode = 1; // the root: no control's address

Ui::Described &Ui::describe(const void *me, Accessible::Role role, const char *name, uint64_t parent)
{
    Described &d = described.emplace_back();
    d.node.id = accessId(me), d.node.role = role, d.parent = parent;
    if (name) d.node.name = name;
    d.node.actions = Accessible::focus;
    return d;
}

Ui::Described &Ui::describeParam(const void *me, Params &params, Param &p, const char *value, Rect r)
{
    Described &d = describe(me, Accessible::Role::slider, p.name.c_str());
    d.node.value = value, d.node.numeric = true, d.node.number = p.value, d.node.min = p.min, d.node.max = p.max;
    d.node.x = r.x, d.node.y = r.y, d.node.width = r.w, d.node.height = r.h;
    d.node.actions |= Accessible::increment | Accessible::decrement | Accessible::setValue;
    d.params = &params, d.param = &p;
    return d;
}

void Ui::accessible(Accessible::Role role, const char *name, const char *value, Rect r)
{
    // Its id: what it is and where, as stable as its place on the page.
    const size_t hash = std::hash<std::string_view>{}(name ? name : "") ^ (size_t(r.x) * 31 + size_t(r.y)) << 1;
    const void *me = reinterpret_cast<const void *>(uintptr_t(hash | 2)); // never the view's id (1) or null
    Described &d = describe(me, role, name);
    if (value) d.node.value = value;
    d.node.x = r.x, d.node.y = r.y, d.node.width = r.w, d.node.height = r.h;
}

void Ui::label(const char *name)
{
    if (lastElement && name) said.push_back({lastElement, name, {}});
}

void Ui::description(const char *text)
{
    if (lastElement && text) said.push_back({lastElement, {}, text});
}

void Ui::hidden()
{
    if (lastElement) said.push_back({lastElement, {}, {}, true});
}

void Ui::hidden(Clay_ElementId element) { said.push_back({element.id, {}, {}, true, true}); }

void Ui::announce(const char *text) { announcement = text ? text : ""; }

static bool same(const Accessible &a, const Accessible &b)
{
    return a.id == b.id && a.role == b.role && a.name == b.name && a.value == b.value && a.description == b.description
        && a.live == b.live && a.orientation == b.orientation && a.numeric == b.numeric
        && a.number == b.number && a.min == b.min && a.max == b.max && a.step == b.step && a.checked == b.checked
        && a.expanded == b.expanded && a.selected == b.selected && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height
        && a.actions == b.actions && a.children == b.children;
}

void Ui::finishDescribing()
{
    // Where each element was laid out, and in what order: reading order is
    // layout order, as a page's is document order.
    const std::vector<Layout::Drawn> &drawn = layout.drawn;
    auto inside = [](const Rect &r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
    auto within = [&](const Accessible &n, const Rect &r) { return inside(r, n.x + n.width / 2, n.y + n.height / 2); };
    auto contains = [](const Rect &outer, const Rect &r) { // with half a pixel to spare
        return r.x >= outer.x - 0.5f && r.y >= outer.y - 0.5f && r.x + r.w <= outer.x + outer.w + 0.5f && r.y + r.h <= outer.y + outer.h + 0.5f;
    };
    // An element's box (pixels), whether or not it draws anything itself.
    auto box = [&](uint32_t element) -> std::optional<Rect> {
        const Clay_ElementData data = Clay_GetElementData({element, 0, 0, {}});
        if (!data.found) return std::nullopt;
        const Clay_BoundingBox &b = data.boundingBox;
        return Rect{b.x * s, b.y * s, b.width * s, b.height * s};
    };
    // Its place in reading order: the first thing drawn in it (a button's
    // label comes before its border).
    auto place = [&](const Rect &r) {
        for (size_t i = 0; i < drawn.size(); ++i)
            if (r.w > 0 && contains(r, drawn[i].pixels)) return float(i);
        return -1.0f;
    };

    struct Entry { Accessible node; float order; uint64_t parent; bool hidden; };
    std::vector<Entry> entries;
    for (Described &d : described)
    {
        float reading = 1e9f + float(entries.size());
        if (d.element) // laid out by Clay: its box, and its place
        {
            if (const std::optional<Rect> r = box(d.element))
            {
                d.node.x = r->x, d.node.y = r->y, d.node.width = r->w, d.node.height = r->h;
                if (const float at = place(*r); at >= 0) reading = at;
            }
        }
        else // drawn at a rectangle: the place of the smallest box around it (not a menu floating over it)
        {
            float smallest = 0;
            for (size_t i = 0; i < drawn.size(); ++i)
            {
                const float area = drawn[i].pixels.w * drawn[i].pixels.h;
                if (!drawn[i].text && within(d.node, drawn[i].pixels) && (smallest == 0 || area < smallest))
                    smallest = area, reading = float(i) + 0.5f;
            }
        }
        entries.push_back({d.node, reading, d.parent, false});
    }
    // What the page says around them: its text runs, those a control doesn't
    // already say (a button's label, a knob's value), a line's runs as one.
    for (size_t i = 0; i < drawn.size(); ++i)
    {
        if (!drawn[i].text) continue;
        Accessible text;
        text.id = 0x7465787400000000ull | drawn[i].id; // "text", above any address
        text.role = Accessible::Role::label; // its words are its value, as AccessKit reads a label
        text.value = drawn[i].words;
        Rect r = drawn[i].pixels;
        const size_t first = i;
        auto nextLine = [&](const Layout::Drawn &line, const Layout::Drawn &next) { // of a wrapped paragraph
            return next.text && (next.id == line.id || (std::abs(next.pixels.x - drawn[first].pixels.x) < 1
                                                       && std::abs(next.pixels.y - (line.pixels.y + line.pixels.h)) < line.pixels.h / 4));
        };
        while (i + 1 < drawn.size() && nextLine(drawn[i], drawn[i + 1]))
        {
            ++i;
            text.value += " " + drawn[i].words;
            const float right = std::max(r.x + r.w, drawn[i].pixels.x + drawn[i].pixels.w), bottom = drawn[i].pixels.y + drawn[i].pixels.h;
            r.x = std::min(r.x, drawn[i].pixels.x), r.w = right - r.x, r.h = bottom - r.y;
        }
        text.x = r.x, text.y = r.y, text.width = r.w, text.height = r.h;
        bool spoken = false;
        for (const Entry &e : entries) spoken |= e.node.width > 0 && inside({e.node.x, e.node.y, e.node.width, e.node.height}, r.x + r.w / 2, r.y + r.h / 2);
        if (!spoken) entries.push_back({text, float(first), 0, false});
    }
    // label, description, hidden: on the control whose element they name, or
    // that was drawn inside it (a knob in its box). A tooltip's description
    // gives way to one set by hand.
    for (const Said &w : said)
    {
        const std::optional<Rect> r = box(w.element);
        for (Entry &e : entries)
        {
            const Rect at{e.node.x, e.node.y, e.node.width, e.node.height};
            const bool mine = e.node.id == uint64_t(w.element)
                           || (r && e.parent == 0 && (w.everything ? contains(*r, at) : within(e.node, *r)));
            if (!mine || (w.everything ? false : e.node.role == Accessible::Role::label)) continue;
            if (w.hidden) e.hidden = true;
            if (!w.label.empty()) e.node.name = w.label;
            if (!w.description.empty() && !(w.fromTooltip && !e.node.description.empty())) e.node.description = w.description;
            if (e.node.description == e.node.name) e.node.description.clear(); // said once is enough
            if (!w.everything) break;
        }
    }

    Accessibility tree;
    Accessible &view = tree.nodes.emplace_back();
    view.id = viewNode, view.role = Accessible::Role::window;
    view.width = viewWidth * s, view.height = viewHeight * s;
    std::vector<const Entry *> top;
    for (const Entry &e : entries)
    {
        if (e.hidden) continue;
        tree.nodes.push_back(e.node);
        if (!e.parent) top.push_back(&e);
    }
    std::stable_sort(top.begin(), top.end(), [](const Entry *a, const Entry *b) { return a->order < b->order; });
    for (const Entry *e : top) tree.nodes.front().children.push_back(e->node.id);
    for (const Entry &e : entries)
        if (e.parent && !e.hidden)
            for (Accessible &n : tree.nodes)
                if (n.id == e.parent) { n.children.push_back(e.node.id); break; }
    if (!announcement.empty()) // a live region: screen readers read it as it changes
    {
        Accessible &live = tree.nodes.emplace_back();
        live.id = 3, live.role = Accessible::Role::label, live.value = announcement, live.live = true;
        tree.nodes.front().children.push_back(live.id);
    }
    tree.focus = viewNode;
    for (const Accessible &n : tree.nodes)
        if (n.id == accessId(focus)) tree.focus = n.id;
    // A focused radio group: its checked radio has the focus, as the
    // WAI-ARIA radio group pattern has it.
    for (const Accessible &n : tree.nodes)
        if (n.id == tree.focus && n.role == Accessible::Role::radioGroup)
            for (uint64_t child : n.children)
                for (const Accessible &c : tree.nodes)
                    if (c.id == child && c.checked == 1) tree.focus = c.id;
#ifndef NDEBUG
    // Every control needs a name a screen reader can say (WCAG 4.1.2).
    for (const Accessible &n : tree.nodes)
        if (n.name.empty() && n.actions & Accessible::focus && n.role != Accessible::Role::window)
        {
            static std::vector<uint64_t> warned;
            if (std::find(warned.begin(), warned.end(), n.id) != warned.end()) continue;
            warned.push_back(n.id);
            static const char *const roles[] = {"window", "group", "button", "check box", "radio group", "radio button", "combo box (dropdown)",
                                                "slider", "spin button (number box)", "text field", "label", "image", "list box", "option"};
            std::fprintf(stderr, "draw2d: a %s has no name for screen readers: give it ui.label(\"...\")\n", roles[int(n.role)]);
        }
#endif
    const bool unchanged = tree.focus == accessTree.focus && tree.nodes.size() == accessTree.nodes.size()
        && std::equal(tree.nodes.begin(), tree.nodes.end(), accessTree.nodes.begin(), same);
    if (unchanged) return;
    tree.version = accessTree.version + 1;
    accessTree = std::move(tree);
}

void Ui::accessAction(uint64_t id, Accessible::Action action, double value)
{
    const auto d = std::find_if(described.begin(), described.end(), [&](const Described &d) { return d.node.id == id; });
    if (d == described.end()) return;
    const void *me = reinterpret_cast<const void *>(uintptr_t(id));
    switch (action)
    {
    case Accessible::focus: focus = me, focusVisible = true; break;
    case Accessible::click: focus = me, accessClick = me; break;
    case Accessible::increment: // as the arrow keys would, on the focused control
    case Accessible::decrement:
        focus = me, focusVisible = true;
        keys.push_back({action == Accessible::increment ? View::Key::up : View::Key::down, 0});
        break;
    case Accessible::setValue:
        if (d->param) set(*d->params, *d->param, value);
        break;
    case Accessible::expand: // a dropdown or drawer, opened by its click when it's closed
    case Accessible::collapse:
        focus = me;
        if (d->node.expanded != (action == Accessible::expand)) accessClick = me;
        break;
    }
}

bool Ui::over(float x, float y, float w, float h) const
{
    return !overPopup && mouse.x >= x && mouse.x <= x + w && mouse.y >= y && mouse.y <= y + h;
}

void Ui::set(Params &params, Param &p, double value)
{
    params.begin(p);
    params.change(p, value);
    params.end(p);
}

// A control's identity: its parameter and where it is, so a knob and a slider
// of one parameter drag, focus and step apart.
const void *Ui::controlId(const Param &p, float x, float y)
{
    uint32_t bits[2];
    std::memcpy(bits, &x, 4), std::memcpy(bits + 1, &y, 4);
    const uintptr_t where = (uintptr_t(bits[0]) * 0x9E3779B1u) ^ bits[1];
    return reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(&p) ^ (where << 3));
}

bool Ui::control(Params &params, Param &p, const void *me, float x, float y, float w, float h, bool vertical, float travel, bool log,
                 bool typable)
{
    order.push_back(me);
    const bool fine = mouse.modifiers & View::shift;
    const double unit = toUnit(p, p.value, log);

    // Knobs and sliders step, and Escape, Delete or Backspace reset them (as
    // double-click does), as compost's do; one with a value to type also
    // takes Enter and digits (typeInto).
    if (focus == me)
        focusUses = stepKeys | keyBit(View::Key::escape) | keyBit(View::Key::backspace) | keyBit(View::Key::deleteForward)
                  | (typable ? keyBit(View::Key::enter) : 0),
        focusTakesDigits = typable;
    if (!active && over(x, y, w, h))
    {
        if (mouse.pressed) focus = me, pressFocused = true;
        if (mouse.pressed && mouse.doubleClicked)
            set(params, p, p.defaultValue);
        else if (mouse.pressed)
        {
            active = me;
            grabX = mouse.x, grabY = mouse.y, grabUnit = unit, grabFine = fine;
            params.begin(p);
            if (lockPointer) lockPointer(true);
            held = {&params, &p, nullptr, true};
        }
        else if (mouse.wheel != 0)
            set(params, p, fromUnit(p, unit + mouse.wheel * (fine ? 0.002 : 0.02), log)), wheelUsed = true;
    }

    if (focus == me)
        for (auto [key, modifiers] : keys)
        {
            // Shift for fine, Alt for a page's step (compost's large step).
            const double step = modifiers & View::shift ? 0.001 : modifiers & View::alt ? 0.1 : 0.01, u = toUnit(p, p.value, log);
            switch (key)
            {
            case View::Key::up: case View::Key::right: set(params, p, fromUnit(p, u + step, log)); break;
            case View::Key::down: case View::Key::left: set(params, p, fromUnit(p, u - step, log)); break;
            case View::Key::pageUp: set(params, p, fromUnit(p, u + 0.1, log)); break;
            case View::Key::pageDown: set(params, p, fromUnit(p, u - 0.1, log)); break;
            case View::Key::home: set(params, p, p.min); break;
            case View::Key::end: set(params, p, p.max); break;
            case View::Key::escape: case View::Key::backspace: case View::Key::deleteForward: set(params, p, p.defaultValue); break;
            default: break; // Tab: in end()
            }
        }

    if (active != me) return false;
    activeSeen = true;
    // Switching to or from fine mid-drag continues from where the value is.
    if (fine != grabFine) grabX = mouse.x, grabY = mouse.y, grabUnit = unit, grabFine = fine;
    const double moved = (vertical ? grabY - mouse.y : mouse.x - grabX) / (travel * (fine ? 10 : 1));
    const double value = fromUnit(p, grabUnit + moved, log);
    if (value != p.value) params.change(p, value);
    if (!mouse.down)
    {
        params.end(p);
        active = nullptr, held = {};
        if (lockPointer) lockPointer(false);
    }
    return true;
}

void Ui::knob(Params &params, Param *p, Rect box, const char *format, bool log)
{
    if (!p) return;
    // The dial fills the width, leaving room below for two lines of text.
    const float size = theme.textSize * s;
    const float r = std::min(box.w, box.h - 3 * size) / 2, cx = box.x + box.w / 2, cy = box.y + r;
    const void *me = controlId(*p, cx, cy);
    const bool dragging = control(params, *p, me, cx - r, cy - r, 2 * r, 2 * r, true, 200 * s, log, true);
    // 270 degrees of travel, from 7:30 to 4:30; the ring a tenth of the dial (as compost's knob).
    const float from = 3.927f, sweep = 4.712f, u = float(toUnit(*p, p->value, log));
    const float width = std::max(2.0f * s, r * 0.19f);
    if (focusShown(me)) draw.circle(cx, cy, r + 2 * s, {0, 0, 0, 0}, 1 * s, theme.accent);
    draw.arc(cx, cy, r - width, width, from, from + sweep, theme.track);
    if (u > 0) draw.arc(cx, cy, r - width, width, from, from + sweep * u, theme.accent);
    const float angle = from + sweep * u, inner = r - 1.6f * width;
    draw.circle(cx, cy, inner, theme.background, 1 * s, theme.edge);
    draw.line(cx + std::sin(angle) * inner * 0.3f, cy - std::cos(angle) * inner * 0.3f,
              cx + std::sin(angle) * inner * 0.9f, cy - std::cos(angle) * inner * 0.9f, (dragging ? 2.0f : 1.4f) * s,
              dragging ? theme.accent : theme.text);
    char value[32];
    std::snprintf(value, sizeof(value), format, p->value);
    describeParam(me, params, *p, value, box).node.orientation = 2; // dragged up and down
    typeInto(me, &p->defaultValue, *p);
    if (!valueEntry(params, *p, &p->defaultValue, {box.x, cy + r + 0.3f * size, box.w, 1.3f * size}, Draw2D::Align::center))
        draw.text(cx, cy + r + 1.3f * size, size, value, theme.text, Draw2D::Align::center, theme.valueFont);
    draw.text(cx, cy + r + 2.6f * size, size, p->name, theme.dim, Draw2D::Align::center, theme.labelFont);
}

// As compost's slider: the name on the left and the value on the right above
// a 3-point track, filled with the accent up to the value, and a 2-point
// thumb line across it.
void Ui::slider(Params &params, Param *p, Rect box, const char *format, bool log)
{
    if (!p) return;
    const float px = box.x, py = box.y, w = box.w, h = box.h, size = theme.textSize * s;
    char value[32];
    std::snprintf(value, sizeof(value), format, p->value);
    // The value first: a click on it types, rather than starting a drag.
    const float valueWidth = std::max(draw.measure(size, value, theme.valueFont), 3 * size) + 4 * s;
    const void *me = controlId(*p, px, py);
    typeInto(me, &p->max, *p);
    const bool typing = valueEntry(params, *p, &p->max, {px + w - valueWidth, py, valueWidth, 1.4f * size}, Draw2D::Align::right);
    describeParam(me, params, *p, value, box).node.orientation = 1;
    const bool dragging = control(params, *p, me, px, py, w, h, false, w, log, true);
    const bool hot = dragging || (!active && over(px, py, w, h));
    const float u = float(toUnit(*p, p->value, log)), thumb = 1.125f * size, track = py + h - thumb / 2;
    if (focusShown(me)) draw.rect(px - 3 * s, py - 2 * s, w + 6 * s, h + 4 * s, theme.radius * s, {0, 0, 0, 0}, 1 * s, theme.accent);
    draw.text(px, py + size, size, p->name, theme.dim, Draw2D::Align::left, theme.labelFont);
    if (!typing) draw.text(px + w, py + size, size, value, theme.dim, Draw2D::Align::right, theme.valueFont);
    draw.rect(px, track - 1.5f * s, w, 3 * s, 0, theme.track);
    draw.rect(px, track - 1.5f * s, w * u, 3 * s, 0, theme.accent);
    const float x = px + w * u, thumbWidth = (hot ? 3.0f : 2.0f) * s;
    draw.rect(x - thumbWidth / 2, track - thumb / 2, thumbWidth, thumb, 0, theme.text);
}

void Ui::resizeGrip(float width, float height)
{
    grip = {width, height}; // drawn in end(), on top of the layout
}

void Ui::drawGrip()
{
    if (!resizeTo || grip.w <= 0) return;
    const float width = grip.w, height = grip.h, size = 14 * s;
    const auto a = area(&gripOffsetX, width - size, height - size, size, size);
    if (a.pressed) gripOffsetX = width - a.x, gripOffsetY = height - a.y;
    if (a.held) resizeTo(a.x + gripOffsetX, a.y + gripOffsetY);
    // Three short diagonal lines, as windows' resize corners have.
    const Color color = a.held ? theme.accent : theme.dim;
    for (float d : {4.0f, 8.0f, 12.0f})
        draw.line(width - 3 * s - d * s, height - 3 * s, width - 3 * s, height - 3 * s - d * s, 1.2f * s, color);
}

Ui::Area Ui::area(const void *id, float x, float y, float w, float h, Presses presses)
{
    return press(id, over(x, y, w, h), presses);
}

Ui::Area Ui::element(Clay_ElementId id, Presses presses)
{
    // Clay ids are hashes, so they serve as the control's identity too.
    return press(reinterpret_cast<const void *>(uintptr_t(id.id)), Clay_PointerOver(id), presses);
}

Ui::Area Ui::press(const void *id, bool isOver, Presses presses)
{
    Area result;
    result.modifiers = mouse.modifiers;
    if (accessClick == id) result.pressed = result.clicked = true, accessClick = nullptr; // a press and release in one
    if (!active && isOver)
    {
        if (mouse.pressed && mouse.doubleClicked && presses == doubleClicks) result.doubleClicked = true;
        else if (mouse.pressed) active = id, result.pressed = true;
        result.wheel = mouse.wheel;
    }
    if (active != id) return result;
    activeSeen = true;
    result.held = true, result.x = mouse.x, result.y = mouse.y;
    if (!mouse.down) result.released = true, result.clicked = isOver, active = nullptr;
    return result;
}

}

namespace gpu {

Clay_ElementId Ui::id(const char *kind, int index)
{
    return Clay_GetElementIdWithIndex({false, int32_t(std::strlen(kind)), kind}, uint32_t(index));
}

Clay_TextElementConfig *Ui::textConfig(Color color, int font)
{
    return Clay__StoreTextElementConfig({.textColor = clayColor(color), .fontId = uint16_t(font < 0 ? theme.labelFont : font),
                                         .fontSize = uint16_t(theme.textSize), .wrapMode = CLAY_TEXT_WRAP_NONE});
}

// Puts a Clay control in the Tab order and gives it focus when clicked;
// returns the keys it gets this frame, if it has focus.
std::vector<View::Key> Ui::focusable(Clay_ElementId element, const Area &a)
{
    const void *me = reinterpret_cast<const void *>(uintptr_t(element.id));
    order.push_back(me);
    if (a.pressed) focus = me, pressFocused = true;
    if (focus == me) // buttons, toggles, segmented, dropdowns: Enter, and arrows to choose
        focusUses = keyBit(View::Key::enter) | keyBit(View::Key::left) | keyBit(View::Key::right) | keyBit(View::Key::up)
                  | keyBit(View::Key::down),
        focusTakesDigits = false;
    std::vector<View::Key> mine;
    if (focus == me)
        for (auto [key, modifiers] : keys)
            if (key != View::Key::tab) mine.push_back(key);
    return mine;
}

bool Ui::focusShown(Clay_ElementId element) const
{
    return focusShown(reinterpret_cast<const void *>(uintptr_t(element.id)));
}

bool Ui::button(const char *label, int index)
{
    const Clay_ElementId bid = id(label, index);
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(bid.id)), Accessible::Role::button, label);
    d.element = bid.id, d.node.actions |= Accessible::click;
    const Area a = element(bid, eachPress);
    bool clicked = a.clicked;
    for (View::Key key : focusable(bid, a)) clicked |= key == View::Key::enter;
    const bool hot = Clay_PointerOver(bid);
    CLAY({.id = bid, .layout = {.padding = {10, 10, 4, 4}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = clayColor(a.held ? theme.text : hot ? theme.panel : Color{0, 0, 0, 0}),
          .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .border = {.color = clayColor(focusShown(bid) ? theme.accent : hot ? theme.text : theme.edge), .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        CLAY_TEXT(layout.text(label), textConfig(a.held ? theme.background : theme.text));
    }
    lastElement = bid.id;
    return clicked;
}

// Off: an outline. On: filled with the text colour, its label in the
// background's (Slide's Sync button).
bool Ui::toggle(const char *label, bool &on, int index)
{
    const Clay_ElementId tid = id(label, index);
    const Area a = element(tid, eachPress);
    bool clicked = a.clicked;
    for (View::Key key : focusable(tid, a)) clicked |= key == View::Key::enter;
    if (clicked) on = !on;
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(tid.id)), Accessible::Role::checkBox, label);
    d.element = tid.id, d.node.checked = on, d.node.actions |= Accessible::click;
    const bool hot = Clay_PointerOver(tid);
    CLAY({.id = tid, .layout = {.padding = {7, 7, 3, 3}, .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = clayColor(on ? theme.text : hot ? theme.panel : Color{0, 0, 0, 0}),
          .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .border = {.color = clayColor(focusShown(tid) ? theme.accent : on || a.held ? theme.text : theme.edge),
                     .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        CLAY_TEXT(layout.text(label), textConfig(on ? theme.background : theme.text));
    }
    lastElement = tid.id;
    return clicked;
}

bool Ui::segmented(const char *const *items, int count, int &selected, int index)
{
    bool changed = false;
    const Clay_ElementId sid = id("Segmented", index);
    CLAY({.id = sid, .border = {.color = clayColor(focusShown(sid) ? theme.accent : theme.edge), .width = CLAY_BORDER_ALL(1)}})
    {
        // Clicks and keys first, so every cell draws the new choice.
        Area any;
        for (int i = 0; i < count; ++i)
        {
            const Area a = element(id("Segment", index * 64 + i), eachPress);
            if (a.clicked && selected != i) selected = i, changed = true;
            any.pressed |= a.pressed;
        }
        for (View::Key key : focusable(sid, any))
        {
            const int step = key == View::Key::right || key == View::Key::down ? 1 : key == View::Key::left || key == View::Key::up ? -1 : 0;
            const int next = std::clamp(selected + step, 0, count - 1);
            if (next != selected) selected = next, changed = true;
        }
        const void *group = reinterpret_cast<const void *>(uintptr_t(sid.id));
        describe(group, Accessible::Role::radioGroup, nullptr).element = sid.id;
        for (int i = 0; i < count; ++i)
        {
            const Clay_ElementId cell = id("Segment", index * 64 + i);
            Described &d = describe(reinterpret_cast<const void *>(uintptr_t(cell.id)), Accessible::Role::radioButton, items[i], accessId(group));
            d.element = cell.id, d.node.checked = selected == i, d.node.actions |= Accessible::click;
        }
        for (int i = 0; i < count; ++i)
        {
            const Clay_ElementId cell = id("Segment", index * 64 + i);
            const bool on = selected == i, hot = Clay_PointerOver(cell);
            CLAY({.id = cell, .layout = {.padding = {8, 8, 3, 3}},
                  .backgroundColor = clayColor(on ? theme.text : hot ? theme.panel : Color{0, 0, 0, 0})})
            {
                CLAY_TEXT(layout.text(items[i]), textConfig(on ? theme.background : hot ? theme.text : theme.dim));
            }
        }
    }
    lastElement = sid.id;
    return changed;
}

bool Ui::dropdown(const char *const *items, int count, int &selected, int index)
{
    bool changed = false;
    const Clay_ElementId did = id("Dropdown", index), menu = id("DropdownMenu", index);
    const bool open = openMenu == menu.id;
    const Area a = element(did, eachPress);
    if (a.pressed) openMenu = open ? 0 : menu.id;
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(did.id)), Accessible::Role::comboBox, nullptr);
    d.element = did.id, d.node.value = items[std::clamp(selected, 0, count - 1)], d.node.expanded = openMenu == menu.id;
    d.node.actions |= Accessible::click | Accessible::expand | Accessible::collapse;
    // Keys: Enter opens and closes it, Escape closes it, the arrows choose.
    for (View::Key key : focusable(did, a))
    {
        if (key == View::Key::enter) openMenu = openMenu == menu.id ? 0 : menu.id;
        if (key == View::Key::escape) openMenu = 0;
        const int step = key == View::Key::down ? 1 : key == View::Key::up ? -1 : 0;
        const int next = std::clamp(selected + step, 0, count - 1);
        if (step && next != selected) selected = next, changed = true;
    }
    // A press anywhere else closes it.
    if (open && mouse.pressed && !Clay_PointerOver(menu) && !Clay_PointerOver(did)) openMenu = 0;
    const bool hot = Clay_PointerOver(did);
    // The menu opens below, or above where there's no room below (sizes from
    // the last frame, in points).
    const Clay_BoundingBox box = Clay_GetElementData(did).boundingBox;
    const float menuHeight = float(count) * (theme.textSize * 1.25f + 6) + 4;
    const bool up = box.y + box.height + menuHeight > viewHeight && box.y - menuHeight >= 0;
    CLAY({.id = did, .layout = {.padding = {7, 7, 3, 3}, .childGap = 8, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = clayColor(hot || open ? theme.panel : Color{0, 0, 0, 0}),
          .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .border = {.color = clayColor(focusShown(did) ? theme.accent : open || hot ? theme.text : theme.edge),
                     .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        CLAY_TEXT(layout.text(selected >= 0 && selected < count ? items[selected] : ""), textConfig(theme.text));
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(7), CLAY_SIZING_FIXED(7)}}, .custom = {.customData = layout.custom(chevronBox)}}) {}
        if (openMenu == menu.id)
        {
            menuSeen = true;
            // Its list, for screen readers: the options, the chosen one selected.
            const void *list = reinterpret_cast<const void *>(uintptr_t(menu.id));
            Described &listBox = describe(list, Accessible::Role::listBox, nullptr, accessId(reinterpret_cast<const void *>(uintptr_t(did.id))));
            listBox.element = menu.id, listBox.node.actions = 0; // the dropdown names it and has the focus
            for (int i = 0; i < count; ++i)
            {
                const Clay_ElementId item = id("DropdownItem", index * 1024 + i);
                Described &option = describe(reinterpret_cast<const void *>(uintptr_t(item.id)), Accessible::Role::option, items[i], accessId(list));
                option.element = item.id, option.node.selected = i == selected, option.node.actions |= Accessible::click;
            }
            CLAY({.id = menu, .layout = {.sizing = {.width = CLAY_SIZING_FIT(box.width)}, .padding = CLAY_PADDING_ALL(1),
                                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
                  .backgroundColor = clayColor(theme.background), .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
                  .floating = {.offset = {0, up ? -2.0f : 2.0f}, .zIndex = 100,
                               .attachPoints = {up ? CLAY_ATTACH_POINT_LEFT_BOTTOM : CLAY_ATTACH_POINT_LEFT_TOP,
                                                up ? CLAY_ATTACH_POINT_LEFT_TOP : CLAY_ATTACH_POINT_LEFT_BOTTOM},
                               .attachTo = CLAY_ATTACH_TO_PARENT},
                  .border = {.color = clayColor(theme.edge), .width = CLAY_BORDER_OUTSIDE(1)},
                  .userData = layout.shadow(theme.shadow, 10, 0, 3)})
            {
                for (int i = 0; i < count; ++i)
                {
                    const Clay_ElementId item = id("DropdownItem", index * 1024 + i);
                    if (element(item, eachPress).clicked) selected = i, changed = true, openMenu = 0;
                    const bool itemHot = Clay_PointerOver(item);
                    CLAY({.id = item, .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {7, 7, 3, 3}},
                          .backgroundColor = clayColor(itemHot ? theme.panel : Color{0, 0, 0, 0})})
                    {
                        CLAY_TEXT(layout.text(items[i]), textConfig(i == selected ? theme.accent : theme.text));
                    }
                }
            }
        }
    }
    lastElement = did.id;
    return changed;
}

void Ui::tooltip(const char *text)
{
    if (!lastElement) return;
    said.push_back({lastElement, {}, text ? text : "", false, false, true}); // as HTML's title: its description
    const Clay_ElementId target{lastElement, 0, 0, {}};
    if (!Clay_PointerOver(target) || mouse.down || active)
    {
        if (hovered == lastElement) hovered = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (hovered != lastElement) hovered = lastElement, hoveredSince = now;
    if (now - hoveredSince < std::chrono::milliseconds(500))
    {
        tooltipWaiting = true; // the presenter keeps drawing until it shows
        return;
    }
    // Below the control, left-aligned with it; flipped where that would leave the view.
    const Clay_BoundingBox box = Clay_GetElementData(target).boundingBox;
    const float tipWidth = draw.measure(theme.textSize, text, theme.labelFont) + 12;
    const float tipHeight = theme.textSize * 1.25f + 6;
    const bool left = box.x + tipWidth > viewWidth, up = box.y + box.height + 4 + tipHeight > viewHeight;
    const Clay_FloatingAttachPointType element = up ? (left ? CLAY_ATTACH_POINT_RIGHT_BOTTOM : CLAY_ATTACH_POINT_LEFT_BOTTOM)
                                                    : (left ? CLAY_ATTACH_POINT_RIGHT_TOP : CLAY_ATTACH_POINT_LEFT_TOP);
    const Clay_FloatingAttachPointType parent = up ? (left ? CLAY_ATTACH_POINT_RIGHT_TOP : CLAY_ATTACH_POINT_LEFT_TOP)
                                                   : (left ? CLAY_ATTACH_POINT_RIGHT_BOTTOM : CLAY_ATTACH_POINT_LEFT_BOTTOM);
    CLAY({.layout = {.padding = {6, 6, 3, 3}}, .backgroundColor = clayColor(theme.text),
          .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .floating = {.offset = {0, up ? -4.0f : 4.0f}, .parentId = lastElement, .zIndex = 200,
                       .attachPoints = {element, parent},
                       .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                       .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID},
          .userData = layout.shadow(theme.shadow, 6, 0, 2)})
    {
        CLAY_TEXT(layout.text(text), textConfig(theme.background));
    }
}

// Applies this frame's typing to the field being edited, if it's this one.
// Returns true when the edit is committed with a changed value.
bool Ui::editKeys(uint32_t field, std::string &value, bool overField, float textX)
{
    if (editing != field) return false;
    TextEdit &t = *textEdit;
    TextBuffer &b = t.buffer;
    b.width = [this](char32_t c) {
        const std::string one = toUtf8(std::u32string_view(&c, 1));
        return draw.measure(theme.textSize, one, theme.valueFont);
    };
    bool committed = false;
    auto finish = [&](bool keep) {
        draft = toUtf8(b.text);
        if (keep && draft != value) value = draft, committed = true;
        editing = 0;
    };
    auto selected = [&] {
        const int from = std::min(t.state.select_start, t.state.select_end), to = std::max(t.state.select_start, t.state.select_end);
        return toUtf8(std::u32string_view(b.text).substr(size_t(from), size_t(to - from)));
    };
    if (mouse.pressed && !overField) finish(true);
    else if (!std::isnan(textX))
    {
        const float x = mouse.x / s - textX;
        if (mouse.pressed && mouse.doubleClicked) // the whole text, as a word would be one in a short field
            t.state.select_start = 0, t.state.select_end = t.state.cursor = int(b.text.size()), t.selecting = false;
        else if (mouse.pressed) stb_textedit_click(&b, &t.state, x, 0), t.selecting = true;
        if (mouse.pressed) t.typing = -1;
        else if (mouse.down && t.selecting) stb_textedit_drag(&b, &t.state, x, 0);
        if (!mouse.down) t.selecting = false;
    }
    for (const auto &[text, key, modifiers] : edits) // in the order they came
    {
        if (editing != field) break;
        if (!text.empty())
        {
            std::u32string typed = toCodepoints(text);
            std::erase_if(typed, [](char32_t c) { return c < 0x20 || c == 0x7F; }); // one line
            if (typed.empty()) continue;
            const int before = t.state.undostate.undo_point;
            if (t.typing < 0 || t.typing >= before) t.typing = before;
            stb_textedit_paste(&b, &t.state, typed.data(), int(typed.size()));
            joinUndo(t.state.undostate, t.typing);
            continue;
        }
        t.typing = -1;
        const int shift = modifiers & View::shift ? STB_TEXTEDIT_K_SHIFT : 0;
        const bool word = modifiers & (View::alt | View::control), line = modifiers & View::command;
        switch (key)
        {
        case View::Key::left: stb_textedit_key(&b, &t.state, (line ? STB_TEXTEDIT_K_LINESTART : word ? STB_TEXTEDIT_K_WORDLEFT : STB_TEXTEDIT_K_LEFT) | shift); break;
        case View::Key::right: stb_textedit_key(&b, &t.state, (line ? STB_TEXTEDIT_K_LINEEND : word ? STB_TEXTEDIT_K_WORDRIGHT : STB_TEXTEDIT_K_RIGHT) | shift); break;
        case View::Key::up: case View::Key::home: case View::Key::pageUp: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_TEXTSTART | shift); break;
        case View::Key::down: case View::Key::end: case View::Key::pageDown: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_TEXTEND | shift); break;
        case View::Key::backspace: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_BACKSPACE); break;
        case View::Key::deleteForward: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_DELETE); break;
        case View::Key::undo: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_UNDO); break;
        case View::Key::redo: stb_textedit_key(&b, &t.state, STB_TEXTEDIT_K_REDO); break;
        case View::Key::selectAll: t.state.select_start = 0, t.state.select_end = t.state.cursor = int(b.text.size()); break;
        case View::Key::copy: case View::Key::cut:
            if (t.state.select_start != t.state.select_end)
            {
                if (copyText) copyText(selected());
                if (key == View::Key::cut) stb_textedit_cut(&b, &t.state);
            }
            break;
        case View::Key::enter: finish(true); break;
        case View::Key::escape: finish(false); break;
        default: break;
        }
    }
    if (editing == field)
    {
        draft = toUtf8(b.text);
        auto bytes = [&](int i) { return toUtf8(std::u32string_view(b.text).substr(0, size_t(std::clamp(i, 0, int(b.text.size()))))).size(); };
        caret = bytes(t.state.cursor);
        selectFrom = bytes(std::min(t.state.select_start, t.state.select_end));
        selectTo = bytes(std::max(t.state.select_start, t.state.select_end));
    }
    return committed;
}

void Ui::startEditing(uint32_t field, std::string text, bool selectAll)
{
    if (editing && editing != field) pendingField = editing, pendingDraft = draft; // see textField
    editing = field, draft = std::move(text), focus = nullptr, fieldScroll = 0;
    TextEdit &t = *textEdit;
    t.buffer.text = toCodepoints(draft);
    t.typing = -1;
    stb_textedit_initialize_state(&t.state, 1);
    const int length = int(t.buffer.text.size());
    t.state.cursor = length;
    if (selectAll) t.state.select_start = 0, t.state.select_end = length;
    t.selecting = false;
    caret = draft.size(), selectFrom = selectAll ? 0 : caret, selectTo = caret;
}

// A value as it's typed: 2.5, not 2.500000.
// Typed and shown values use '.' whatever locale the host set (a decimal
// comma would show "2,5" and read "2.5" as 2); a typed ',' counts as '.'.
static char decimalPoint() { return *std::localeconv()->decimal_point; }

static bool parseNumber(std::string text, double &value)
{
    for (char &c : text)
        if (c == '.' || c == ',') c = decimalPoint();
    char *end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str();
}

std::string Ui::plainNumber(double value)
{
    char formatted[64];
    std::snprintf(formatted, sizeof(formatted), "%.6f", value);
    std::string text = formatted;
    std::replace(text.begin(), text.end(), decimalPoint(), '.');
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text == "-0" ? "0" : text;
}

// A knob's or slider's value, which a click opens for typing (as compost's
// editable values): returns true while it's being edited, drawn here.
// id tells apart controls of the same parameter (a knob and a slider of it).
// A focused knob or slider: Enter opens its value for typing, and a digit
// starts typing a new one, as a number box does.
void Ui::typeInto(const void *me, const void *entry, const Param &p)
{
    if (focus != me) return;
    const uint32_t field = uint32_t(reinterpret_cast<uintptr_t>(entry)) | 1u;
    if (editing == field) return;
    if (!typedToFocus.empty()) startEditing(field, std::exchange(typedToFocus, {}));
    const auto enter = std::find_if(keys.begin(), keys.end(), [](auto k) { return k.first == View::Key::enter; });
    if (enter != keys.end()) keys.erase(enter), startEditing(field, plainNumber(p.value), true); // not also its commit
}

bool Ui::valueEntry(Params &params, Param &p, const void *id, Rect r, Draw2D::Align align)
{
    const uint32_t field = uint32_t(reinterpret_cast<uintptr_t>(id)) | 1u;
    const bool isOver = over(r.x, r.y, r.w, r.h);
    if (press(id, isOver, doubleClicks).pressed && editing != field) startEditing(field, plainNumber(p.value), true);
    std::string typed = plainNumber(p.value);
    if (editKeys(field, typed, isOver))
    {
        if (double value; parseNumber(typed, value)) set(params, p, value);
    }
    if (editing != field) return false;
    const float size = theme.textSize * s, width = draw.measure(size, draft, theme.valueFont);
    const float x = align == Draw2D::Align::center ? r.x + (r.w - width) / 2 : align == Draw2D::Align::right ? r.x + r.w - width : r.x;
    draw.rect(std::min(x, r.x + r.w / 2) - 3 * s, r.y, std::max(width, r.w / 2) + 6 * s, r.h, theme.radius * s, theme.background,
              1 * s, theme.accent);
    if (selectTo > selectFrom)
    {
        const float from = x + draw.measure(size, std::string_view(draft).substr(0, selectFrom), theme.valueFont);
        const float to = x + draw.measure(size, std::string_view(draft).substr(0, selectTo), theme.valueFont);
        draw.rect(from, r.y + 2 * s, to - from, r.h - 4 * s, 0, {theme.accent.r, theme.accent.g, theme.accent.b, 0.35f});
    }
    draw.text(x, r.y + r.h / 2 + 0.35f * size, size, draft, theme.text, Draw2D::Align::left, theme.valueFont);
    const float caretX = x + draw.measure(size, std::string_view(draft).substr(0, caret), theme.valueFont);
    draw.rect(caretX, r.y + 2 * s, 1 * s, r.h - 4 * s, 0, theme.accent);
    return true;
}

// The text inside a field: the draft and its caret while editing, else shown.
void Ui::fieldText(const std::string &shown, bool edit)
{
    const float lineHeight = theme.textSize * 1.25f;
    auto run = [&](size_t from, size_t to) {
        if (to > from) CLAY_TEXT(layout.text(draft.substr(from, to - from)), textConfig(theme.text, theme.valueFont));
    };
    auto caretBar = [&] {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(lineHeight)}}, .backgroundColor = clayColor(theme.accent)}) {}
    };
    if (!edit)
    {
        if (!shown.empty()) CLAY_TEXT(layout.text(shown), textConfig(theme.text, theme.valueFont));
        else CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(lineHeight)}}}) {}
        return;
    }
    if (selectFrom == selectTo)
    {
        run(0, caret), caretBar(), run(caret, draft.size());
        return;
    }
    run(0, selectFrom);
    if (caret == selectFrom) caretBar();
    const Color tint = {theme.accent.r, theme.accent.g, theme.accent.b, 0.35f};
    CLAY({.backgroundColor = clayColor(tint)}) { run(selectFrom, selectTo); }
    if (caret == selectTo) caretBar();
    run(selectTo, draft.size());
}

// A parameter's value as an item of a segmented control or dropdown.
static int itemOf(const Param &p, int count) { return std::clamp(int(std::lround(p.value - p.min)), 0, count - 1); }

bool Ui::toggle(Params &params, Param *p, const char *label)
{
    if (!p) return false;
    bool on = p->value >= (p->min + p->max) / 2;
    if (!toggle(label, on, p->id)) return false;
    set(params, *p, on ? p->max : p->min);
    return true;
}

bool Ui::segmented(Params &params, Param *p, const char *const *items, int count)
{
    if (!p || count < 1) return false;
    int selected = itemOf(*p, count);
    if (!segmented(items, count, selected, p->id)) return false;
    set(params, *p, p->min + selected);
    return true;
}

bool Ui::dropdown(Params &params, Param *p, const char *const *items, int count)
{
    if (!p || count < 1) return false;
    int selected = itemOf(*p, count);
    if (!dropdown(items, count, selected, p->id)) return false;
    set(params, *p, p->min + selected);
    return true;
}

bool Ui::textField(std::string &value, float widthPoints, int index)
{
    const Clay_ElementId fid = id("TextField", index);
    const Area a = element(fid);
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(fid.id)), Accessible::Role::textInput, nullptr);
    d.element = fid.id, d.node.value = value;
    bool committed = false;
    // Clicking from one field to another keeps the first one's edit, which
    // it picks up the next time it's declared.
    if (pendingField == fid.id)
    {
        committed = value != pendingDraft;
        value = std::move(pendingDraft);
        pendingField = 0;
    }
    if ((a.pressed || a.doubleClicked) && editing != fid.id) startEditing(fid.id, value);
    const Clay_BoundingBox box = Clay_GetElementData(fid).boundingBox; // last frame's
    committed |= editKeys(fid.id, value, Clay_PointerOver(fid), box.x + 5 - fieldScroll);
    const bool edit = editing == fid.id, hot = Clay_PointerOver(fid);
    // Text longer than the field scrolls to keep the caret in view, as far
    // as it must and no further; it shows its start again when not edited.
    float scroll = 0;
    if (edit)
    {
        const float visible = widthPoints - 10 - 1; // less the padding and the caret
        const float caretX = draw.measure(theme.textSize, draft.substr(0, caret), theme.valueFont);
        const float textWidth = draw.measure(theme.textSize, draft, theme.valueFont);
        fieldScroll = std::clamp(fieldScroll, caretX - visible, caretX);
        scroll = fieldScroll = std::clamp(fieldScroll, 0.0f, std::max(0.0f, textWidth - visible));
        caretAt = {(box.x + 5 - scroll + caretX) * s, box.y * s, box.height * s};
    }
    CLAY({.id = fid, .layout = {.sizing = {CLAY_SIZING_FIXED(widthPoints), CLAY_SIZING_FIT(0)}, .padding = {5, 5, 2, 2},
                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .cornerRadius = CLAY_CORNER_RADIUS(theme.radius), .clip = {.horizontal = true, .childOffset = {-scroll, 0}},
          .border = {.color = clayColor(edit ? theme.accent : hot ? theme.text : theme.edge), .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        fieldText(value, edit);
    }
    lastElement = fid.id;
    return committed;
}

// Drag up or down for the value (Shift for fine), as ImGui's DragFloat;
// double-click to type one.
void Ui::numberBox(Params &params, Param *p, const char *format, bool log, float widthPoints)
{
    if (!p) return;
    const Clay_ElementId nid = id("Number", p->id);
    const void *me = reinterpret_cast<const void *>(uintptr_t(nid.id));
    char shown[32];
    std::snprintf(shown, sizeof(shown), format, p->value);
    Described &self = describeParam(me, params, *p, shown, {});
    self.node.role = Accessible::Role::spinButton, self.element = nid.id;
    std::string typed = plainNumber(p->value);
    const std::string before = typed;
    if (pendingField == nid.id) typed = std::move(pendingDraft), pendingField = 0;
    order.push_back(me);
    const Area a = element(nid);
    const bool fine = mouse.modifiers & View::shift, edit = editing == nid.id;
    if (a.pressed || a.doubleClicked) focus = me, pressFocused = true;
    if (focus == me)
        focusUses = stepKeys | keyBit(View::Key::enter) | keyBit(View::Key::escape) | keyBit(View::Key::backspace)
                  | keyBit(View::Key::deleteForward),
        focusTakesDigits = true;
    auto endDrag = [&] {
        numberDrag = 0, held = {};
        params.end(*p);
        if (lockPointer) lockPointer(false);
    };
    // Double-click resets; a drag (right or up, past a few points' dead zone)
    // changes it, Shift for fine and Escape to cancel; clicks do nothing else.
    if (a.doubleClicked && !edit) set(params, *p, p->defaultValue);
    else if (a.pressed && !edit)
    {
        grabX = mouse.x, grabY = mouse.y, grabUnit = toUnit(*p, p->value, log), grabFine = fine;
        numberDrag = nid.id, numberMoved = false, numberStart = p->value;
        params.begin(*p);
        if (lockPointer) lockPointer(true);
        held = {&params, p, nullptr, true};
    }
    else if (a.held && numberDrag == nid.id)
    {
        if (fine != grabFine) grabX = mouse.x, grabY = mouse.y, grabUnit = toUnit(*p, p->value, log), grabFine = fine;
        const float moved = (mouse.x - grabX) + (grabY - mouse.y);
        numberMoved |= std::abs(moved) > 4 * s;
        if (numberMoved)
        {
            const double value = fromUnit(*p, grabUnit + moved / (180 * s) * (fine ? 0.1 : 1), log);
            if (value != p->value) params.change(*p, value);
        }
    }
    if (a.released && numberDrag == nid.id) endDrag();
    // Keys while it has focus: arrows step (Alt a large step), Page Up and
    // Down step large, Home and End go to the ends, Escape, Delete or
    // Backspace reset it, Enter or a typed number edits it.
    if (focus == me && !edit)
    {
        for (auto [key, modifiers] : keys)
        {
            const double u = toUnit(*p, p->value, log), small = modifiers & View::alt ? 0.1 : 0.01;
            switch (key)
            {
            case View::Key::up: case View::Key::right: set(params, *p, fromUnit(*p, u + small, log)); break;
            case View::Key::down: case View::Key::left: set(params, *p, fromUnit(*p, u - small, log)); break;
            case View::Key::pageUp: set(params, *p, fromUnit(*p, u + 0.1, log)); break;
            case View::Key::pageDown: set(params, *p, fromUnit(*p, u - 0.1, log)); break;
            case View::Key::home: set(params, *p, p->min); break;
            case View::Key::end: set(params, *p, p->max); break;
            case View::Key::enter: startEditing(nid.id, typed, true); break;
            case View::Key::escape:
                if (numberDrag == nid.id) params.change(*p, numberStart), endDrag(); // cancel the drag
                else set(params, *p, p->defaultValue);
                break;
            case View::Key::backspace: case View::Key::deleteForward: set(params, *p, p->defaultValue); break;
            default: break;
            }
        }
        if (!typedToFocus.empty() && editing != nid.id) startEditing(nid.id, std::exchange(typedToFocus, {}));
    }
    if (editKeys(nid.id, typed, Clay_PointerOver(nid)) || typed != before)
    {
        if (double value; parseNumber(typed, value)) set(params, *p, value);
    }
    // An edit ended with Enter or Escape leaves the keys with the box (a
    // click elsewhere takes them away as usual).
    if (edit && editing != nid.id && !mouse.pressed) focus = me;
    // The value as a bar filling the box, the number over it (compost's look).
    const bool editNow = editing == nid.id, hot = Clay_PointerOver(nid);
    const float lineHeight = theme.textSize * 1.25f;
    const Color fill = {theme.accent.r, theme.accent.g, theme.accent.b, 0.3f};
    CLAY({.id = nid, .layout = {.sizing = {CLAY_SIZING_FIXED(widthPoints), CLAY_SIZING_FIXED(lineHeight + 4)}},
          .backgroundColor = clayColor(theme.background), .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
          .clip = {.horizontal = true},
          .border = {.color = clayColor(editNow || a.held ? theme.accent : hot ? theme.text : theme.edge), .width = CLAY_BORDER_OUTSIDE(1)}})
    {
        if (!editNow)
            CLAY({.layout = {.sizing = {CLAY_SIZING_PERCENT(float(std::clamp(toUnit(*p, p->value, log), 0.0, 1.0))), CLAY_SIZING_GROW(0)}},
                  .backgroundColor = clayColor(fill)}) {}
        CLAY({.layout = {.padding = {5, 5, 0, 0}},
              .floating = {.attachPoints = {CLAY_ATTACH_POINT_CENTER_CENTER, CLAY_ATTACH_POINT_CENTER_CENTER},
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                           .attachTo = CLAY_ATTACH_TO_PARENT, .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT}})
        {
            fieldText(shown, editNow);
        }
    }
    lastElement = nid.id;
}

// A header that opens and closes the section under it (compost's drawer,
// ImGui's collapsing header). Declare the section only while it returns true.
bool Ui::drawer(const char *title, bool &open, int index)
{
    const Clay_ElementId did = id("Drawer", index);
    if (element(did, eachPress).clicked) open = !open;
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(did.id)), Accessible::Role::button, title);
    d.element = did.id, d.node.expanded = open, d.node.actions |= Accessible::click | Accessible::expand | Accessible::collapse;
    const bool hot = Clay_PointerOver(did);
    CLAY({.id = did, .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {6, 6, 4, 4}, .childGap = 6,
                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = clayColor(hot ? theme.panel : Color{0, 0, 0, 0}),
          .border = {.color = clayColor(theme.edge), .width = {.bottom = 1}}})
    {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(7), CLAY_SIZING_FIXED(7)}},
              .custom = {.customData = layout.custom(open ? chevronBox : chevronRightBox)}}) {}
        CLAY_TEXT(layout.text(title), textConfig(theme.text));
    }
    lastElement = did.id;
    return open;
}

void Ui::knob(Params &params, Param *p, const char *format, bool log, float diameter)
{
    if (!p) return;
    const Clay_ElementId kid = id("Knob", p->id);
    CLAY({.id = kid, .layout = {.sizing = {CLAY_SIZING_FIXED(std::max(diameter, 56.0f)), CLAY_SIZING_FIXED(diameter + 3.2f * theme.textSize)}},
          .custom = {.customData = layout.custom(knobBox, &placed.emplace_back(Placed{&params, p, format, log}))}}) {}
    lastElement = kid.id;
}

void Ui::slider(Params &params, Param *p, const char *format, bool log)
{
    if (!p) return;
    const Clay_ElementId sid = id("Slider", p->id);
    CLAY({.id = sid, .layout = {.sizing = {CLAY_SIZING_GROW(80), CLAY_SIZING_FIXED(theme.textSize * 2.6f + 6)}},
          .custom = {.customData = layout.custom(sliderBox, &placed.emplace_back(Placed{&params, p, format, log}))}}) {}
    lastElement = sid.id;
}

}

namespace gpu {

// Upright: the value above, the name below, filled from the bottom.
void Ui::vslider(Params &params, Param *p, Rect box, const char *format, bool log)
{
    if (!p) return;
    const float size = theme.textSize * s, cx = box.x + box.w / 2;
    const float top = box.y + 1.6f * size, bottom = box.y + box.h - 1.6f * size, thumb = 1.125f * size;
    const void *me = controlId(*p, box.x, box.y);
    const bool dragging = control(params, *p, me, box.x, top - thumb / 2, box.w, bottom - top + thumb, true, bottom - top, log);
    const bool hot = dragging || (!active && over(box.x, top, box.w, bottom - top));
    const float u = float(toUnit(*p, p->value, log)), y = bottom - (bottom - top) * u;
    if (focusShown(me)) draw.rect(box.x, box.y, box.w, box.h, theme.radius * s, {0, 0, 0, 0}, 1 * s, theme.accent);
    char value[32];
    std::snprintf(value, sizeof(value), format, p->value);
    describeParam(me, params, *p, value, box).node.orientation = 2;
    draw.text(cx, box.y + size, size, value, theme.dim, Draw2D::Align::center, theme.valueFont);
    draw.text(cx, box.y + box.h - 0.3f * size, size, p->name, theme.dim, Draw2D::Align::center, theme.labelFont);
    draw.rect(cx - 1.5f * s, top, 3 * s, bottom - top, 0, theme.track);
    draw.rect(cx - 1.5f * s, y, 3 * s, bottom - y, 0, theme.accent);
    const float thumbHeight = (hot ? 3.0f : 2.0f) * s;
    draw.rect(cx - thumb / 2, y - thumbHeight / 2, thumb, thumbHeight, 0, theme.text);
}

// Two parameters, x and y, set by dragging a point; double-click resets both.
void Ui::xyPad(Params &params, Param *px, Param *py, Rect r)
{
    if (!px || !py) return;
    // Its own drag id: knobs and sliders of px already use px itself.
    const auto a = area(&px->dragging, r.x, r.y, r.w, r.h);
    auto setFrom = [&](float x, float y) {
        params.change(*px, fromUnit(*px, (x - r.x) / r.w, false));
        params.change(*py, fromUnit(*py, 1 - (y - r.y) / r.h, false));
    };
    if (a.doubleClicked) set(params, *px, px->defaultValue), set(params, *py, py->defaultValue);
    if (a.pressed) params.begin(*px), params.begin(*py), held = {&params, px, py, false};
    if (a.held) setFrom(a.x, a.y);
    if (a.released) params.end(*px), params.end(*py), held = {};
    const float x = r.x + r.w * float(toUnit(*px, px->value, false)), y = r.y + r.h * (1 - float(toUnit(*py, py->value, false)));
    draw.rect(r.x, r.y, r.w, r.h, theme.radius * s, {0, 0, 0, 0}, 1 * s, theme.edge);
    draw.line(x, r.y, x, r.y + r.h, 1 * s, theme.edge);
    draw.line(r.x, y, r.x + r.w, y, 1 * s, theme.edge);
    draw.circle(x, y, (a.held ? 7.0f : 6.0f) * s, theme.accent, 1.5f * s, theme.text);
    const float size = theme.textSize * s;
    draw.text(r.x + 4 * s, r.y + r.h - 4 * s, size, px->name, theme.dim, Draw2D::Align::left, theme.labelFont);
    draw.text(r.x + 4 * s, r.y + size + 2 * s, size, py->name, theme.dim, Draw2D::Align::left, theme.labelFont);
}

// A level, 0 to 1, filled from the bottom, with a line at the peak (if 0 to 1).
void Ui::meter(float level, float peak, Rect r)
{
    level = std::clamp(level, 0.0f, 1.0f);
    draw.rect(r.x, r.y, r.w, r.h, 0, theme.track);
    draw.rect(r.x, r.y + r.h * (1 - level), r.w, r.h * level, 0, theme.accent);
    if (peak >= 0 && peak <= 1) draw.rect(r.x, r.y + r.h * (1 - peak) - 0.5f * s, r.w, 1 * s, 0, theme.text);
}

void Ui::vslider(Params &params, Param *p, const char *format, bool log, float heightPoints)
{
    if (!p) return;
    const Clay_ElementId vid = id("VSlider", p->id);
    CLAY({.id = vid, .layout = {.sizing = {CLAY_SIZING_FIXED(std::max(40.0f, 4 * theme.textSize)), CLAY_SIZING_FIXED(heightPoints)}},
          .custom = {.customData = layout.custom(vsliderBox, &placed.emplace_back(Placed{&params, p, format, log}))}}) {}
    lastElement = vid.id;
}

void Ui::xyPad(Params &params, Param *px, Param *py)
{
    if (!px || !py) return;
    const Clay_ElementId xid = id("XYPad", px->id * 1024 + py->id);
    CLAY({.id = xid, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
          .custom = {.customData = layout.custom(xyBox, &placed.emplace_back(Placed{&params, px, nullptr, false, py}))}}) {}
    lastElement = xid.id;
}

void Ui::meter(float level, float peak, float widthPoints)
{
    CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(widthPoints), CLAY_SIZING_GROW(0)}},
          .custom = {.customData = layout.custom(meterBox, &placed.emplace_back(Placed{nullptr, nullptr, nullptr, false, nullptr, level, peak}))}}) {}
}

void Ui::imageKnob(Params &params, Param *p, int image, int frames, Rect r, bool log, bool across)
{
    if (!p || frames < 1) return;
    const void *me = controlId(*p, r.x, r.y);
    const bool dragging = control(params, *p, me, r.x, r.y, r.w, r.h, true, 200 * s, log);
    describeParam(me, params, *p, plainNumber(p->value).c_str(), r);
    const int frame = int(std::lround(toUnit(*p, p->value, log) * (frames - 1)));
    const float step = 1.0f / float(frames), a = frame * step, b = a + step;
    draw.image(image, r.x, r.y, r.w, r.h, {}, {1, 1, 1, 1}, across ? Draw2D::Source{a, 0, b, 1} : Draw2D::Source{0, a, 1, b});
    if (focusShown(me) || dragging) draw.circle(r.x + r.w / 2, r.y + r.h / 2, std::min(r.w, r.h) / 2 + 2 * s, {0, 0, 0, 0}, 1 * s, theme.accent);
}

void Ui::imageKnob(Params &params, Param *p, int image, int frames, bool log, bool across)
{
    if (!p || frames < 1) return;
    int width, height;
    draw.imageSize(image, width, height);
    const float w = across ? float(width) / frames : float(width), h = across ? float(height) : float(height) / frames;
    const Clay_ElementId kid = id("ImageKnob", p->id);
    Placed placedKnob{&params, p, nullptr, log};
    placedKnob.image = image, placedKnob.frames = frames, placedKnob.across = across;
    CLAY({.id = kid, .layout = {.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(h)}},
          .custom = {.customData = layout.custom(imageKnobBox, &placed.emplace_back(placedKnob))}}) {}
    lastElement = kid.id;
}

// Which keys of an octave are black, from C.
static bool isBlack(int note)
{
    const int n = ((note % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

Ui::Notes Ui::piano(int lowest, int highest, Rect r, const std::function<bool(int)> &lit)
{
    Notes notes;
    int whites = 0;
    for (int n = lowest; n <= highest; ++n) whites += !isBlack(n);
    if (whites == 0) return notes;
    const float keyWidth = r.w / whites, blackWidth = keyWidth * 0.6f, blackHeight = r.h * 0.62f;
    // Each key's rectangle: whites side by side, blacks over the gap after the white before them.
    auto keyRect = [&](int note) {
        int whitesBefore = 0;
        for (int n = lowest; n < note; ++n) whitesBefore += !isBlack(n);
        if (!isBlack(note)) return Rect{r.x + whitesBefore * keyWidth, r.y, keyWidth, r.h};
        return Rect{r.x + whitesBefore * keyWidth - blackWidth / 2, r.y, blackWidth, blackHeight};
    };
    auto inside = [&](Rect k) { return mouse.x >= k.x && mouse.x < k.x + k.w && mouse.y >= k.y && mouse.y < k.y + k.h; };
    // The key under the pointer: a black one, being on top, first.
    int under = -1;
    for (int n = lowest; n <= highest && under < 0; ++n)
        if (isBlack(n) && inside(keyRect(n))) under = n;
    for (int n = lowest; n <= highest && under < 0; ++n)
        if (!isBlack(n) && inside(keyRect(n))) under = n;

    const auto a = area(&playing, r.x, r.y, r.w, r.h);
    if (a.held && under != playing && (under >= 0 || !a.pressed)) // pressed, or slid onto another key
    {
        if (playing >= 0) notes.off = playing;
        playing = under, notes.on = under;
    }
    if (a.released && playing >= 0) notes.off = playing, notes.on = -1, playing = -1;

    auto drawKey = [&](int n) {
        const Rect k = keyRect(n);
        const bool down = n == playing || (lit && lit(n));
        const Color fill = down ? theme.accent : isBlack(n) ? theme.blackKey : theme.whiteKey;
        draw.rect(k.x, k.y, k.w, k.h, Draw2D::Corners{0, 0, 2 * s, 2 * s}, fill, Draw2D::Sides{1 * s, 0, 0, 1 * s}, theme.edge);
    };
    for (int n = lowest; n <= highest; ++n)
        if (!isBlack(n)) drawKey(n);
    for (int n = lowest; n <= highest; ++n)
        if (isBlack(n)) drawKey(n);
    return notes;
}

Ui::Notes Ui::piano(int lowest, int highest, float heightPoints, const std::function<bool(int)> &lit)
{
    Placed board{nullptr, nullptr, nullptr, false};
    board.lowest = lowest, board.highest = highest, board.lit = lit;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(heightPoints)}},
          .custom = {.customData = layout.custom(pianoBox, &placed.emplace_back(std::move(board)))}}) {}
    const Notes notes = pianoNotes;
    pianoNotes = {};
    return notes;
}

bool Ui::envelope(std::vector<Point> &points, Rect r)
{
    if (points.empty()) return false;
    bool changed = false;
    const float handle = 5 * s;
    auto toPixels = [&](Point p) { return Point{r.x + p.x * r.w, r.y + (1 - p.y) * r.h}; };
    auto toUnit = [&](float x, float y) {
        return Point{std::clamp((x - r.x) / r.w, 0.0f, 1.0f), std::clamp(1 - (y - r.y) / r.h, 0.0f, 1.0f)};
    };
    int near = -1; // the point under the pointer
    for (size_t i = 0; i < points.size(); ++i)
    {
        const Point p = toPixels(points[i]);
        if (std::abs(mouse.x - p.x) <= handle * 1.6f && std::abs(mouse.y - p.y) <= handle * 1.6f) near = int(i);
    }
    const auto a = area(&grabbedPoint, r.x, r.y, r.w, r.h);
    if (a.doubleClicked)
    {
        if (near > 0 && near + 1 < int(points.size())) points.erase(points.begin() + near), changed = true;
        else if (near < 0)
        {
            const Point p = toUnit(mouse.x, mouse.y);
            auto at = std::upper_bound(points.begin(), points.end(), p.x, [](float x, const Point &q) { return x < q.x; });
            points.insert(at, p), changed = true;
        }
    }
    if (a.pressed) grabbedPoint = near;
    if (a.held && grabbedPoint >= 0 && grabbedPoint < int(points.size()))
    {
        Point p = toUnit(a.x, a.y);
        const size_t i = size_t(grabbedPoint);
        if (i == 0) p.x = points[i].x;
        else if (i + 1 == points.size()) p.x = points[i].x;
        else p.x = std::clamp(p.x, points[i - 1].x, points[i + 1].x);
        if (p.x != points[i].x || p.y != points[i].y) points[i] = p, changed = true;
    }
    if (a.released) grabbedPoint = -1;

    // Drawn: a fill under the line, the line, and a handle at each point.
    std::vector<float> xy;
    for (const Point &q : points)
    {
        const Point p = toPixels(q);
        xy.push_back(p.x), xy.push_back(p.y);
    }
    std::vector<float> area = xy;
    area.insert(area.end(), {xy[xy.size() - 2], r.y + r.h, xy[0], r.y + r.h});
    draw.rect(r.x, r.y, r.w, r.h, theme.radius * s, {0, 0, 0, 0}, 1 * s, theme.edge);
    draw.fill(area.data(), area.size() / 2, {theme.accent.r, theme.accent.g, theme.accent.b, 0.18f});
    draw.polyline(xy.data(), points.size(), 1.5f * s, theme.accent);
    for (size_t i = 0; i < points.size(); ++i)
    {
        const bool hot = int(i) == grabbedPoint || (grabbedPoint < 0 && int(i) == near);
        draw.circle(xy[2 * i], xy[2 * i + 1], hot ? handle * 1.25f : handle, hot ? theme.accent : theme.background, 1.5f * s, theme.accent);
    }
    return changed;
}

bool Ui::steps(std::vector<float> &values, Rect r)
{
    if (values.empty()) return false;
    bool changed = false;
    const float width = r.w / float(values.size());
    const auto a = area(&lastStep, r.x, r.y, r.w, r.h);
    if (a.held)
    {
        // Paint every bar between the last one and this, so a fast drag leaves no gaps.
        const int step = std::clamp(int((a.x - r.x) / width), 0, int(values.size()) - 1);
        const float value = std::clamp(1 - (a.y - r.y) / r.h, 0.0f, 1.0f);
        const int from = a.pressed || lastStep < 0 ? step : lastStep;
        for (int i = std::min(from, step); i <= std::max(from, step); ++i)
            if (values[size_t(i)] != value) values[size_t(i)] = value, changed = true;
        lastStep = step;
    }
    if (a.released) lastStep = -1;
    for (size_t i = 0; i < values.size(); ++i)
    {
        const float x = r.x + i * width, h = r.h * std::clamp(values[i], 0.0f, 1.0f);
        draw.rect(x + 1 * s, r.y, width - 2 * s, r.h, 0, theme.track);
        draw.rect(x + 1 * s, r.y + r.h - h, width - 2 * s, h, 0, theme.accent);
    }
    return changed;
}

bool Ui::envelope(std::vector<Point> &points)
{
    Placed editor{nullptr, nullptr, nullptr, false};
    editor.points = &points;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
          .custom = {.customData = layout.custom(envelopeBox, &placed.emplace_back(std::move(editor)))}}) {}
    return std::find(changedLast.begin(), changedLast.end(), &points) != changedLast.end();
}

bool Ui::steps(std::vector<float> &values)
{
    Placed editor{nullptr, nullptr, nullptr, false};
    editor.values = &values;
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
          .custom = {.customData = layout.custom(stepsBox, &placed.emplace_back(std::move(editor)))}}) {}
    return std::find(changedLast.begin(), changedLast.end(), &values) != changedLast.end();
}

}
