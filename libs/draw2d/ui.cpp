// Ui core: input, begin and end, focus, ids, areas and presses.
#include "ui_internal.h"

namespace gpu {

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
    if (event.type == View::Pointer::move) mouse.moved = true;
    if (event.type == View::Pointer::down) mouse.down = true;
    if (event.type == View::Pointer::up) mouse.down = false;
    // While a control is dragged, nothing else is hovered (as in ImGui): Clay
    // keeps the pointer where the press was, however far the drag goes.
    if (active && mouse.down) layout.pointer(lastPressX, lastPressY, true);
    else layout.pointer(mouse.x, mouse.y, mouse.down);
    if (event.type == View::Pointer::leave && !mouse.down) // over nothing now
    {
        // Right after a release this frame hasn't seen (X11 sends the leave
        // as a drag ends outside the view), the release happens where it was:
        // the pointer goes once the frame has seen it.
        if (mouse.released)
        {
            leftAfterRelease = true;
            return;
        }
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
    if (!editing && focus && ((focusTakesDigits && utf8[0] && std::strchr("0123456789.,+-", utf8[0]))
                              || (focusTakesLetters && static_cast<unsigned char>(utf8[0]) >= 0x20)))
    {
        typedToFocus += utf8;
        return true;
    }
    // Space presses a focused button, toggle or drawer (as HTML's do);
    // it comes as typed text, not a key.
    if (!editing && focus && focusTakesSpace && std::strcmp(utf8, " ") == 0)
    {
        keys.push_back({View::Key::enter, 0});
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
    return tooltipWaiting || settle || scrollWaiting || animateAsked || draw.atlasRefilling();
}

void Ui::begin(float width, float height, float scale)
{
    animateAsked = false;
    scopes.clear(); // (each push is popped within a frame)
    s = scale;
    viewWidth = width / scale, viewHeight = height / scale;
    draw.begin(width, height);
    order.clear();
    placed.clear();
    painters.clear();
    described.clear();
    said.clear();
    layout.begin(draw, width, height, scale);
    overPopup = openMenu && Clay_PointerOver({openMenu, 0, 0, {}});
    // The windows still open; the frontmost one under the pointer: controls
    // outside it don't take the pointer.
    std::erase_if(windowOrder, [&](uint32_t w) { return std::find(windowsNow.begin(), windowsNow.end(), w) == windowsNow.end(); });
    windowsNow.clear();
    windowUnder = 0;
    for (auto w = windowOrder.rbegin(); w != windowOrder.rend() && !windowUnder; ++w)
        if (inside({*w, 0, 0, {}}, mouse.x, mouse.y)) windowUnder = *w;
    menuSeen = false;
    lastElement = 0;
    settle = mouse.pressed || mouse.released || !keys.empty() || mouse.wheel != 0;
    focusTakesLetters = focusTakesSpace = false; // a focused dropdown, button or toggle says so again
    cursor = View::Cursor::arrow;                // unless a control under the pointer wants another
    tooltipWaiting = false;
}

void Ui::end(const wgpu::RenderPassEncoder &pass)
{
    record();
    draw.flush(pass);
}

bool Ui::record()
{
    layout.end([&](int kind, void *data, Rect r) {
        const auto *c = static_cast<const Placed *>(data);
        if (kind == knobBox) knob(*c->params, c->p, r, c->format, c->log);
        else if (kind == sliderBox) slider(*c->params, c->p, r, c->format, c->log);
        else if (kind == vsliderBox) vslider(*c->params, c->p, r, c->format, c->log);
        else if (kind == xyBox) xyPad(*c->params, c->p, c->second, r);
        else if (kind == meterBox) meter(c->level, c->peak, r);
        else if (kind == paintBox) (*static_cast<const std::function<void(Rect)> *>(data))(r);
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
        else if (kind == gripBox) // three short diagonals, as a window's resize corner
        {
            for (float d : {0.3f, 0.6f, 0.9f})
                draw.line(r.x + r.w * (1 - d), r.y + r.h, r.x + r.w, r.y + r.h * (1 - d), 1.2f * s, theme.dim);
        }
        else if (kind == closeBox) // a cross
        {
            draw.line(r.x, r.y, r.x + r.w, r.y + r.h, 1.4f * s, theme.text);
            draw.line(r.x + r.w, r.y, r.x, r.y + r.h, 1.4f * s, theme.text);
        }
    });
    finishDescribing();
    // Tab goes in layout order too, not the order controls were declared in
    // (a knob Clay places is drawn after the controls declared beside it).
    auto place = [&](const void *control) {
        for (auto [id, at] : reading)
            if (id == accessId(control)) return at;
        return 1e9f;
    };
    std::stable_sort(order.begin(), order.end(), [&](const void *a, const void *b) { return place(a) < place(b); });
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
        // From a text field being edited: keep its edit (as a click
        // elsewhere does) and go on from the field.
        const void *from = focus;
        if (editing)
        {
            const void *field = reinterpret_cast<const void *>(uintptr_t(editing));
            if (std::find(order.begin(), order.end(), field) != order.end()) from = field;
            pendingField = editing, pendingDraft = draft, editing = 0;
        }
        const auto at = std::find(order.begin(), order.end(), from);
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
    mouse.pressed = mouse.released = mouse.doubleClicked = mouse.moved = false;
    if (std::exchange(leftAfterRelease, false)) mouse.x = mouse.y = -1e9f, layout.pointer(mouse.x, mouse.y, false);
    mouse.wheel = mouse.scroll = 0;
    if (const std::optional<View::PointerEvent> move = std::exchange(heldMove, std::nullopt)) pointer(*move), settle = true;
    keys.clear();
    edits.clear();
    typedToFocus.clear();
    accessClick = nullptr; // for a control no longer there
    splitterAsked = nullptr;
    if (cursor != cursorShown && setCursor) setCursor(cursor), cursorShown = cursor;
    return draw.changed();
}

void *Ui::paint(std::function<void(Rect)> drawing)
{
    return layout.custom(paintBox, &painters.emplace_back(std::move(drawing)));
}

bool Ui::over(float x, float y, float w, float h) const
{
    if (overPopup || mouse.x < x || mouse.x > x + w || mouse.y < y || mouse.y > y + h) return false;
    // Over a window, only what's in that window takes the pointer.
    return !windowUnder || inside({windowUnder, 0, 0, {}}, x + w / 2, y + h / 2);
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

Clay_ElementId Ui::idIn(const void *scope, const char *name, int index)
{
    // In a scope, the index is mixed with the scope's address: the same name and
    // number in two scopes are two ids. Outside any, it's Clay's own.
    uint32_t mixed = uint32_t(index);
    if (scope)
    {
        const uint64_t at = uint64_t(reinterpret_cast<uintptr_t>(scope));
        mixed = mixed * 2654435761u ^ uint32_t(at ^ (at >> 32)) * 40503u;
    }
    return Clay_GetElementIdWithIndex({false, int32_t(std::strlen(name)), name}, mixed);
}

Clay_ElementId Ui::id(const char *name, int index) const { return idIn(scopes.empty() ? nullptr : scopes.back(), name, index); }

void Ui::pushId(const void *scope) { scopes.push_back(scope); }

void Ui::popId()
{
    if (!scopes.empty()) scopes.pop_back();
}

Rect Ui::bounds(Clay_ElementId element) const
{
    const Clay_ElementData d = Clay_GetElementData(element);
    return d.found ? Rect{d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height} : Rect{};
}

bool Ui::pointerOver(Clay_ElementId element) const { return Clay_PointerOver(element); }

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
    if (focus == me) // buttons, toggles, segmented, dropdowns: Enter (and Space), and arrows to choose
        focusUses = keyBit(View::Key::enter) | keyBit(View::Key::left) | keyBit(View::Key::right) | keyBit(View::Key::up)
                  | keyBit(View::Key::down),
        focusTakesDigits = false, focusTakesSpace = true;
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

}
