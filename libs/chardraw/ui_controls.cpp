// Ui controls: buttons, toggles, dropdowns, context menus, tabs, knobs, sliders, pads, meters, piano, envelope, steps, tooltip.
//
// Modelled on, where named below:
// compost https://github.com/charCulbert/compost
// Slide https://github.com/charCulbert/slide
#include "ui_internal.h"

namespace gpu
{

// Typing a letter in a list: the next item after `from` starting with it
// (wrapping; separators, a nullptr label, skipped), or -1. label(i) is item i's.
template <typename Label>
static int startingWith (Label label, int count, int from, char c)
{
    for (int k = 1; k <= count; ++k)
    {
        const int i = ((from + k) % count + count) % count;
        const char* l = label (i);
        if (l && std::tolower (static_cast<unsigned char> (l[0])) == std::tolower (static_cast<unsigned char> (c)))
            return i;
    }
    return -1;
}

double Ui::toUnit (const Param& p, double v, bool log)
{
    if (p.max <= p.min)
        return 0;
    if (log && p.min > 0 && !p.stepped)
        return std::log (v / p.min) / std::log (p.max / p.min);
    return (v - p.min) / (p.max - p.min);
}

double Ui::fromUnit (const Param& p, double u, bool log)
{
    u = std::clamp (u, 0.0, 1.0);
    // Stepped: the travel shared evenly between the steps, each its own
    // stretch (a 0..4 parameter's 4 from four fifths of the way on, not only
    // at the end). toUnit's whole values fall inside their own stretches.
    if (p.stepped)
        return p.min + std::min (std::floor (u * (p.max - p.min + 1)), p.max - p.min);
    if (log && p.min > 0)
        return p.min * std::pow (p.max / p.min, u);
    return p.min + u * (p.max - p.min);
}

double Ui::grabFrom (const Param& p, bool log)
{
    // Stepped: the middle of the value's stretch, so a drag either way goes
    // as far to change it (toUnit's whole values sit near one end).
    if (p.stepped && p.max > p.min)
        return (p.value - p.min + 0.5) / (p.max - p.min + 1);
    return toUnit (p, p.value, log);
}

double Ui::moved (const Param& p, double by, bool log)
{
    if (!p.stepped)
        return fromUnit (p, toUnit (p, p.value, log) + by, log);
    // An arrow's or a notch's move (0.01 or less) is one step, however many there are.
    const double steps = std::abs (by) <= 0.01 ? 1 : std::max (1.0, std::round (std::abs (by) * (p.max - p.min)));
    return std::clamp (p.value + (by < 0 ? -steps : steps), p.min, p.max);
}

void Ui::set (Params& params, Param& p, double value)
{
    params.begin (p);
    params.change (p, value);
    params.end (p);
}

// A control's identity: its parameter and where it is, so a knob and a slider
// of one parameter drag, focus and step apart.
const void* Ui::controlId (const Param& p, float x, float y)
{
    uint32_t bits[2];
    std::memcpy (bits, &x, 4), std::memcpy (bits + 1, &y, 4);
    const uintptr_t where = (uintptr_t (bits[0]) * 0x9E3779B1u) ^ bits[1];
    return reinterpret_cast<const void*> (reinterpret_cast<uintptr_t> (&p) ^ (where << 3));
}

bool Ui::control (Params& params, Param& p, const void* me, float x, float y, float w, float h, bool vertical,
    float travel, bool log, bool typable, bool absolute, float ends)
{
    order.push_back (me);
    const bool fine = mouse.modifiers & View::shift;
    const double unit = toUnit (p, p.value, log);

    // Knobs and sliders step, and Delete or Backspace reset them (as
    // double-click does); Escape is left to close what they're in (a menu,
    // a drawer). One with a value to type also takes Enter and digits (typeInto).
    if (focus == me)
        focusUses = stepKeys | keyBit (View::Key::backspace) | keyBit (View::Key::deleteForward)
                    | (typable ? keyBit (View::Key::enter) : 0),
        focusTakesDigits = typable;
    if (!active && over (x, y, w, h))
    {
        if (mouse.pressed)
            focus = me, pressFocused = true;
        if (mouse.pressed)
        {
            active = me;
            grabSecond = mouse.doubleClicked, grabMoved = false, pressX = mouse.x, pressY = mouse.y;
            grabX = mouse.x, grabY = mouse.y, grabUnit = grabFrom (p, log),
            grabFine = fine || grabSecond; // measured from the press
            grabRelative = !absolute || fine || grabSecond;
            params.begin (p);
            // Relative: the pointer stays put while the value moves; absolute:
            // the value jumps to the pointer, which it then follows.
            if (grabRelative && lockPointer)
                lockPointer (true);
            held = { &params, &p, nullptr, grabRelative };
        }
        else if (mouse.wheel != 0 && (!wheelNeedsFocus || focus == me))
        {
            if (p.stepped) // a step a notch; a trackpad's small turns add up to one
            {
                itemWheel += mouse.wheel;
                if (std::abs (itemWheel) >= 1)
                    set (params, p, moved (p, itemWheel > 0 ? 0.01 : -0.01, log)), itemWheel = 0;
            }
            else
                set (params, p, fromUnit (p, unit + mouse.wheel * (fine ? 0.002 : 0.02), log));
            wheelUsed = true;
        }
    }

    if (focus == me)
        for (auto [key, modifiers] : keys)
        {
            // Shift for fine, Alt for a page's step (compost's large step).
            const double step = modifiers & View::shift ? 0.001 : modifiers & View::alt ? 0.1 : 0.01;
            switch (key)
            {
                case View::Key::up:
                case View::Key::right:
                    set (params, p, moved (p, step, log));
                    break;
                case View::Key::down:
                case View::Key::left:
                    set (params, p, moved (p, -step, log));
                    break;
                case View::Key::pageUp:
                    set (params, p, moved (p, 0.1, log));
                    break;
                case View::Key::pageDown:
                    set (params, p, moved (p, -0.1, log));
                    break;
                case View::Key::home:
                    set (params, p, p.min);
                    break;
                case View::Key::end:
                    set (params, p, p.max);
                    break;
                case View::Key::backspace:
                case View::Key::deleteForward:
                    set (params, p, p.defaultValue);
                    break;
                default:
                    break; // Tab: in end()
            }
        }

    if (active != me)
        return false;
    activeSeen = true;
    if (!grabMoved && std::hypot (mouse.x - pressX, mouse.y - pressY) > 4 * s)
        grabMoved = true;
    // A second press drags fine once it moves; until then it changes nothing.
    const bool slow = fine || grabSecond;
    if (slow && !grabRelative)
        grabRelative = true, grabX = mouse.x, grabY = mouse.y, grabUnit = grabFrom (p, log), grabFine = slow;
    if (!grabRelative) // absolute: the value under the pointer
    {
        const double u = std::clamp (
            vertical ? (y + h - ends - mouse.y) / (h - 2 * ends) : (mouse.x - x - ends) / (w - 2 * ends), 0.0f, 1.0f);
        const double value = fromUnit (p, u, log);
        if (value != p.value)
            params.change (p, value);
    }
    else if (!grabSecond || grabMoved)
    {
        // Switching to or from fine mid-drag continues from where the value is.
        if (slow != grabFine)
            grabX = mouse.x, grabY = mouse.y, grabUnit = grabFrom (p, log), grabFine = slow;
        const double moved = (vertical ? grabY - mouse.y : mouse.x - grabX) / (travel * (slow ? 10 : 1));
        // Past an end the value stops, and so does the count: turning back
        // moves it at once (as compost's), not after undoing the overshoot.
        const double u = grabUnit + moved;
        if (u > 1)
            grabUnit -= u - 1;
        if (u < 0)
            grabUnit -= u;
        const double value = fromUnit (p, std::clamp (u, 0.0, 1.0), log);
        if (value != p.value)
            params.change (p, value);
    }
    if (!mouse.down)
    {
        if (grabSecond && !grabMoved)
            params.change (p, p.defaultValue); // a double-click: reset, in this gesture
        params.end (p);
        active = nullptr;
        if (held.locked && lockPointer)
            lockPointer (false);
        held = {};
    }
    return true;
}

std::string Ui::Format::operator() (double value) const
{
    if (text)
        return text (value);
    char shown[64];
    std::snprintf (shown, sizeof (shown), format ? format : "%.2f", value);
    return shown;
}

float Ui::fillStart (const Param& p, const Format& format, bool log)
{
    return format.fill == Fill::fromDefault ? float (toUnit (p, p.defaultValue, log)) : 0;
}

void Ui::knob (Params& params, Param* p, Rect box, const Format& format, bool log)
{
    if (!p)
        return;
    // The dial fills the width, leaving room below for two lines of text.
    const float size = theme.textSize * s;
    const float r = std::min (box.w, box.h - 3 * size) / 2, cx = box.x + box.w / 2, cy = box.y + r;
    const void* me = controlId (*p, cx, cy);
    const bool dragging = control (params, *p, me, cx - r, cy - r, 2 * r, 2 * r, true, 200 * s, log, true);
    // 270 degrees of travel, from 7:30 to 4:30; the ring a tenth of the dial (as compost's knob).
    const float from = 3.927f, sweep = 4.712f, u = float (toUnit (*p, p->value, log));
    const float width = std::max (2.0f * s, r * 0.19f);
    if (focusShown (me))
        draw.circle (cx, cy, r + 2 * s, { 0, 0, 0, 0 }, 1 * s, theme.accent);
    draw.arc (cx, cy, r - width, width, from, from + sweep, theme.track);
    const float start = fillStart (*p, format, log), lo = std::min (start, u), hi = std::max (start, u);
    if (hi > lo)
        draw.arc (cx, cy, r - width, width, from + sweep * lo, from + sweep * hi, theme.accent);
    // A stepped parameter's detents: a notch cut across the ring at each step between the ends.
    if (const double steps = p->max - p->min; p->stepped && detents && steps >= 1 && steps <= 32)
        for (int k = 1; k < int (steps); ++k)
        {
            const float a = from + sweep * float (k / steps);
            draw.line (cx + std::sin (a) * (r - 2.2f * width), cy - std::cos (a) * (r - 2.2f * width),
                cx + std::sin (a) * r, cy - std::cos (a) * r, 1.5f * s, theme.background);
        }
    const float angle = from + sweep * u, inner = r - 1.6f * width;
    draw.circle (cx, cy, inner, theme.background, 1 * s, theme.edge);
    draw.line (cx + std::sin (angle) * inner * 0.3f, cy - std::cos (angle) * inner * 0.3f,
        cx + std::sin (angle) * inner * 0.9f, cy - std::cos (angle) * inner * 0.9f, (dragging ? 2.0f : 1.4f) * s,
        dragging ? theme.accent : theme.text);
    const std::string value = format (p->value);
    describeParam (me, params, *p, value.c_str(), box).node.orientation = 2; // dragged up and down
    typeInto (me, &p->defaultValue, *p, format);
    if (!valueEntry (params, *p, format, me, &p->defaultValue, { box.x, cy + r + 0.3f * size, box.w, 1.3f * size },
            Draw2D::Align::center))
        draw.text (cx, cy + r + 1.3f * size, size, value, theme.text, Draw2D::Align::center, theme.valueFont);
    draw.text (cx, cy + r + 2.6f * size, size, format.name (*p), theme.dim, Draw2D::Align::center, theme.labelFont);
}

// As compost's slider: the name on the left and the value on the right above
// a 3-point track, filled with the accent up to the value, and a 2-point
// thumb line across it.
void Ui::slider (Params& params, Param* p, Rect box, const Format& format, bool log)
{
    if (!p)
        return;
    const float px = box.x, py = box.y, w = box.w, h = box.h, size = theme.textSize * s;
    const std::string value = format (p->value);
    // The value first: a click on it types, rather than starting a drag.
    const float valueWidth = std::max (draw.measure (size, value, theme.valueFont), 3 * size) + 4 * s;
    const void* me = controlId (*p, px, py);
    typeInto (me, &p->max, *p, format);
    const bool typing = valueEntry (
        params, *p, format, me, &p->max, { px + w - valueWidth, py, valueWidth, 1.4f * size }, Draw2D::Align::right);
    describeParam (me, params, *p, value.c_str(), box).node.orientation = 1;
    const bool dragging = control (params, *p, me, px, py, w, h, false, w, log, true, sliderDrag == Drag::absolute);
    const bool hot = dragging || (!active && over (px, py, w, h));
    const float u = float (toUnit (*p, p->value, log)), thumb = 1.125f * size, track = py + h - thumb / 2;
    if (focusShown (me))
        draw.rect (px - 3 * s, py - 2 * s, w + 6 * s, h + 4 * s, theme.radius * s, { 0, 0, 0, 0 }, 1 * s, theme.accent);
    draw.text (px, py + size, size, format.name (*p), theme.dim, Draw2D::Align::left, theme.labelFont);
    if (!typing)
        draw.text (px + w, py + size, size, value, theme.dim, Draw2D::Align::right, theme.valueFont);
    draw.rect (px, track - 1.5f * s, w, 3 * s, 0, theme.track);
    const float start = fillStart (*p, format, log);
    draw.rect (px + w * std::min (start, u), track - 1.5f * s, w * std::abs (u - start), 3 * s, 0, theme.accent);
    const float x = px + w * u, thumbWidth = (hot ? 3.0f : 2.0f) * s;
    draw.rect (x - thumbWidth / 2, track - thumb / 2, thumbWidth, thumb, 0, theme.text);
}

bool Ui::button (const char* label, int index)
{
    const Clay_ElementId bid = id (label, index);
    Described& d = describe (reinterpret_cast<const void*> (uintptr_t (bid.id)), Accessible::Role::button, label);
    d.element = bid.id, d.node.actions |= Accessible::click;
    const Area a = element (bid, eachPress);
    bool clicked = a.clicked;
    for (View::Key key : focusable (bid, a))
        clicked |= key == View::Key::enter;
    const bool hot = Clay_PointerOver (bid);
    CLAY ({ .id = bid,
        .layout = { .padding = { 10, 10, 4, 4 }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = clayColor (a.held ? theme.text
                                      : hot  ? theme.panel
                                             : Color { 0, 0, 0, 0 }),
        .cornerRadius = CLAY_CORNER_RADIUS (theme.radius),
        .border = { .color = clayColor (focusShown (bid) ? theme.accent
                                        : hot            ? theme.text
                                                         : theme.edge),
            .width = CLAY_BORDER_OUTSIDE (1) } })
    {
        CLAY_TEXT (layout.text (label), textConfig (a.held ? theme.background : theme.text));
    }
    lastElement = bid.id;
    return clicked;
}

// Off: an outline. On: filled with the text colour, its label in the
// background's (Slide's Sync button).
bool Ui::toggle (const char* label, bool& on, int index)
{
    const Clay_ElementId tid = id (label, index);
    const Area a = element (tid, eachPress);
    bool clicked = a.clicked;
    for (View::Key key : focusable (tid, a))
        clicked |= key == View::Key::enter;
    if (clicked)
        on = !on;
    Described& d = describe (reinterpret_cast<const void*> (uintptr_t (tid.id)), Accessible::Role::checkBox, label);
    d.element = tid.id, d.node.checked = on, d.node.actions |= Accessible::click;
    const bool hot = Clay_PointerOver (tid);
    CLAY ({ .id = tid,
        .layout = { .padding = { 7, 7, 3, 3 }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = clayColor (on    ? theme.text
                                      : hot ? theme.panel
                                            : Color { 0, 0, 0, 0 }),
        .cornerRadius = CLAY_CORNER_RADIUS (theme.radius),
        .border = { .color = clayColor (focusShown (tid) ? theme.accent
                                        : on || a.held   ? theme.text
                                                         : theme.edge),
            .width = CLAY_BORDER_OUTSIDE (1) } })
    {
        CLAY_TEXT (layout.text (label), textConfig (on ? theme.background : theme.text));
    }
    lastElement = tid.id;
    return clicked;
}

bool Ui::segmented (const char* const* items, int count, int& selected, int index)
{
    bool changed = false;
    const Clay_ElementId sid = id ("Segmented", index);
    CLAY ({ .id = sid,
        .border = { .color = clayColor (focusShown (sid) ? theme.accent : theme.edge), .width = CLAY_BORDER_ALL (1) } })
    {
        // Clicks and keys first, so every cell draws the new choice.
        Area any;
        for (int i = 0; i < count; ++i)
        {
            const Area a = element (id ("Segment", index * 64 + i), eachPress);
            if (a.clicked && selected != i)
                selected = i, changed = true;
            any.pressed |= a.pressed;
        }
        // The arrows move the choice, wrapping at the ends, as APG's radio group.
        for (View::Key key : focusable (sid, any))
        {
            const int step = key == View::Key::right || key == View::Key::down ? 1
                             : key == View::Key::left || key == View::Key::up  ? -1
                                                                               : 0;
            const int next = count > 0 ? ((selected + step) % count + count) % count : selected;
            if (step && next != selected)
                selected = next, changed = true;
        }
        const void* group = reinterpret_cast<const void*> (uintptr_t (sid.id));
        describe (group, Accessible::Role::radioGroup, nullptr).element = sid.id;
        for (int i = 0; i < count; ++i)
        {
            const Clay_ElementId cell = id ("Segment", index * 64 + i);
            Described& d = describe (reinterpret_cast<const void*> (uintptr_t (cell.id)), Accessible::Role::radioButton,
                items[i], accessId (group));
            d.element = cell.id, d.node.checked = selected == i, d.node.actions |= Accessible::click;
        }
        for (int i = 0; i < count; ++i)
        {
            const Clay_ElementId cell = id ("Segment", index * 64 + i);
            const bool on = selected == i, hot = Clay_PointerOver (cell);
            CLAY ({ .id = cell,
                .layout = { .padding = { 8, 8, 3, 3 } },
                .backgroundColor = clayColor (on    ? theme.text
                                              : hot ? theme.panel
                                                    : Color { 0, 0, 0, 0 }) })
            {
                CLAY_TEXT (layout.text (items[i]), textConfig (on ? theme.background : hot ? theme.text : theme.dim));
            }
        }
    }
    lastElement = sid.id;
    return changed;
}

Ui::Tabs Ui::tabs (const char* const* items, int count, int& selected, int index)
{
    Tabs result;
    const Clay_ElementId lid = id ("TabList", index);
    const void* me = reinterpret_cast<const void*> (uintptr_t (lid.id));
    // Presses and keys first, so every tab draws the new choice.
    Area any;
    for (int i = 0; i < count; ++i)
    {
        const Area a = element (id ("Tab", index * 64 + i), eachPress);
        if (a.clicked && selected != i)
            selected = i, result.changed = true; // let go on it, as segmented's (WCAG 2.5.2)
        if (a.held)
            result.held = i, result.x = a.x, result.y = a.y, result.released = a.released;
        // Under the pointer now (Clay's hover trails it by a frame): the last frame's box.
        if (const Clay_ElementData d = Clay_GetElementData (id ("Tab", index * 64 + i)); d.found)
        {
            const Clay_BoundingBox& b = d.boundingBox;
            if (mouse.x >= b.x * s && mouse.x < (b.x + b.width) * s && mouse.y >= b.y * s
                && mouse.y < (b.y + b.height) * s)
                result.over = i;
        }
        any.pressed |= a.pressed;
    }
    // The bar's empty part, after the tabs: a press on a tab is the tab's.
    if (const Area bar = element (lid); bar.held && result.held < 0)
        result.barHeld = true, result.x = bar.x, result.y = bar.y, result.released = bar.released;
    order.push_back (me);
    if (any.pressed)
        focus = me, pressFocused = true;
    if (focus == me)
    {
        focusUses = keyBit (View::Key::left) | keyBit (View::Key::right) | keyBit (View::Key::home)
                    | keyBit (View::Key::end) | keyBit (View::Key::up) | keyBit (View::Key::down)
                    | keyBit (View::Key::enter);
        focusTakesDigits = false;
        for (auto [key, modifiers] : keys)
        {
            if (modifiers & View::alt)
            {
                result.altKey = true, result.key = key;
                continue;
            }
            // The arrows wrap, as APG's tabs do.
            int next = selected;
            if (count < 1)
                break; // no tabs: nothing to choose
            if (key == View::Key::right)
                next = ((selected + 1) % count + count) % count;
            else if (key == View::Key::left)
                next = ((selected - 1) % count + count) % count;
            else if (key == View::Key::home)
                next = 0;
            else if (key == View::Key::end)
                next = count - 1;
            if (next != selected)
                selected = next, result.changed = true;
        }
    }
    if (const Clay_ElementData bar = Clay_GetElementData (lid); bar.found)
        result.bar = { bar.boundingBox.x * s, bar.boundingBox.y * s, bar.boundingBox.width * s,
            bar.boundingBox.height * s };
    describe (me, Accessible::Role::tabList, nullptr).element = lid.id;
    for (int i = 0; i < count; ++i)
    {
        const Clay_ElementId tab = id ("Tab", index * 64 + i);
        Described& d = describe (
            reinterpret_cast<const void*> (uintptr_t (tab.id)), Accessible::Role::tab, items[i], accessId (me));
        d.element = tab.id, d.node.selected = selected == i, d.node.actions |= Accessible::click;
    }
    CLAY ({ .id = lid,
        .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIT (0) } },
        .border = { .color = clayColor (focusShown (me) ? theme.accent : theme.edge), .width = { .bottom = 1 } } })
    {
        for (int i = 0; i < count; ++i)
        {
            const Clay_ElementId tab = id ("Tab", index * 64 + i);
            const bool on = selected == i, hot = Clay_PointerOver (tab);
            CLAY ({ .id = tab,
                .layout = { .padding = { 10, 10, 4, 4 } },
                .backgroundColor = clayColor (on ? theme.panel : Color { 0, 0, 0, 0 }),
                .cornerRadius = { theme.radius, theme.radius, 0, 0 },
                .border = { .color = clayColor (on ? theme.edge : Color { 0, 0, 0, 0 }), .width = { 1, 1, 1, 0 } } })
            {
                CLAY_TEXT (layout.text (items[i]), textConfig (on || hot ? theme.text : theme.dim));
            }
        }
    }
    lastElement = lid.id;
    return result;
}

bool Ui::dropdown (const char* const* items, int count, int& selected, int index)
{
    bool changed = false;
    const Clay_ElementId did = id ("Dropdown", index), menu = id ("DropdownMenu", index);
    const bool open = openMenu == menu.id;
    const Area a = element (did, eachPress);
    auto openAt = [&] (int option) {
        if (count > 0)
            openMenu = menu.id, menuFor = 0, highlight = std::clamp (option, 0, count - 1); // (none: nothing to open)
    };
    auto choose = [&] {
        if (highlight >= 0 && highlight < count && highlight != selected)
            selected = highlight, changed = true;
        openMenu = 0;
    };
    if (a.pressed)
    {
        if (open)
            openMenu = 0;
        else
            openAt (selected);
    }
    // Keys, as HTML's select (the WAI-ARIA select-only combo box): closed,
    // Enter, Space and the arrows open it on the choice, Home and End on the
    // first and last; open, the arrows, Home and End move the highlight,
    // Enter, Space or Tab choose it, Escape closes it as it was. Typing a
    // letter jumps to the next option starting with it.
    focusable (did, a);
    if (focus == reinterpret_cast<const void*> (uintptr_t (did.id)))
    {
        focusUses |= keyBit (View::Key::escape) | keyBit (View::Key::home) | keyBit (View::Key::end)
                     | keyBit (View::Key::pageUp) | keyBit (View::Key::pageDown);
        focusTakesLetters = true;
        for (auto [key, modifiers] : keys)
        {
            const bool isOpen = openMenu == menu.id;
            switch (key)
            {
                case View::Key::enter:
                    isOpen ? choose() : openAt (selected);
                    break;
                case View::Key::escape:
                    openMenu = 0;
                    break;
                case View::Key::tab:
                    if (isOpen)
                        choose();
                    break;
                case View::Key::down:
                    isOpen ? void (highlight = std::min (highlight + 1, count - 1)) : openAt (selected);
                    break;
                case View::Key::up:
                    if (isOpen && modifiers & View::alt)
                        choose();
                    else
                        isOpen ? void (highlight = std::max (highlight - 1, 0)) : openAt (selected);
                    break;
                case View::Key::home:
                case View::Key::pageUp:
                    openAt (0);
                    break;
                case View::Key::end:
                case View::Key::pageDown:
                    openAt (count - 1);
                    break;
                default:
                    break;
            }
        }
        for (char c : std::exchange (typedToFocus, {}))
        {
            if (c == ' ')
            {
                openMenu == menu.id ? choose() : openAt (selected);
                continue;
            }
            if (const int i = startingWith (
                    [&] (int i) { return items[i]; }, count, openMenu == menu.id ? highlight : selected, c);
                i >= 0)
                openAt (i);
        }
    }
    const size_t combo = described.size(); // an index: its options are described after it
    Described& d = describe (reinterpret_cast<const void*> (uintptr_t (did.id)), Accessible::Role::comboBox, nullptr);
    d.element = did.id, d.node.value = selected >= 0 && selected < count ? items[selected] : "",
    d.node.expanded = openMenu == menu.id;
    d.node.actions |= Accessible::click | Accessible::expand | Accessible::collapse;
    const bool hot = Clay_PointerOver (did);
    // The menu opens below, or above where there's no room below (sizes from
    // the last frame, in points).
    const Clay_BoundingBox box = Clay_GetElementData (did).boundingBox;
    const float menuHeight = float (count) * (theme.textSize * 1.25f + 6) + 4;
    const bool up = box.y + box.height + menuHeight > viewHeight && box.y - menuHeight >= 0;
    CLAY ({ .id = did,
        .layout = { .padding = { 7, 7, 3, 3 }, .childGap = 8, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
        .backgroundColor = clayColor (hot || open ? theme.panel : Color { 0, 0, 0, 0 }),
        .cornerRadius = CLAY_CORNER_RADIUS (theme.radius),
        .border = { .color = clayColor (focusShown (did) ? theme.accent
                                        : open || hot    ? theme.text
                                                         : theme.edge),
            .width = CLAY_BORDER_OUTSIDE (1) } })
    {
        CLAY_TEXT (layout.text (selected >= 0 && selected < count ? items[selected] : ""), textConfig (theme.text));
        CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIXED (7), CLAY_SIZING_FIXED (7) } },
            .custom = { .customData = layout.custom (chevronBox) } })
        {}
        if (openMenu == menu.id)
        {
            menuSeen = true;
            // Its list, for screen readers: the options, the chosen one selected.
            const void* list = reinterpret_cast<const void*> (uintptr_t (menu.id));
            Described& listBox = describe (list, Accessible::Role::listBox, nullptr,
                accessId (reinterpret_cast<const void*> (uintptr_t (did.id))));
            listBox.element = menu.id, listBox.node.actions = 0; // the dropdown names it and has the focus
            for (int i = 0; i < count; ++i)
            {
                const Clay_ElementId item = id ("DropdownItem", index * 1024 + i);
                Described& option = describe (reinterpret_cast<const void*> (uintptr_t (item.id)),
                    Accessible::Role::option, items[i], accessId (list));
                option.element = item.id, option.node.selected = i == highlight,
                option.node.actions |= Accessible::click;
                if (i == highlight)
                    described[combo].node.activeDescendant = option.node.id;
            }
            const Clay_FloatingElementConfig below = { .offset = { 0, up ? -2.0f : 2.0f },
                .zIndex = 100,
                .attachPoints = { up ? CLAY_ATTACH_POINT_LEFT_BOTTOM : CLAY_ATTACH_POINT_LEFT_TOP,
                    up ? CLAY_ATTACH_POINT_LEFT_TOP : CLAY_ATTACH_POINT_LEFT_BOTTOM },
                .attachTo = CLAY_ATTACH_TO_PARENT };
            if (const int i = popup (menu, "DropdownItem", index * 1024, items, count, selected, below, box.width);
                i >= 0)
                selected = i, changed = true, openMenu = 0;
        }
    }
    lastElement = did.id;
    return changed;
}

// The list a dropdown or context menu shows, over everything: an item per
// row, the highlighted one shaded and the marked one (a dropdown's choice) in
// the accent; the pointer moves the highlight. A nullptr item is a separator.
int Ui::popup (Clay_ElementId menu, const char* itemName, int itemBase, const char* const* items, int count, int marked,
    Clay_FloatingElementConfig floating, float minWidth)
{
    int clicked = -1;
    CLAY ({ .id = menu,
        .layout = { .sizing = { .width = CLAY_SIZING_FIT (minWidth) },
            .padding = CLAY_PADDING_ALL (1),
            .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = clayColor (
            { theme.background.r, theme.background.g, theme.background.b, theme.background.a * menuOpacity }),
        .cornerRadius = CLAY_CORNER_RADIUS (theme.radius),
        .floating = floating,
        .border = { .color = clayColor (theme.edge), .width = CLAY_BORDER_OUTSIDE (1) },
        .userData = layout.shadow (theme.shadow, 10, 0, 3) })
    {
        for (int i = 0; i < count; ++i)
        {
            if (!items[i])
            {
                CLAY ({ .layout = { .sizing = { .width = CLAY_SIZING_GROW (0) }, .padding = { 0, 0, 3, 3 } } })
                {
                    CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIXED (1) } },
                        .backgroundColor = clayColor (theme.edge) })
                    {}
                }
                continue;
            }
            const Clay_ElementId item = id (itemName, itemBase + i);
            if (element (item, eachPress).clicked)
                clicked = i;
            const bool itemHot = Clay_PointerOver (item);
            if (itemHot && (mouse.moved || mouse.pressed))
                highlight = i; // the pointer moves the highlight too
            CLAY ({ .id = item,
                .layout = { .sizing = { .width = CLAY_SIZING_GROW (0) }, .padding = { 7, 7, 3, 3 } },
                .backgroundColor = clayColor (i == highlight ? theme.panel : Color { 0, 0, 0, 0 }) })
            {
                CLAY_TEXT (layout.text (items[i]), textConfig (i == marked ? theme.accent : theme.text));
            }
        }
    }
    return clicked;
}

int Ui::contextMenu (const char* const* items, int count)
{
    return lastElement ? contextMenu (Clay_ElementId { lastElement, 0, 0, {} }, items, count) : -1;
}

int Ui::contextMenu (const MenuItem* items, int count)
{
    return lastElement ? contextMenu (Clay_ElementId { lastElement, 0, 0, {} }, items, count) : -1;
}

void Ui::closeMenu()
{
    if (menuFor && focus == reinterpret_cast<const void*> (uintptr_t (openMenu)))
        focus = menuReturn;
    openMenu = menuFor = 0;
}

// A list of labels is a menu without submenus or checks, each item's id its index.
int Ui::contextMenu (Clay_ElementId element, const char* const* items, int count)
{
    std::vector<MenuItem> list (size_t (std::max (count, 0)));
    for (int i = 0; i < count; ++i)
        list[size_t (i)] = { items[i], i };
    return contextMenu (element, list.data(), count);
}

int Ui::contextMenu (Clay_ElementId element, const MenuItem* items, int count)
{
    if (count < 1)
        return -1;
    said.push_back ({ element.id, {}, {}, false, false, false, true }); // a screen reader can open it (showMenu)
    const Clay_ElementId menu = id ("ContextMenu", int (element.id));
    const void* me = reinterpret_cast<const void*> (uintptr_t (menu.id));
    auto asId = [] (Clay_ElementId e) { return reinterpret_cast<const void*> (uintptr_t (e.id)); };
    // The next item after `from` in a list that isn't a separator, wrapping; by is 1 or -1.
    auto next = [] (const MenuItem* list, int n, int from, int by) {
        for (int k = 1; k <= n; ++k)
            if (const int i = ((from + by * k) % n + n) % n; list[i].label)
                return i;
        return -1;
    };

    // It opens on a secondary press on the element, at the pointer, or on the
    // Menu key (a screen reader's showMenu sends one) while a control in it
    // has focus, below it with the first item highlighted.
    const bool focused = controlIn (focus, element);
    focusHasMenu |= focused;
    bool byKey = false;
    if (focused)
        for (auto [key, modifiers] : keys)
            byKey |= key == View::Key::contextMenu;
    const bool byPointer = !byKey && mouse.secondary && !overPopup && Clay_PointerOver (element);
    if ((byKey || byPointer) && openMenu != menu.id)
    {
        // Focus goes back to the control when it closes: the one focused, or
        // the one clicked (the element, or a control drawn in it).
        menuReturn = focus;
        if (byPointer)
            for (const Accessible& n : accessTree.nodes)
                if (n.actions & Accessible::focus
                    && controlIn (reinterpret_cast<const void*> (uintptr_t (n.id)), element))
                {
                    menuReturn = reinterpret_cast<const void*> (uintptr_t (n.id));
                    break;
                }
        const Clay_BoundingBox box = Clay_GetElementData (element).boundingBox;
        menuAt =
            byPointer ? Rect { mouse.x, mouse.y, 0, 0 } : Rect { box.x * s, box.y * s, box.width * s, box.height * s };
        openMenu = menu.id, menuFor = element.id, focus = me, focusVisible = byKey;
        highlight = byKey ? next (items, count, -1, 1) : -1;
        submenuPath.clear(), aiming = false;
        mouse.secondary = false; // taken: not also an element around this one
    }
    else if (byKey)
        focus = me; // open already (a screen reader asked as the key came): it keeps focus
    if (openMenu != menu.id)
        return -1;
    menuSeen = true;

    // The lists shown: the menu, then each submenu open in the one before.
    struct Level
    {
        const MenuItem* items;
        int count;
    };
    std::vector<Level> levels { { items, count } };
    for (int at : submenuPath)
    {
        const Level& l = levels.back();
        if (at < 0 || at >= l.count || !l.items[at].label || !l.items[at].submenu || l.items[at].count < 1)
            break;
        levels.push_back ({ l.items[at].submenu, l.items[at].count });
    }
    submenuPath.resize (levels.size() - 1); // (the caller's lists changed)
    // The first list's items have the ids they always had; a submenu's, and
    // its box's, go by its depth and the item it's from.
    auto itemId = [&] (size_t level, int i) {
        return level ? id ("ContextMenuItem", int (level << 20 | size_t (submenuPath[level - 1]) << 10) + i)
                     : id ("ContextMenuItem", i);
    };
    auto boxId = [&] (size_t level) {
        return level ? id ("ContextSubmenu", int (level << 10) + submenuPath[level - 1]) : menu;
    };
    auto closeTo = [&] (size_t level) {
        levels.resize (level + 1), submenuPath.resize (level);
    }; // the submenus deeper than level
    // Opens the submenu of item i of the deepest list, on its first item, or
    // on none (the pointer's); false if it has none.
    auto openSubmenu = [&] (int i, bool onFirst) {
        const MenuItem& item = levels.back().items[i];
        if (!item.submenu || item.count < 1)
            return false;
        submenuPath.push_back (i), levels.push_back ({ item.submenu, item.count });
        highlight = onFirst ? next (item.submenu, item.count, -1, 1) : -1;
        return true;
    };

    // A screen reader focused or clicked an item: the menu has focus, on it,
    // and the submenus beyond its list close.
    bool found = false;
    for (size_t l = 0; l < levels.size() && !found; ++l)
        for (int i = 0; i < levels[l].count && !found; ++i)
            if (levels[l].items[i].label && focus == asId (itemId (l, i)))
                focus = me, closeTo (l), highlight = i, found = true;
    if (focus != me) // something else took focus (a screen reader moved it)
    {
        openMenu = menuFor = 0;
        return -1;
    }
    if (mouse.pressed && overPopup)
        pressFocused = true; // a press in it, between items, keeps focus in it

    // Keys, as the WAI-ARIA menu's: this frame's are all the menu's. They
    // act in the deepest list open; Right opens a submenu, Left and Escape
    // close it.
    int chosen = -1;
    bool picked = false, close = false;
    auto choose = [&] (int i) { // a leaf is chosen; one with a submenu opens it
        if (i < 0 || openSubmenu (i, true))
            return;
        chosen = levels.back().items[i].id, picked = true;
    };
    auto back = [&] { highlight = submenuPath.back(), closeTo (levels.size() - 2); };
    focusUses = keyBit (View::Key::up) | keyBit (View::Key::down) | keyBit (View::Key::left) | keyBit (View::Key::right)
                | keyBit (View::Key::home) | keyBit (View::Key::end) | keyBit (View::Key::enter)
                | keyBit (View::Key::escape) | keyBit (View::Key::tab);
    focusTakesDigits = false, focusTakesLetters = true; // letters jump to an item, Space chooses
    for (auto [key, modifiers] : keys)
    {
        const MenuItem* list = levels.back().items;
        const int n = levels.back().count;
        aiming = false;
        switch (key)
        {
            case View::Key::down:
                highlight = next (list, n, highlight, 1);
                break;
            case View::Key::up:
                highlight = next (list, n, highlight < 0 ? n : highlight, -1);
                break;
            case View::Key::home:
                highlight = next (list, n, -1, 1);
                break;
            case View::Key::end:
                highlight = next (list, n, n, -1);
                break;
            case View::Key::right:
                if (highlight >= 0)
                    openSubmenu (highlight, true);
                break;
            case View::Key::left:
                if (levels.size() > 1)
                    back();
                break;
            case View::Key::enter:
                choose (highlight);
                break;
            case View::Key::escape:
                levels.size() > 1 ? back() : void (close = true);
                break;
            case View::Key::tab:
                close = true;
                break;
            default:
                break;
        }
    }
    keys.clear();
    for (char c : std::exchange (typedToFocus, {}))
    {
        const Level l = levels.back();
        if (c == ' ')
            choose (highlight);
        else if (const int i = startingWith ([&] (int i) { return l.items[i].label; }, l.count, highlight, c); i >= 0)
            highlight = i;
    }
    if (picked || close) // by a key: gone before it's drawn again
    {
        closeMenu(), focusVisible = true;
        focusHasMenu |= controlIn (focus, element);
        return chosen;
    }

    // The pointer: over an item it highlights it, and opens its submenu.
    // Over another item of a list whose submenu is open, heading for that
    // submenu (inside the triangle from where it was last frame to the
    // submenu's near side), it leaves the submenu open until it stops a
    // moment. A click chooses an item, or opens its submenu.
    const float px = mouse.x / s, py = mouse.y / s;
    const auto now = std::chrono::steady_clock::now();
    int overLevel = -1, overItem = -1, clickedLevel = -1, clickedItem = -1;
    uint32_t clickedId = 0;
    for (size_t l = 0; l < levels.size(); ++l)
        for (int i = 0; i < levels[l].count; ++i)
        {
            if (!levels[l].items[i].label)
                continue;
            const Clay_ElementId item = itemId (l, i);
            if (Ui::element (item, eachPress).clicked)
                clickedLevel = int (l), clickedItem = i, clickedId = item.id;
            if (Clay_PointerOver (item))
                overLevel = int (l), overItem = i;
        }
    if (overLevel >= 0 && (mouse.moved || mouse.pressed || aiming))
    {
        const size_t l = size_t (overLevel);
        if (l + 1 == levels.size())
            highlight = overItem, aiming = false, openSubmenu (overItem, false);
        else if (overItem == submenuPath[l])
            closeTo (l + 1), highlight = -1, aiming = false; // back on the item it's from
        else
        {
            const Clay_BoundingBox sub = Clay_GetElementData (boxId (l + 1)).boundingBox,
                                   from = Clay_GetElementData (boxId (l)).boundingBox;
            const float edge = sub.x >= from.x ? sub.x : sub.x + sub.width;
            auto side = [&] (float x0, float y0, float x1, float y1) {
                return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
            };
            const float a = side (aimX, aimY, edge, sub.y), b = side (edge, sub.y, edge, sub.y + sub.height),
                        c = side (edge, sub.y + sub.height, aimX, aimY);
            const bool towards = mouse.moved && ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0));
            if (towards)
                aimSince = now;
            aiming = towards
                     || (aiming && !mouse.moved && !mouse.pressed && now - aimSince < std::chrono::milliseconds (300));
            if (aiming)
                tooltipWaiting = true; // (drawn on until it's decided, as a tooltip's delay)
            else
                closeTo (l), highlight = overItem, openSubmenu (overItem, false);
        }
    }
    aimX = px, aimY = py;
    if (clickedLevel >= 0 && size_t (clickedLevel) < levels.size()
        && itemId (size_t (clickedLevel), clickedItem).id == clickedId)
    {
        const size_t l = size_t (clickedLevel);
        if (l + 1 == levels.size() || submenuPath[l] != clickedItem)
            closeTo (l), choose (clickedItem); // (not one already open)
    }
    if (picked)
    {
        closeMenu();
        focusHasMenu |= controlIn (focus, element);
        return chosen;
    }

    // Each list: at the pointer, or below the control (points); moved in
    // where it would leave the view, above where there's no room below, and
    // over the control's top where there's room on neither side. A submenu
    // beside the item it's from, its first item level with it; on the other
    // side where there's no room, and moved in where there's none there.
    submenuBoxes.clear();
    const float tick = theme.textSize; // the check marks' column
    for (size_t l = 0; l < levels.size(); ++l)
    {
        const MenuItem* list = levels[l].items;
        const int n = levels[l].count;
        float width = 0, height = 4;
        bool checks = false, arrows = false;
        for (int i = 0; i < n; ++i)
        {
            if (list[i].label)
                width = std::max (width, draw.measure (theme.textSize, list[i].label, theme.labelFont));
            height += list[i].label ? theme.textSize * 1.25f + 6 : 7;
            checks |= list[i].label && list[i].checked >= 0;
            arrows |= list[i].label && list[i].submenu && list[i].count > 0;
        }
        width += 16 + (checks ? tick : 0) + (arrows ? 19 : 0);
        float x = 0, y = 0;
        if (l == 0)
        {
            const Rect at { menuAt.x / s, menuAt.y / s, menuAt.w / s, menuAt.h / s };
            const float gap = at.h > 0 ? 2.0f : 0.0f;
            x = at.x, y = at.y + at.h + gap;
            if (x + width > viewWidth)
                x = std::max (0.0f, at.x + at.w - width);
            if (y + height > viewHeight && at.y - height - gap >= 0)
                y = at.y - height - gap;
            else if (y + height > viewHeight)
                y = std::max (0.0f, std::min (at.y, viewHeight - height)); // (a plot filling the view)
        }
        else
        {
            const Clay_BoundingBox from = Clay_GetElementData (boxId (l - 1)).boundingBox;
            const Clay_BoundingBox item = Clay_GetElementData (itemId (l - 1, submenuPath[l - 1])).boundingBox;
            x = from.x + from.width;
            if (x + width > viewWidth)
                x = from.x - width;
            if (x < 0)
                x = std::max (0.0f, viewWidth - width);
            y = std::max (0.0f, std::min (item.y - 1, viewHeight - height));
            submenuBoxes.push_back (boxId (l).id);
        }
        const Clay_FloatingElementConfig floating = {
            .offset = { x, y }, .zIndex = int16_t (100 + l), .attachTo = CLAY_ATTACH_TO_ROOT
        };
        CLAY ({ .id = boxId (l),
            .layout = { .padding = CLAY_PADDING_ALL (1), .layoutDirection = CLAY_TOP_TO_BOTTOM },
            .backgroundColor = clayColor (
                { theme.background.r, theme.background.g, theme.background.b, theme.background.a * menuOpacity }),
            .cornerRadius = CLAY_CORNER_RADIUS (theme.radius),
            .floating = floating,
            .border = { .color = clayColor (theme.edge), .width = CLAY_BORDER_OUTSIDE (1) },
            .userData = layout.shadow (theme.shadow, 10, 0, 3) })
        {
            // The highlighted item, or in a list with a submenu open, the item it's from.
            const int lit = l + 1 < levels.size() ? submenuPath[l] : highlight;
            for (int i = 0; i < n; ++i)
            {
                if (!list[i].label)
                {
                    CLAY ({ .layout = { .sizing = { .width = CLAY_SIZING_GROW (0) }, .padding = { 0, 0, 3, 3 } } })
                    {
                        CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIXED (1) } },
                            .backgroundColor = clayColor (theme.edge) })
                        {}
                    }
                    continue;
                }
                CLAY ({ .id = itemId (l, i),
                    .layout = { .sizing = { .width = CLAY_SIZING_GROW (0) },
                        .padding = { 7, 7, 3, 3 },
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = clayColor (i == lit ? theme.panel : Color { 0, 0, 0, 0 }) })
                {
                    if (checks)
                        CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIXED (tick), CLAY_SIZING_FIT (0) } } })
                        {
                            if (list[i].checked == 1)
                                CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIXED (tick * 0.6f),
                                                        CLAY_SIZING_FIXED (tick * 0.5f) } },
                                    .custom = { .customData = layout.custom (tickBox) } })
                                {}
                        }
                    CLAY_TEXT (layout.text (list[i].label), textConfig (theme.text));
                    if (arrows)
                    {
                        CLAY ({ .layout = { .sizing = { .width = CLAY_SIZING_GROW (12) } } }) {}
                        const bool opens = list[i].submenu && list[i].count > 0;
                        CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIXED (7),
                                                CLAY_SIZING_FIXED (opens ? 7.0f : 0.0f) } },
                            .custom = { .customData = opens ? layout.custom (submenuBox) : nullptr } })
                        {}
                    }
                }
            }
        }
    }

    // For screen readers: a menu named for its control, its items in it, and
    // the highlighted one focused (as aria-activedescendant). An item with a
    // submenu can open it (expand) and says whether it's open; a submenu is a
    // menu in that item, named for it.
    const size_t top = described.size();
    const char* name = "Menu";
    for (const Accessible& n : accessTree.nodes)
        if (n.id == accessId (menuReturn) && !n.name.empty())
            name = n.name.c_str();
    describe (me, Accessible::Role::menu, name).element = menu.id;
    uint64_t parent = accessId (me), activeId = 0;
    for (size_t l = 0; l < levels.size(); ++l)
    {
        if (l)
        {
            const Clay_ElementId from = itemId (l - 1, submenuPath[l - 1]);
            const void* sub = asId (boxId (l));
            describe (
                sub, Accessible::Role::menu, levels[l - 1].items[submenuPath[l - 1]].label, accessId (asId (from)))
                .element = boxId (l).id;
            parent = accessId (sub);
        }
        for (int i = 0; i < levels[l].count; ++i)
        {
            const MenuItem& item = levels[l].items[i];
            if (!item.label)
                continue;
            const Clay_ElementId e = itemId (l, i);
            Described& d =
                describe (asId (e), item.checked >= 0 ? Accessible::Role::menuItemCheckBox : Accessible::Role::menuItem,
                    item.label, parent);
            d.element = e.id, d.node.actions |= Accessible::click,
            d.node.checked = item.checked >= 0 ? int (item.checked == 1) : -1;
            if (item.submenu && item.count > 0)
                d.node.actions |= Accessible::expand | Accessible::collapse,
                    d.node.expanded = l + 1 < levels.size() && submenuPath[l] == i;
            if (l + 1 == levels.size() && i == highlight)
                activeId = d.node.id;
        }
    }
    // A submenu the pointer opened has nothing highlighted: the item it's from.
    if (!activeId && levels.size() > 1)
        activeId = accessId (asId (itemId (levels.size() - 2, submenuPath.back())));
    described[top].node.activeDescendant = activeId;
    return -1;
}

void Ui::tooltip (const char* text)
{
    if (!lastElement)
        return;
    said.push_back ({ lastElement, {}, text ? text : "", false, false, true }); // as HTML's title: its description
    const Clay_ElementId target { lastElement, 0, 0, {} };
    if (!Clay_PointerOver (target) || mouse.down || active || openMenu) // (nor over an open menu)
    {
        if (hovered == lastElement)
            hovered = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (hovered != lastElement)
        hovered = lastElement, hoveredSince = now;
    if (now - hoveredSince < std::chrono::milliseconds (500))
    {
        tooltipWaiting = true; // the presenter keeps drawing until it shows
        return;
    }
    // Below the control, left-aligned with it; flipped where that would leave the view.
    const Clay_BoundingBox box = Clay_GetElementData (target).boundingBox;
    const float tipWidth = draw.measure (theme.textSize, text, theme.labelFont) + 12;
    const float tipHeight = theme.textSize * 1.25f + 6;
    const bool left = box.x + tipWidth > viewWidth, up = box.y + box.height + 4 + tipHeight > viewHeight;
    const Clay_FloatingAttachPointType element =
        up ? (left ? CLAY_ATTACH_POINT_RIGHT_BOTTOM : CLAY_ATTACH_POINT_LEFT_BOTTOM)
           : (left ? CLAY_ATTACH_POINT_RIGHT_TOP : CLAY_ATTACH_POINT_LEFT_TOP);
    const Clay_FloatingAttachPointType parent =
        up ? (left ? CLAY_ATTACH_POINT_RIGHT_TOP : CLAY_ATTACH_POINT_LEFT_TOP)
           : (left ? CLAY_ATTACH_POINT_RIGHT_BOTTOM : CLAY_ATTACH_POINT_LEFT_BOTTOM);
    CLAY ({ .layout = { .padding = { 6, 6, 3, 3 } },
        .backgroundColor = clayColor (theme.text),
        .cornerRadius = CLAY_CORNER_RADIUS (theme.radius),
        .floating = { .offset = { 0, up ? -4.0f : 4.0f },
            .parentId = lastElement,
            .zIndex = 200,
            .attachPoints = { element, parent },
            .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
            .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID },
        .userData = layout.shadow (theme.shadow, 6, 0, 2) })
    {
        CLAY_TEXT (layout.text (text), textConfig (theme.background));
    }
}

// A parameter's value as an item of a segmented control or dropdown.
static int itemOf (const Param& p, int count)
{
    return std::clamp (int (std::lround (p.value - p.min)), 0, count - 1);
}

bool Ui::toggle (Params& params, Param* p, const char* label)
{
    if (!p)
        return false;
    // Stepped, only the top step is on (a 0..2's 1 is off, not past halfway).
    bool on = p->stepped ? p->value >= p->max : p->value >= (p->min + p->max) / 2;
    if (!toggle (label, on, p->id))
        return false;
    set (params, *p, on ? p->max : p->min);
    return true;
}

bool Ui::segmented (Params& params, Param* p, const char* const* items, int count)
{
    if (!p || count < 1)
        return false;
    int selected = itemOf (*p, count);
    const bool chose = segmented (items, count, selected, p->id);
    label (p->name.c_str()); // named for its parameter, as a knob is; a label after it still wins
    if (!chose)
        return false;
    set (params, *p, p->min + selected);
    return true;
}

bool Ui::dropdown (Params& params, Param* p, const char* const* items, int count)
{
    if (!p || count < 1)
        return false;
    int selected = itemOf (*p, count);
    const bool chose = dropdown (items, count, selected, p->id);
    label (p->name.c_str()); // named for its parameter, as a knob is; a label after it still wins
    if (!chose)
        return false;
    set (params, *p, p->min + selected);
    return true;
}

void Ui::knob (Params& params, Param* p, const Format& format, bool log, float diameter)
{
    if (!p)
        return;
    const Clay_ElementId kid = id ("Knob", p->id);
    CLAY ({ .id = kid,
        .layout = { .sizing = { CLAY_SIZING_FIXED (std::max (diameter, 56.0f)),
                        CLAY_SIZING_FIXED (diameter + 3.2f * theme.textSize) } },
        .custom = {
            .customData = layout.custom (knobBox, &placed.emplace_back (Placed { &params, p, format, log })) } })
    {}
    lastElement = kid.id;
}

void Ui::slider (Params& params, Param* p, const Format& format, bool log)
{
    if (!p)
        return;
    const Clay_ElementId sid = id ("Slider", p->id);
    CLAY ({ .id = sid,
        .layout = { .sizing = { CLAY_SIZING_GROW (80), CLAY_SIZING_FIXED (theme.textSize * 2.6f + 6) } },
        .custom = {
            .customData = layout.custom (sliderBox, &placed.emplace_back (Placed { &params, p, format, log })) } })
    {}
    lastElement = sid.id;
}

// Upright: the value above, the name below, filled from the bottom.
void Ui::vslider (Params& params, Param* p, Rect box, const Format& format, bool log)
{
    if (!p)
        return;
    const float size = theme.textSize * s, cx = box.x + box.w / 2;
    const float top = box.y + 1.6f * size, bottom = box.y + box.h - 1.6f * size, thumb = 1.125f * size;
    const void* me = controlId (*p, box.x, box.y);
    const bool dragging = control (params, *p, me, box.x, top - thumb / 2, box.w, bottom - top + thumb, true,
        bottom - top, log, false, sliderDrag == Drag::absolute, thumb / 2);
    const bool hot = dragging || (!active && over (box.x, top, box.w, bottom - top));
    const float u = float (toUnit (*p, p->value, log)), y = bottom - (bottom - top) * u;
    if (focusShown (me))
        draw.rect (box.x, box.y, box.w, box.h, theme.radius * s, { 0, 0, 0, 0 }, 1 * s, theme.accent);
    const std::string value = format (p->value);
    describeParam (me, params, *p, value.c_str(), box).node.orientation = 2;
    draw.text (cx, box.y + size, size, value, theme.dim, Draw2D::Align::center, theme.valueFont);
    draw.text (
        cx, box.y + box.h - 0.3f * size, size, format.name (*p), theme.dim, Draw2D::Align::center, theme.labelFont);
    draw.rect (cx - 1.5f * s, top, 3 * s, bottom - top, 0, theme.track);
    const float start = bottom - (bottom - top) * fillStart (*p, format, log);
    draw.rect (cx - 1.5f * s, std::min (y, start), 3 * s, std::abs (start - y), 0, theme.accent);
    const float thumbHeight = (hot ? 3.0f : 2.0f) * s;
    draw.rect (cx - thumb / 2, y - thumbHeight / 2, thumb, thumbHeight, 0, theme.text);
}

// Two parameters, x and y, set by dragging a point; double-click resets both.
void Ui::xyPad (Params& params, Param* px, Param* py, Rect r)
{
    if (!px || !py)
        return;
    // Its own drag id: knobs and sliders of px already use px itself.
    const auto a = area (&px->dragging, r.x, r.y, r.w, r.h);
    auto setFrom = [&] (float x, float y) {
        params.change (*px, fromUnit (*px, (x - r.x) / r.w, false));
        params.change (*py, fromUnit (*py, 1 - (y - r.y) / r.h, false));
    };
    const void* me = &px->dragging;
    order.push_back (me);
    if (a.pressed || a.doubleClicked)
        focus = me, pressFocused = true;
    if (a.doubleClicked)
        set (params, *px, px->defaultValue), set (params, *py, py->defaultValue);
    if (a.pressed)
        params.begin (*px), params.begin (*py), held = { &params, px, py, false };
    // Keys, as two sliders': left and right move x, up and down y; Shift
    // fine, Alt a page's step; Delete or Backspace reset both.
    if (focus == me)
    {
        focusUses = stepKeys | keyBit (View::Key::backspace) | keyBit (View::Key::deleteForward);
        focusTakesDigits = false;
        for (auto [key, modifiers] : keys)
        {
            const double step = modifiers & View::shift ? 0.001 : modifiers & View::alt ? 0.1 : 0.01;
            auto nudge = [&] (Param& q, double by) { set (params, q, moved (q, by, false)); };
            switch (key)
            {
                case View::Key::left:
                    nudge (*px, -step);
                    break;
                case View::Key::right:
                    nudge (*px, step);
                    break;
                case View::Key::up:
                    nudge (*py, step);
                    break;
                case View::Key::down:
                    nudge (*py, -step);
                    break;
                case View::Key::backspace:
                case View::Key::deleteForward:
                    set (params, *px, px->defaultValue), set (params, *py, py->defaultValue);
                    break;
                default:
                    break;
            }
        }
    }
    // For screen readers: a group of its two sliders.
    Described& group = describe (me, Accessible::Role::group, (px->name + " and " + py->name).c_str());
    group.node.x = r.x, group.node.y = r.y, group.node.width = r.w, group.node.height = r.h;
    char text[32];
    std::snprintf (text, sizeof (text), "%g", px->value);
    describeParam (reinterpret_cast<const char*> (me) + 1, params, *px, text, r).parent = accessId (me);
    std::snprintf (text, sizeof (text), "%g", py->value);
    describeParam (reinterpret_cast<const char*> (me) + 2, params, *py, text, r).parent = accessId (me);
    if (a.held)
        setFrom (a.x, a.y);
    if (a.released)
        params.end (*px), params.end (*py), held = {};
    const float x = r.x + r.w * float (toUnit (*px, px->value, false)),
                y = r.y + r.h * (1 - float (toUnit (*py, py->value, false)));
    draw.rect (
        r.x, r.y, r.w, r.h, theme.radius * s, { 0, 0, 0, 0 }, 1 * s, focusShown (me) ? theme.accent : theme.edge);
    draw.line (x, r.y, x, r.y + r.h, 1 * s, theme.edge);
    draw.line (r.x, y, r.x + r.w, y, 1 * s, theme.edge);
    draw.circle (x, y, (a.held ? 7.0f : 6.0f) * s, theme.accent, 1.5f * s, theme.text);
    const float size = theme.textSize * s;
    draw.text (r.x + 4 * s, r.y + r.h - 4 * s, size, px->name, theme.dim, Draw2D::Align::left, theme.labelFont);
    draw.text (r.x + 4 * s, r.y + size + 2 * s, size, py->name, theme.dim, Draw2D::Align::left, theme.labelFont);
}

// A level, 0 to 1, filled from the bottom, with a line at the peak (if 0 to 1).
void Ui::meter (float level, float peak, Rect r)
{
    level = std::clamp (level, 0.0f, 1.0f);
    draw.rect (r.x, r.y, r.w, r.h, 0, theme.track);
    draw.rect (r.x, r.y + r.h * (1 - level), r.w, r.h * level, 0, theme.accent);
    if (peak >= 0 && peak <= 1)
        draw.rect (r.x, r.y + r.h * (1 - peak) - 0.5f * s, r.w, 1 * s, 0, theme.text);
}

void Ui::vslider (Params& params, Param* p, const Format& format, bool log, float heightPoints)
{
    if (!p)
        return;
    const Clay_ElementId vid = id ("VSlider", p->id);
    CLAY ({ .id = vid,
        .layout = { .sizing = { CLAY_SIZING_FIXED (std::max (40.0f, 4 * theme.textSize)),
                        heightPoints > 0 ? CLAY_SIZING_FIXED (heightPoints) : CLAY_SIZING_GROW (80) } },
        .custom = {
            .customData = layout.custom (vsliderBox, &placed.emplace_back (Placed { &params, p, format, log })) } })
    {}
    lastElement = vid.id;
}

void Ui::xyPad (Params& params, Param* px, Param* py)
{
    if (!px || !py)
        return;
    const Clay_ElementId xid = id ("XYPad", px->id * 1024 + py->id);
    CLAY ({ .id = xid,
        .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) } },
        .custom = {
            .customData = layout.custom (xyBox, &placed.emplace_back (Placed { &params, px, nullptr, false, py })) } })
    {}
    lastElement = xid.id;
}

void Ui::meter (float level, float peak, float widthPoints)
{
    CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIXED (widthPoints), CLAY_SIZING_GROW (0) } },
        .custom = { .customData = layout.custom (meterBox,
                        &placed.emplace_back (Placed { nullptr, nullptr, nullptr, false, nullptr, level, peak })) } })
    {}
}

void Ui::imageKnob (Params& params, Param* p, int image, int frames, Rect r, bool log, bool across)
{
    if (!p || frames < 1)
        return;
    const void* me = controlId (*p, r.x, r.y);
    const bool dragging = control (params, *p, me, r.x, r.y, r.w, r.h, true, 200 * s, log);
    describeParam (me, params, *p, plainNumber (p->value).c_str(), r).node.orientation =
        2; // dragged up and down, as a knob
    const int frame = int (std::lround (toUnit (*p, p->value, log) * (frames - 1)));
    const float step = 1.0f / float (frames), a = frame * step, b = a + step;
    draw.image (image, r.x, r.y, r.w, r.h, {}, { 1, 1, 1, 1 },
        across ? Draw2D::Source { a, 0, b, 1 } : Draw2D::Source { 0, a, 1, b });
    if (focusShown (me) || dragging)
        draw.circle (
            r.x + r.w / 2, r.y + r.h / 2, std::min (r.w, r.h) / 2 + 2 * s, { 0, 0, 0, 0 }, 1 * s, theme.accent);
}

void Ui::imageKnob (Params& params, Param* p, int image, int frames, bool log, bool across)
{
    if (!p || frames < 1)
        return;
    int width, height;
    draw.imageSize (image, width, height);
    const float w = across ? float (width) / frames : float (width),
                h = across ? float (height) : float (height) / frames;
    const Clay_ElementId kid = id ("ImageKnob", p->id);
    Placed placedKnob { &params, p, nullptr, log };
    placedKnob.image = image, placedKnob.frames = frames, placedKnob.across = across;
    CLAY ({ .id = kid,
        .layout = { .sizing = { CLAY_SIZING_FIXED (w), CLAY_SIZING_FIXED (h) } },
        .custom = { .customData = layout.custom (imageKnobBox, &placed.emplace_back (placedKnob)) } })
    {}
    lastElement = kid.id;
}

int& Ui::within (const void* control)
{
    for (auto& [owner, part] : withins)
        if (owner == control)
            return part;
    return withins.emplace_back (control, 0).second;
}

// A screen reader focused or clicked a part (a bar, a point, a key): the
// control has the focus, on that part.
void Ui::takePartFocus (const void* control, int first, int last)
{
    for (int part = first; part <= last; ++part)
        if (focus == partId (control, part))
        {
            focus = control, within (control) = part;
            return;
        }
}

// The part an id is the id of, or -1.
static int partOf (uint64_t id, const void* control)
{
    const uint64_t from = accessId (control);
    return id > from && (id - from) % 2 == 1 ? int ((id - from - 1) / 2) : -1;
}

// What a screen reader reads for a part's level: "50%".
static std::string percent (float value)
{
    return std::to_string (int (std::lround (std::clamp (value, 0.0f, 1.0f) * 100))) + "%";
}

// Which keys of an octave are black, from C.
static bool isBlack (int note)
{
    const int n = ((note % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// A note as a musician says it: "C4" is MIDI 60.
static std::string noteName (int note)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((note % 12) + 12) % 12] + std::to_string (note / 12 - 1);
}

Ui::Keyboard& Ui::keyboardOf (uint32_t id)
{
    for (Keyboard& k : keyboards)
        if (k.id == id)
            return k;
    return keyboards.emplace_back (Keyboard { id });
}

Ui::Notes Ui::piano (int lowest, int highest, Rect r, const std::function<bool (int)>& lit, int index)
{
    return keyboard (id ("Piano", index).id, lowest, highest, r, lit, true, true);
}

Ui::Notes Ui::keyboard (
    uint32_t identity, int lowest, int highest, Rect r, const std::function<bool (int)>& lit, bool play, bool show)
{
    Keyboard& board = keyboardOf (identity);
    Notes notes;
    if (play)
        notes.off = std::exchange (board.owed, -1);
    int whites = 0;
    for (int n = lowest; n <= highest; ++n)
        whites += !isBlack (n);
    if (whites == 0)
        return notes;
    // Whites 20 points wide, the keyboard at that width in the middle of r
    // (narrower only where r is). Blacks in a real piano's proportions, 0.58
    // of a white wide and 0.63 as tall, spaced as one is built: C to E's
    // width shared evenly by its five keys, F to B's by its seven.
    const float keyWidth = std::min (20 * s, r.w / whites), blackWidth = keyWidth * 0.58f, blackHeight = r.h * 0.63f;
    const float left = r.x + (r.w - whites * keyWidth) / 2;
    auto keyRect = [&] (int note) {
        int whitesBefore = 0;
        for (int n = lowest; n < note; ++n)
            whitesBefore += !isBlack (n);
        if (!isBlack (note))
            return Rect { left + whitesBefore * keyWidth, r.y, keyWidth, r.h };
        const int pc = ((note % 12) + 12) % 12;
        const bool lower = pc < 5;                                       // C#, D# in C to E; F#, G#, A# in F to B
        const int slot = lower ? pc + 1 : pc - 4;                        // its slot in the group, from 1: 2, 4 or 6
        const float slotWidth = (lower ? 3.0f : 4.0f) / (lower ? 5 : 7); // in whites
        const float fromGap = (slot - 0.5f) * slotWidth - slot / 2;      // its centre, from the gap it sits in
        const float x = left + (whitesBefore + fromGap) * keyWidth - blackWidth / 2;
        return Rect { x, r.y, blackWidth, blackHeight };
    };
    auto inside = [&] (Rect k) {
        return mouse.x >= k.x && mouse.x < k.x + k.w && mouse.y >= k.y && mouse.y < k.y + k.h;
    };
    // The key under the pointer: a black one, being on top, first.
    int under = -1;
    for (int n = lowest; n <= highest && under < 0; ++n)
        if (isBlack (n) && inside (keyRect (n)))
            under = n;
    for (int n = lowest; n <= highest && under < 0; ++n)
        if (!isBlack (n) && inside (keyRect (n)))
            under = n;

    if (play)
    {
        // Every press plays: a quick second one is the note again, not a double-click.
        const auto a = area (reinterpret_cast<const void*> (uintptr_t (identity)), r.x, r.y, r.w, r.h, eachPress);
        if (a.held && under != board.playing && (under >= 0 || !a.pressed)) // pressed, or slid onto another key
        {
            if (board.playing >= 0)
                notes.off = board.playing;
            board.playing = under, notes.on = under;
        }
        // Let go: its off now, or from the next call for a note begun in this one
        // (a tap within a frame), as a call's off is always of the note before its on.
        if (a.released && board.playing >= 0)
        {
            if (notes.on == board.playing)
                board.owed = board.playing;
            else
                notes.off = board.playing;
            board.playing = -1;
        }
        // A note played or owed: one more frame, so an off owed is sent
        // without waiting for input (an idle host draws only for that).
        settle |= notes.on >= 0 || notes.off >= 0 || board.owed >= 0;
    }
    if (!show)
        return notes;

    // Square keys with a hairline edge; a pressed one in the accent; each C
    // named at its foot ("C4" is MIDI 60), small and muted.
    auto drawKey = [&] (int n) {
        const Rect k = keyRect (n);
        const bool down = n == board.playing || (lit && lit (n));
        const Color fill = down ? theme.accent : isBlack (n) ? theme.blackKey : theme.whiteKey;
        const Color line = down ? theme.accent : isBlack (n) ? theme.blackKey : theme.edge;
        draw.rect (k.x, k.y, k.w, k.h, Draw2D::Corners {}, fill, Draw2D::Sides { 1 * s, 1 * s, 1 * s, 1 * s }, line);
        if (!isBlack (n) && ((n % 12) + 12) % 12 == 0)
        {
            const float size = 0.7f * theme.textSize * s;
            const std::string name = "C" + std::to_string (n / 12 - 1);
            draw.text (k.x + k.w / 2, k.y + k.h - 0.6f * size, size, name, down ? theme.background : theme.dim,
                Draw2D::Align::center, theme.labelFont);
        }
    };
    for (int n = lowest; n <= highest; ++n)
        if (!isBlack (n))
            drawKey (n);
    for (int n = lowest; n <= highest; ++n)
        if (isBlack (n))
            drawKey (n);
    // To screen readers, a picture of a keyboard: playing it is the mouse's
    // job, or MIDI's, not something to walk key by key.
    const std::string range = noteName (lowest) + " to " + noteName (highest);
    accessible (Accessible::Role::image, "Piano", nullptr, r, range.c_str());
    return notes;
}

Ui::Notes Ui::piano (int lowest, int highest, float heightPoints, const std::function<bool (int)>& lit, int index)
{
    const Clay_ElementId pid = id ("Piano", index);
    Placed board { nullptr, nullptr, nullptr, false };
    board.lowest = lowest, board.highest = highest, board.lit = lit, board.keyboard = pid.id;
    CLAY ({ .id = pid,
        .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_FIXED (heightPoints) } },
        .custom = { .customData = layout.custom (pianoBox, &placed.emplace_back (std::move (board))) } })
    {}
    lastElement = pid.id;
    // Played here, where it was laid out last frame (as Clay hit-tests every
    // element), so its notes come back from this call; drawn at end().
    const Rect last = bounds (pid); // (points)
    if (last.w <= 0 || last.h <= 0)
        return { -1, std::exchange (keyboardOf (pid.id).owed, -1) };
    return keyboard (pid.id, lowest, highest, { last.x * s, last.y * s, last.w * s, last.h * s }, lit, true, false);
}

bool Ui::envelope (std::vector<Point>& points, Rect r)
{
    if (points.empty())
        return false;
    bool changed = false;
    const float handle = 5 * s;
    const void* me = &points;
    order.push_back (me);
    takePartFocus (me, 0, int (points.size()) - 1);
    auto toPixels = [&] (Point p) { return Point { r.x + p.x * r.w, r.y + (1 - p.y) * r.h }; };
    auto toUnit = [&] (float x, float y) {
        return Point { std::clamp ((x - r.x) / r.w, 0.0f, 1.0f), std::clamp (1 - (y - r.y) / r.h, 0.0f, 1.0f) };
    };
    int near = -1; // the point under the pointer
    for (size_t i = 0; i < points.size(); ++i)
    {
        const Point p = toPixels (points[i]);
        if (std::abs (mouse.x - p.x) <= handle * 1.6f && std::abs (mouse.y - p.y) <= handle * 1.6f)
            near = int (i);
    }
    const auto a = area (me, r.x, r.y, r.w, r.h); // by its data, as its focus is: two envelopes drag apart
    if (a.pressed || a.doubleClicked)
        focus = me, pressFocused = true;
    if (a.doubleClicked)
    {
        if (near > 0 && near + 1 < int (points.size()))
            points.erase (points.begin() + near), changed = true;
        else if (near < 0)
        {
            const Point p = toUnit (mouse.x, mouse.y);
            auto at =
                std::upper_bound (points.begin(), points.end(), p.x, [] (float x, const Point& q) { return x < q.x; });
            points.insert (at, p), changed = true;
        }
    }
    if (a.pressed)
    {
        grabbedPoint = near;
        if (near >= 0)
            within (me) = near;
    }
    if (a.held && grabbedPoint >= 0 && grabbedPoint < int (points.size()))
    {
        Point p = toUnit (a.x, a.y);
        const size_t i = size_t (grabbedPoint);
        if (i == 0)
            p.x = points[i].x;
        else if (i + 1 == points.size())
            p.x = points[i].x;
        else
            p.x = std::clamp (p.x, points[i - 1].x, points[i + 1].x);
        if (p.x != points[i].x || p.y != points[i].y)
            points[i] = p, changed = true;
    }
    if (a.released)
        grabbedPoint = -1;

    // Keys: Left and Right choose a point, Up and Down move it (Shift for
    // fine, the Page keys coarse), Alt with Left or Right sideways, between
    // its neighbours; Enter adds a point after it, on the line, and Delete or
    // Backspace removes it, neither at an end (as the pointer's).
    int chosen = std::clamp (within (me), 0, int (points.size()) - 1);
    auto level = [&] (int i, float to) {
        to = std::clamp (to, 0.0f, 1.0f);
        if (to != points[size_t (i)].y)
            points[size_t (i)].y = to, changed = true;
    };
    auto sideways = [&] (int i, float by) {
        if (i == 0 || i + 1 == int (points.size()))
            return;
        const float x = std::clamp (points[size_t (i)].x + by, points[size_t (i) - 1].x, points[size_t (i) + 1].x);
        if (x != points[size_t (i)].x)
            points[size_t (i)].x = x, changed = true;
    };
    if (focus == me)
    {
        focusUses =
            stepKeys | keyBit (View::Key::enter) | keyBit (View::Key::backspace) | keyBit (View::Key::deleteForward);
        focusTakesDigits = false;
        for (auto [key, modifiers] : keys)
        {
            const float step = modifiers & View::shift ? 0.01f : 0.05f, now = points[size_t (chosen)].y;
            const bool sideToSide = modifiers & View::alt;
            const int last = int (points.size()) - 1;
            switch (key)
            {
                case View::Key::left:
                    if (sideToSide)
                        sideways (chosen, modifiers & View::shift ? -0.001f : -0.01f);
                    else
                        chosen = std::max (chosen - 1, 0);
                    break;
                case View::Key::right:
                    if (sideToSide)
                        sideways (chosen, modifiers & View::shift ? 0.001f : 0.01f);
                    else
                        chosen = std::min (chosen + 1, last);
                    break;
                case View::Key::up:
                    level (chosen, now + step);
                    break;
                case View::Key::down:
                    level (chosen, now - step);
                    break;
                case View::Key::pageUp:
                    level (chosen, now + 0.25f);
                    break;
                case View::Key::pageDown:
                    level (chosen, now - 0.25f);
                    break;
                case View::Key::home:
                    chosen = 0;
                    break;
                case View::Key::end:
                    chosen = last;
                    break;
                case View::Key::enter:
                    if (chosen < last)
                    {
                        const Point from = points[size_t (chosen)], to = points[size_t (chosen) + 1];
                        points.insert (points.begin() + chosen + 1, Point { (from.x + to.x) / 2, (from.y + to.y) / 2 });
                        ++chosen, changed = true;
                    }
                    break;
                case View::Key::backspace:
                case View::Key::deleteForward:
                    if (chosen > 0 && chosen < last)
                        points.erase (points.begin() + chosen), --chosen, changed = true;
                    break;
                default:
                    break; // Tab: in end()
            }
        }
    }
    // A screen reader steps or sets a point's level.
    for (const Asked& ask : asked)
        if (const int i = partOf (ask.id, me); i >= 0 && i < int (points.size()))
            level (i, ask.action == Accessible::setValue
                          ? float (ask.value)
                          : points[size_t (i)].y + (ask.action == Accessible::increment ? 0.05f : -0.05f));
    chosen = std::clamp (chosen, 0, int (points.size()) - 1);
    within (me) = chosen;

    // For screen readers: a group of sliders, one for each point's level, its
    // time said in its value.
    Described& group = describe (me, Accessible::Role::group, "Envelope");
    group.node.x = r.x, group.node.y = r.y, group.node.width = r.w, group.node.height = r.h;
    group.node.activeDescendant = accessId (partId (me, chosen));
    for (size_t i = 0; i < points.size(); ++i)
    {
        const Point p = toPixels (points[i]);
        const std::string name = "Point " + std::to_string (i + 1);
        Described& d = describe (partId (me, int (i)), Accessible::Role::slider, name.c_str(), accessId (me));
        d.node.value = "time " + percent (points[i].x) + ", level " + percent (points[i].y);
        d.node.numeric = true, d.node.number = points[i].y, d.node.min = 0, d.node.max = 1, d.node.step = 0.05,
        d.node.orientation = 2;
        d.node.x = p.x - handle, d.node.y = p.y - handle, d.node.width = 2 * handle, d.node.height = 2 * handle;
        d.node.actions |= Accessible::increment | Accessible::decrement | Accessible::setValue;
        d.place = float (i);
    }

    // Drawn: a fill under the line, the line, and a handle at each point.
    std::vector<float> xy;
    for (const Point& q : points)
    {
        const Point p = toPixels (q);
        xy.push_back (p.x), xy.push_back (p.y);
    }
    std::vector<float> area = xy;
    area.insert (area.end(), { xy[xy.size() - 2], r.y + r.h, xy[0], r.y + r.h });
    draw.rect (
        r.x, r.y, r.w, r.h, theme.radius * s, { 0, 0, 0, 0 }, 1 * s, focusShown (me) ? theme.accent : theme.edge);
    draw.fill (area.data(), area.size() / 2, { theme.accent.r, theme.accent.g, theme.accent.b, 0.18f });
    draw.polyline (xy.data(), points.size(), 1.5f * s, theme.accent);
    for (size_t i = 0; i < points.size(); ++i)
    {
        const bool hot = active == me ? int (i) == grabbedPoint : !active && int (i) == near;
        draw.circle (xy[2 * i], xy[2 * i + 1], hot ? handle * 1.25f : handle, hot ? theme.accent : theme.background,
            1.5f * s, theme.accent);
    }
    if (focusShown (me))
        draw.circle (xy[2 * size_t (chosen)], xy[2 * size_t (chosen) + 1], handle * 1.9f, { 0, 0, 0, 0 }, 1.5f * s,
            theme.accent);
    return changed;
}

bool Ui::steps (std::vector<float>& values, Rect r)
{
    if (values.empty())
        return false;
    bool changed = false;
    const int count = int (values.size());
    const float width = r.w / float (count);
    const void* me = &values;
    order.push_back (me);
    takePartFocus (me, 0, count - 1);
    const auto a = area (me, r.x, r.y, r.w, r.h); // by its data, as its focus is: two step editors paint apart
    if (a.pressed || a.doubleClicked)
        focus = me, pressFocused = true;
    if (a.held)
    {
        // Paint every bar between the last one and this, so a fast drag leaves no gaps.
        const int step = std::clamp (int ((a.x - r.x) / width), 0, count - 1);
        const float value = std::clamp (1 - (a.y - r.y) / r.h, 0.0f, 1.0f);
        const int from = a.pressed || lastStep < 0 ? step : lastStep;
        for (int i = std::min (from, step); i <= std::max (from, step); ++i)
            if (values[size_t (i)] != value)
                values[size_t (i)] = value, changed = true;
        lastStep = step, within (me) = step;
    }
    if (a.released)
        lastStep = -1;

    // Keys: Left and Right choose a bar, Up and Down change it (Shift for
    // fine, the Page keys coarse), Delete or Backspace clear it.
    int chosen = std::clamp (within (me), 0, count - 1);
    auto change = [&] (int i, float to) {
        to = std::clamp (to, 0.0f, 1.0f);
        if (to != values[size_t (i)])
            values[size_t (i)] = to, changed = true;
    };
    if (focus == me)
    {
        focusUses = stepKeys | keyBit (View::Key::backspace) | keyBit (View::Key::deleteForward);
        focusTakesDigits = false;
        for (auto [key, modifiers] : keys)
        {
            const float step = modifiers & View::shift ? 0.01f : 0.05f, now = values[size_t (chosen)];
            switch (key)
            {
                case View::Key::left:
                    chosen = std::max (chosen - 1, 0);
                    break;
                case View::Key::right:
                    chosen = std::min (chosen + 1, count - 1);
                    break;
                case View::Key::up:
                    change (chosen, now + step);
                    break;
                case View::Key::down:
                    change (chosen, now - step);
                    break;
                case View::Key::pageUp:
                    change (chosen, now + 0.25f);
                    break;
                case View::Key::pageDown:
                    change (chosen, now - 0.25f);
                    break;
                case View::Key::home:
                    chosen = 0;
                    break;
                case View::Key::end:
                    chosen = count - 1;
                    break;
                case View::Key::backspace:
                case View::Key::deleteForward:
                    change (chosen, 0);
                    break;
                default:
                    break; // Tab: in end()
            }
        }
    }
    // A screen reader steps or sets a bar.
    for (const Asked& ask : asked)
        if (const int i = partOf (ask.id, me); i >= 0 && i < count)
            change (i, ask.action == Accessible::setValue
                           ? float (ask.value)
                           : values[size_t (i)] + (ask.action == Accessible::increment ? 0.05f : -0.05f));
    within (me) = chosen;

    // For screen readers: a group of sliders, one for each bar.
    Described& group = describe (me, Accessible::Role::group, "Steps");
    group.node.x = r.x, group.node.y = r.y, group.node.width = r.w, group.node.height = r.h;
    group.node.activeDescendant = accessId (partId (me, chosen));
    for (int i = 0; i < count; ++i)
    {
        const std::string name = "Step " + std::to_string (i + 1);
        Described& d = describe (partId (me, i), Accessible::Role::slider, name.c_str(), accessId (me));
        d.node.value = percent (values[size_t (i)]);
        d.node.numeric = true, d.node.number = values[size_t (i)], d.node.min = 0, d.node.max = 1, d.node.step = 0.05,
        d.node.orientation = 2;
        d.node.x = r.x + i * width, d.node.y = r.y, d.node.width = width, d.node.height = r.h;
        d.node.actions |= Accessible::increment | Accessible::decrement | Accessible::setValue;
        d.place = float (i);
    }
    for (size_t i = 0; i < values.size(); ++i)
    {
        const float x = r.x + i * width, h = r.h * std::clamp (values[i], 0.0f, 1.0f);
        draw.rect (x + 1 * s, r.y, width - 2 * s, r.h, 0, theme.track);
        draw.rect (x + 1 * s, r.y + r.h - h, width - 2 * s, h, 0, theme.accent);
    }
    if (focusShown (me))
        draw.rect (r.x + chosen * width, r.y, width, r.h, 0, { 0, 0, 0, 0 }, 1.5f * s, theme.accent);
    return changed;
}

bool Ui::envelope (std::vector<Point>& points)
{
    Placed editor { nullptr, nullptr, nullptr, false };
    editor.points = &points;
    const Clay_ElementId eid = id ("Envelope", int (reinterpret_cast<uintptr_t> (&points) >> 4));
    CLAY ({ .id = eid,
        .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) } },
        .custom = { .customData = layout.custom (envelopeBox, &placed.emplace_back (std::move (editor))) } })
    {}
    lastElement = eid.id;
    return std::find (changedLast.begin(), changedLast.end(), &points) != changedLast.end();
}

bool Ui::steps (std::vector<float>& values)
{
    Placed editor { nullptr, nullptr, nullptr, false };
    editor.values = &values;
    const Clay_ElementId sid = id ("Steps", int (reinterpret_cast<uintptr_t> (&values) >> 4));
    CLAY ({ .id = sid,
        .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) } },
        .custom = { .customData = layout.custom (stepsBox, &placed.emplace_back (std::move (editor))) } })
    {}
    lastElement = sid.id;
    return std::find (changedLast.begin(), changedLast.end(), &values) != changedLast.end();
}

} // namespace gpu
