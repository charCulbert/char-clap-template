// Ui panels: resize grip, split, handles, drawers and windows.
#include "ui_internal.h"

namespace gpu {

void Ui::resizeGrip(float width, float height)
{
    grip = {0, 0, width, height}; // drawn in end(), on top of the layout
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

// The handle between two areas (a split's divider, a drawer's edge), as
// the WAI-ARIA window splitter: dragged, or focused and moved with the
// arrows (16 points; Shift 1), Home and End for the limits, Enter to
// collapse its area and restore it. size is that area's (points), measured
// from origin, growing in direction sign along the handle's axis (y when
// vertical). Declare its element with handleBox after.
bool Ui::handle(Clay_ElementId hid, const char *name, float &size, float min, float most, bool vertical, float origin, int sign)
{
    const float before = size;
    auto clamp = [&](float v) { return std::clamp(v, min, most); };
    const Area a = element(hid);
    const float along = (vertical ? mouse.y : mouse.x) / s;
    if (a.pressed) splitGrab = sign * (along - origin) - size, focusVisible = false;
    if (a.held) size = clamp(sign * (along - origin) - splitGrab);
    if (a.doubleClicked) size = clamp(min); // as Enter: collapse
    const void *me = reinterpret_cast<const void *>(uintptr_t(hid.id));
    focusable(hid, a);
    for (auto [key, modifiers] : keys)
    {
        if (focus != me) break;
        // The arrows move the handle on screen: its area grows or shrinks as it does.
        const float step = modifiers & View::shift ? 1.0f : 16.0f;
        const bool back = vertical ? key == View::Key::up : key == View::Key::left;
        const bool forth = vertical ? key == View::Key::down : key == View::Key::right;
        if (back || forth) size = clamp(size + (forth ? step : -step) * float(sign));
        if (key == View::Key::home) size = clamp(min);
        if (key == View::Key::end) size = most;
        if (key == View::Key::enter) // collapse the area, or restore it
        {
            const auto saved = std::find_if(splitRestore.begin(), splitRestore.end(), [&](auto &r) { return r.first == hid.id; });
            if (size > min || saved == splitRestore.end())
            {
                if (saved != splitRestore.end()) saved->second = size;
                else splitRestore.push_back({hid.id, size});
                size = clamp(min);
            }
            else size = clamp(saved->second);
        }
    }
    if (splitterAsked == me) size = clamp(float(splitterTo)), splitterAsked = nullptr; // a screen reader's value
    if (focus == me) focusUses |= keyBit(View::Key::home) | keyBit(View::Key::end);
    Described &d = describe(me, Accessible::Role::splitter, name);
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f points", size);
    d.element = hid.id, d.node.value = text, d.node.numeric = true, d.node.number = size, d.node.min = min, d.node.max = most;
    d.node.orientation = vertical ? 1 : 2; // a separator's orientation is its line's
    d.node.actions |= Accessible::increment | Accessible::decrement | Accessible::setValue;
    if ((Clay_PointerOver(hid) && !active) || a.held) cursor = vertical ? View::Cursor::resizeUpDown : View::Cursor::resizeLeftRight;
    return size != before;
}

// The handle's element: 7 points across, a line in it that thickens and
// takes the accent while it's hovered, dragged or focused.
void Ui::handleBox(Clay_ElementId hid, bool vertical)
{
    const bool lit = Clay_PointerOver(hid) || (active && active == reinterpret_cast<const void *>(uintptr_t(hid.id))) || focusShown(hid);
    const auto grow = CLAY_SIZING_GROW(0);
    CLAY({.id = hid, .layout = {.sizing = {vertical ? grow : CLAY_SIZING_FIXED(7), vertical ? CLAY_SIZING_FIXED(7) : grow},
                                .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}})
    {
        const float line = lit ? 2.0f : 1.0f;
        CLAY({.layout = {.sizing = {vertical ? grow : CLAY_SIZING_FIXED(line), vertical ? CLAY_SIZING_FIXED(line) : grow}},
              .backgroundColor = clayColor(lit ? theme.accent : theme.edge)}) {}
    }
}

bool Ui::split(const char *name, float &size, const std::function<void()> &first, const std::function<void()> &second,
               Split options, int index)
{
    const Clay_ElementId sid = id(name, index), did = id("SplitDivider", sid.id);
    const float before = size;
    // The room the first area has, from the last frame's layout.
    const Clay_ElementData laidOut = Clay_GetElementData(sid);
    const Clay_BoundingBox box = laidOut.boundingBox;
    if (!laidOut.found) settle = true; // its room isn't known until it's laid out: one more frame
    const float whole = options.stacked ? box.height : box.width;
    const float most = std::max(options.min, std::min(options.max, whole > 0 ? whole - 7 - options.secondMin : options.max));
    // Drawn within the room it has now, while size keeps what was set: a
    // room that shrinks for a moment (a resize) doesn't lose it.
    float shown = laidOut.found ? std::clamp(size, options.min, most) : size;
    if (handle(did, name, shown, options.min, most, options.stacked, options.stacked ? box.y : box.x, 1)) size = shown;

    // The first area is a share of the room, not a fixed size: Clay counts a
    // fixed size in its parents' minimum, so dragging it bigger made the
    // whole section grow (and the room with it), pushing the second area
    // out of view. Until the room is known, a fixed size for one frame.
    const auto grow = CLAY_SIZING_GROW(0);
    const auto fixed = whole > 0 ? CLAY_SIZING_PERCENT(std::min(1.0f, shown / whole)) : CLAY_SIZING_FIXED(shown);
    // Its areas clip only across it: Clay doesn't wrap text along an axis it
    // clips, so along the split they wrap instead.
    CLAY({.id = sid, .layout = {.sizing = {grow, grow}, .layoutDirection = options.stacked ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT},
          .clip = {.horizontal = !options.stacked, .vertical = options.stacked}})
    {
        CLAY({.layout = {.sizing = {options.stacked ? grow : fixed, options.stacked ? fixed : grow}},
              .clip = {.horizontal = options.stacked, .vertical = !options.stacked}}) { first(); }
        handleBox(did, options.stacked);
        CLAY({.layout = {.sizing = {grow, grow}}, .clip = {.horizontal = options.stacked, .vertical = !options.stacked}}) { second(); }
    }
    lastElement = did.id;
    return size != before;
}

bool Ui::drawer(const char *title, bool &open, float &size, const std::function<void()> &contents, DrawerOptions options, int index)
{
    using Edge = DrawerOptions::Edge;
    const bool across = options.edge == Edge::left || options.edge == Edge::right; // it sizes widths
    const Clay_ElementId xid = id(title, index), hid = id("DrawerHandle", xid.id);
    // Its header: the disclosure button, as the plain drawer's.
    const Clay_ElementData laidOut = Clay_GetElementData(xid);
    const Clay_BoundingBox box = laidOut.boundingBox;
    if (!laidOut.found) settle = true;
    // Its handle resizes it, measured from the docked edge: inwards is bigger.
    if (open && options.resizable)
    {
        const bool fromEnd = options.edge == Edge::bottom || options.edge == Edge::right;
        const float origin = across ? (fromEnd ? box.x + box.width : box.x) : (fromEnd ? box.y + box.height : box.y);
        const float header = across ? 0 : theme.textSize * 1.25f + 8 + 1; // a column's header sits above what the handle sizes
        const std::string name = std::string("Resize ") + title;
        float content = size;
        if (handle(hid, name.c_str(), content, options.min, options.max, !across,
                   fromEnd ? origin - (across ? 0 : header) : origin + header, fromEnd ? -1 : 1))
            size = content;
    }
    const auto grow = CLAY_SIZING_GROW(0), fixed = CLAY_SIZING_FIXED(size);
    auto body = [&] {
        CLAY({.layout = {.sizing = {grow, across ? grow : fixed}, .padding = CLAY_PADDING_ALL(6), .childGap = 6,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .clip = {.vertical = true}}) { contents(); }
    };
    const bool before = open;
    CLAY({.id = xid, .layout = {.sizing = {across ? (open ? fixed : CLAY_SIZING_FIT(0)) : grow, across ? grow : CLAY_SIZING_FIT(0)},
                                .layoutDirection = across ? CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM}})
    {
        if (open && options.resizable && (options.edge == Edge::bottom || options.edge == Edge::right)) handleBox(hid, !across);
        CLAY({.layout = {.sizing = {grow, grow}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            if (open && options.edge == Edge::bottom) body();
            drawer(title, open, index); // the header
            if (before && open && options.edge != Edge::bottom) body();
        }
        if (open && options.resizable && (options.edge == Edge::top || options.edge == Edge::left)) handleBox(hid, !across);
    }
    return open;
}

bool Ui::window(const char *title, Rect &frame, bool &open, const std::function<void()> &contents, WindowLimits limits, int index,
                Area *headerArea)
{
    if (!open) return false;
    const Clay_ElementId wid = id(title, index), hid = id("WindowHeader", wid.id), gid = id("WindowGrip", wid.id),
                         cid = id("WindowClose", wid.id);
    windowsNow.push_back(wid.id);
    if (std::find(windowOrder.begin(), windowOrder.end(), wid.id) == windowOrder.end())
        windowOrder.push_back(wid.id), settle = true; // new: in front, and laid out again so its contents are known as its
    // A press anywhere on it brings it to the front.
    if (mouse.pressed && windowUnder == wid.id)
        std::erase(windowOrder, wid.id), windowOrder.push_back(wid.id);
    auto keepInside = [&] { // within its limits, and inside the view
        frame.w = std::clamp(frame.w, limits.minWidth, std::max(limits.minWidth, std::min(limits.maxWidth, viewWidth)));
        frame.h = std::clamp(frame.h, limits.minHeight, std::max(limits.minHeight, std::min(limits.maxHeight, viewHeight)));
        frame.x = std::clamp(frame.x, 0.0f, std::max(0.0f, viewWidth - frame.w));
        frame.y = std::clamp(frame.y, 0.0f, std::max(0.0f, viewHeight - frame.h));
    };
    // Its close button first, so a press on it isn't the header's drag.
    limits.closable &= limits.header; // its close button is in its header
    const Area close = limits.closable ? element(cid, eachPress) : Area{};
    // Dragged by its header.
    const Area header = element(hid);
    if (headerArea) *headerArea = header; // a dock moving it watches this
    if (header.pressed) windowGrabX = mouse.x / s - frame.x, windowGrabY = mouse.y / s - frame.y;
    if (header.held) frame.x = mouse.x / s - windowGrabX, frame.y = mouse.y / s - windowGrabY;
    // The move pointer over the header, not over its close button (an arrow, as buttons have).
    const bool overClose = limits.closable && Clay_PointerOver(cid);
    if ((Clay_PointerOver(hid) && !overClose && !active) || header.held) cursor = View::Cursor::move;
    // Resized from its corner.
    if (limits.resizable)
    {
        const Area corner = element(gid);
        if (corner.pressed) windowGrabX = frame.x + frame.w - mouse.x / s, windowGrabY = frame.y + frame.h - mouse.y / s;
        if (corner.held) frame.w = mouse.x / s + windowGrabX - frame.x, frame.h = mouse.y / s + windowGrabY - frame.y;
        if ((Clay_PointerOver(gid) && !active) || corner.held) cursor = View::Cursor::resizeDiagonal;
    }
    // Keys, on its header: the arrows move it, Alt and the arrows resize it,
    // Escape closes it (the moves a drag makes, without one: WCAG 2.5.7).
    const void *me = reinterpret_cast<const void *>(uintptr_t(hid.id));
    if (limits.header) focusable(hid, header);
    if (focus == me && limits.header)
    {
        focusUses |= keyBit(View::Key::escape);
        for (auto [key, modifiers] : keys)
        {
            const float step = modifiers & View::shift ? 1.0f : 16.0f;
            const float dx = key == View::Key::left ? -step : key == View::Key::right ? step : 0;
            const float dy = key == View::Key::up ? -step : key == View::Key::down ? step : 0;
            if (limits.resizable && modifiers & View::alt) frame.w += dx, frame.h += dy;
            else frame.x += dx, frame.y += dy;
            if (key == View::Key::escape && limits.closable) open = false;
        }
    }
    keepInside();
    if (!open)
    {
        std::erase(windowsNow, wid.id); // gone next frame
        return false;
    }

    // For screen readers: a dialog named by its title (its header has the
    // focus), what's in it its children; a close button named for it.
    Described &dialog = describe(me, Accessible::Role::dialog, title);
    dialog.element = wid.id;
    windowDialogs.push_back({wid.id, dialog.node.id}); // for its contents, once the frame's laid out (and its scope gone)
    const bool front = !windowOrder.empty() && windowOrder.back() == wid.id;
    const auto z = int16_t(10 + 2 * (std::find(windowOrder.begin(), windowOrder.end(), wid.id) - windowOrder.begin())); // its grip at z + 1, under the next window
    CLAY({.id = wid, .layout = {.sizing = {CLAY_SIZING_FIXED(frame.w), CLAY_SIZING_FIXED(frame.h)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
          .backgroundColor = clayColor(theme.panel), .cornerRadius = CLAY_CORNER_RADIUS(theme.radius + 2),
          .floating = {.offset = {frame.x, frame.y}, .zIndex = z, .attachTo = CLAY_ATTACH_TO_ROOT},
          .border = {.color = clayColor(focusShown(hid) ? theme.accent : front ? theme.text : theme.edge), .width = CLAY_BORDER_OUTSIDE(1)},
          .userData = layout.shadow(theme.shadow, front ? 14.0f : 8.0f, 0, front ? 4.0f : 2.0f)})
    {
        if (limits.header)
        CLAY({.id = hid, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {8, 6, 5, 5}, .childGap = 8,
                                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = clayColor(theme.background)})
        {
            CLAY_TEXT(layout.text(title), textConfig(front ? theme.text : theme.dim));
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            if (limits.closable)
            {
                const bool closed = close.clicked;
                const std::string closeName = std::string("Close ") + title;
                Described &button = describe(reinterpret_cast<const void *>(uintptr_t(cid.id)), Accessible::Role::button, closeName.c_str(),
                                             accessId(me));
                button.element = cid.id, button.node.actions |= Accessible::click;
                CLAY({.id = cid, .layout = {.sizing = {CLAY_SIZING_FIXED(16), CLAY_SIZING_FIXED(16)}, .padding = CLAY_PADDING_ALL(4)},
                      .backgroundColor = clayColor(Clay_PointerOver(cid) ? theme.panel : Color{0, 0, 0, 0}),
                      .cornerRadius = CLAY_CORNER_RADIUS(theme.radius)})
                {
                    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}, .custom = {.customData = layout.custom(closeBox)}}) {}
                }
                if (closed) open = false, std::erase(windowsNow, wid.id); // gone next frame
            }
        }
        // Clipped top to bottom only: Clay doesn't wrap text across an axis it clips.
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(8), .childGap = 8,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .clip = {.vertical = true}})
        {
            if (open) contents();
        }
        if (limits.resizable)
            CLAY({.id = gid, .layout = {.sizing = {CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(12)}},
                  .floating = {.offset = {-3, -3}, .zIndex = int16_t(z + 1),
                               .attachPoints = {CLAY_ATTACH_POINT_RIGHT_BOTTOM, CLAY_ATTACH_POINT_RIGHT_BOTTOM}, .attachTo = CLAY_ATTACH_TO_PARENT},
                  .custom = {.customData = layout.custom(gripBox)}}) {}
    }
    lastElement = hid.id; // label() and hidden() after it are about the window: its node is its header's
    return open;
}

// A header that opens and closes the section under it (compost's drawer,
// ImGui's collapsing header). Declare the section only while it returns true.
bool Ui::drawer(const char *title, bool &open, int index)
{
    // Its id from its title, as a window's: two drawers need no index to
    // differ. (Not the title's own id: a docked drawer's box has that.)
    const Clay_ElementId did = id("DrawerHeader", id(title, index).id);
    const Area a = element(did, eachPress);
    bool toggled = a.clicked;
    for (View::Key key : focusable(did, a)) toggled |= key == View::Key::enter;
    if (toggled) open = !open;
    Described &d = describe(reinterpret_cast<const void *>(uintptr_t(did.id)), Accessible::Role::button, title);
    d.element = did.id, d.node.expanded = open, d.node.actions |= Accessible::click | Accessible::expand | Accessible::collapse;
    const bool hot = Clay_PointerOver(did);
    CLAY({.id = did, .layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = {6, 6, 4, 4}, .childGap = 6,
                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = clayColor(hot ? theme.panel : Color{0, 0, 0, 0}),
          .border = focusShown(did) ? Clay_BorderElementConfig{.color = clayColor(theme.accent), .width = CLAY_BORDER_OUTSIDE(1)}
                                    : Clay_BorderElementConfig{.color = clayColor(theme.edge), .width = {.bottom = 1}}})
    {
        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(7), CLAY_SIZING_FIXED(7)}},
              .custom = {.customData = layout.custom(open ? chevronBox : chevronRightBox)}}) {}
        CLAY_TEXT(layout.text(title), textConfig(theme.text));
    }
    lastElement = did.id;
    return open;
}

}
