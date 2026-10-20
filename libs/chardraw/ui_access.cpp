// Ui accessibility: the tree screen readers see, and the actions they ask for.
#include "ui_internal.h"

namespace gpu
{

// What a role is called, for warnings (no default: each role must have one).
[[maybe_unused]] static const char* roleName (Accessible::Role role) // (warnings are debug builds only)
{
    switch (role)
    {
        case Accessible::Role::window:
            return "window";
        case Accessible::Role::group:
            return "group";
        case Accessible::Role::button:
            return "button";
        case Accessible::Role::checkBox:
            return "check box";
        case Accessible::Role::radioGroup:
            return "radio group";
        case Accessible::Role::radioButton:
            return "radio button";
        case Accessible::Role::comboBox:
            return "combo box (dropdown)";
        case Accessible::Role::slider:
            return "slider";
        case Accessible::Role::spinButton:
            return "spin button (number box)";
        case Accessible::Role::textInput:
            return "text field";
        case Accessible::Role::label:
            return "label";
        case Accessible::Role::image:
            return "image";
        case Accessible::Role::listBox:
            return "list box";
        case Accessible::Role::option:
            return "option";
        case Accessible::Role::splitter:
            return "splitter";
        case Accessible::Role::dialog:
            return "window (dialog)";
        case Accessible::Role::tabList:
            return "tab list";
        case Accessible::Role::tab:
            return "tab";
        case Accessible::Role::tabPanel:
            return "tab panel";
        case Accessible::Role::menu:
            return "menu";
        case Accessible::Role::menuItem:
            return "menu item";
        case Accessible::Role::menuItemCheckBox:
            return "menu item check box";
    }
    return "control";
}

static constexpr uint64_t viewNode = 1; // the root: no control's address

Ui::Described& Ui::describe (const void* me, Accessible::Role role, const char* name, uint64_t parent)
{
    Described& d = described.emplace_back();
    d.node.id = accessId (me), d.node.role = role, d.parent = parent;
    if (name)
        d.node.name = name;
    d.node.actions = Accessible::focus;
    return d;
}

Ui::Described& Ui::describeParam (const void* me, Params& params, Param& p, const char* value, Rect r)
{
    Described& d = describe (me, Accessible::Role::slider, p.name.c_str());
    d.node.value = value, d.node.numeric = true, d.node.number = p.value, d.node.min = p.min, d.node.max = p.max;
    d.node.x = r.x, d.node.y = r.y, d.node.width = r.w, d.node.height = r.h;
    d.node.actions |= Accessible::increment | Accessible::decrement | Accessible::setValue;
    d.params = &params, d.param = &p;
    return d;
}

void Ui::accessible (Accessible::Role role, const char* name, const char* value, Rect r, const char* description)
{
    // Its id: what it is and where, as stable as its place on the page.
    const size_t hash = std::hash<std::string_view> {}(name ? name : "") ^ (size_t (r.x) * 31 + size_t (r.y)) << 1;
    const void* me = reinterpret_cast<const void*> (uintptr_t (hash | 2)); // never the view's id (1) or null
    Described& d = describe (me, role, name);
    if (value)
        d.node.value = value;
    if (description)
        d.node.description = description;
    d.node.x = r.x, d.node.y = r.y, d.node.width = r.w, d.node.height = r.h;
}

void Ui::accessible (Clay_ElementId element, Accessible::Role role, const char* name, const char* description)
{
    const void* me = reinterpret_cast<const void*> (uintptr_t (element.id)); // as element() knows it, for its clicks
    Described& d = describe (me, role, name);
    d.element = element.id, d.node.actions |= Accessible::click;
    if (description)
        d.node.description = description;
}

bool Ui::tabStop (Clay_ElementId element, Accessible::Role role, const char* name)
{
    const void* me = reinterpret_cast<const void*> (uintptr_t (element.id));
    order.push_back (me);
    Described& d = describe (me, role, name);
    d.element = element.id, d.stop = true;
    if (focus == me)
        focusUses = 0, focusTakesDigits = false; // its keys are the host's, but for its menu's
    return focusShown (me);
}

void Ui::label (const char* name)
{
    if (lastElement && name)
        said.push_back ({ lastElement, name, {} });
}

void Ui::description (const char* text)
{
    if (lastElement && text)
        said.push_back ({ lastElement, {}, text });
}

void Ui::hidden()
{
    if (lastElement)
        said.push_back ({ lastElement, {}, {}, true });
}

void Ui::hidden (Clay_ElementId element)
{
    said.push_back ({ element.id, {}, {}, true, true });
}

void Ui::announce (const char* text)
{
    announcement = text ? text : "";
}

// A screen reader focused or clicked a part of a control that isn't a
// control of its own (a segmented control's radio, a tab, a dropdown's
// option, an XY pad's axis, a window's close button): the keys go to the
// control it's part of.
void Ui::focusOwner()
{
    auto inOrder = [&] (uint64_t id) {
        return std::find (order.begin(), order.end(), reinterpret_cast<const void*> (uintptr_t (id))) != order.end();
    };
    if (!focus || inOrder (accessId (focus)))
        return;
    uint64_t at = accessId (focus);
    for (size_t step = 0; step < described.size(); ++step) // up through its parents
    {
        const auto d =
            std::find_if (described.begin(), described.end(), [&] (const Described& d) { return d.node.id == at; });
        if (d == described.end() || !d->parent)
            return; // nobody's part (an open context menu has focus itself)
        at = d->parent;
        if (inOrder (at))
        {
            focus = reinterpret_cast<const void*> (uintptr_t (at));
            settle = true; // drawn once more with it, so it takes the keys it uses
            return;
        }
    }
}

static bool same (const Accessible& a, const Accessible& b)
{
    return a.id == b.id && a.role == b.role && a.name == b.name && a.value == b.value && a.description == b.description
           && a.live == b.live && a.orientation == b.orientation && a.numeric == b.numeric && a.number == b.number
           && a.min == b.min && a.max == b.max && a.step == b.step && a.checked == b.checked && a.expanded == b.expanded
           && a.selected == b.selected && a.activeDescendant == b.activeDescendant && a.x == b.x && a.y == b.y
           && a.width == b.width && a.height == b.height && a.actions == b.actions && a.children == b.children;
}

void Ui::finishDescribing()
{
    asked.clear(); // what a screen reader asked of parts was applied by their controls, in this frame
    // Where each element was laid out, and in what order: reading order is
    // layout order, as a page's is document order.
    const std::vector<Layout::Drawn>& drawn = layout.drawn;
    auto inside = [] (const Rect& r, float x, float y) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    };
    auto within = [&] (
                      const Accessible& n, const Rect& r) { return inside (r, n.x + n.width / 2, n.y + n.height / 2); };
    auto contains = [] (const Rect& outer, const Rect& r) { // with half a pixel to spare
        return r.x >= outer.x - 0.5f && r.y >= outer.y - 0.5f && r.x + r.w <= outer.x + outer.w + 0.5f
               && r.y + r.h <= outer.y + outer.h + 0.5f;
    };
    // An element's box (pixels), whether or not it draws anything itself.
    auto box = [&] (uint32_t element) -> std::optional<Rect> {
        const Clay_ElementData data = Clay_GetElementData ({ element, 0, 0, {} });
        if (!data.found)
            return std::nullopt;
        const Clay_BoundingBox& b = data.boundingBox;
        return Rect { b.x * s, b.y * s, b.width * s, b.height * s };
    };
    // Its place in reading order: the first thing drawn in it (a button's
    // label comes before its border).
    auto place = [&] (const Rect& r) {
        for (size_t i = 0; i < drawn.size(); ++i)
            if (r.w > 0 && contains (r, drawn[i].pixels))
                return float (i);
        return -1.0f;
    };

    // A tab stop and a view drawn at its box (a plot filling it): one node,
    // the stop's, saying what the view says of itself.
    std::vector<bool> merged (described.size());
    for (Described& d : described)
        if (const std::optional<Rect> r = d.stop ? box (d.element) : std::nullopt)
            for (size_t i = 0; i < described.size(); ++i)
            {
                const Accessible& v = described[i].node;
                if (merged[i] || described[i].element || described[i].parent || std::abs (v.x - r->x) > 0.5f
                    || std::abs (v.y - r->y) > 0.5f || std::abs (v.width - r->w) > 0.5f
                    || std::abs (v.height - r->h) > 0.5f)
                    continue;
                if (d.node.description.empty())
                    d.node.description = v.description;
                merged[i] = true;
                break;
            }
    struct Entry
    {
        Accessible node;
        float order;
        uint64_t parent;
        bool hidden;
    };
    std::vector<Entry> entries;
    for (Described& d : described)
    {
        if (merged[size_t (&d - described.data())])
            continue;
        float at = 1e9f + float (entries.size()); // its place in reading order
        if (d.element)                            // laid out by Clay: its box, and its place
        {
            if (const std::optional<Rect> r = box (d.element))
            {
                d.node.x = r->x, d.node.y = r->y, d.node.width = r->w, d.node.height = r->h;
                if (const float first = place (*r); first >= 0)
                    at = first;
            }
        }
        else // drawn at a rectangle: the place of the smallest box around it (not a menu floating over it)
        {
            float smallest = 0;
            for (size_t i = 0; i < drawn.size(); ++i)
            {
                const float area = drawn[i].pixels.w * drawn[i].pixels.h;
                if (!drawn[i].text && within (d.node, drawn[i].pixels) && (smallest == 0 || area < smallest))
                    smallest = area, at = float (i) + 0.5f;
            }
        }
        if (d.place >= 0)
            at = d.place;
        entries.push_back ({ d.node, at, d.parent, false });
    }
    // What the page says around them: its text runs, those a control doesn't
    // already say (a button's label, a knob's value), a line's runs as one.
    for (size_t i = 0; i < drawn.size(); ++i)
    {
        if (!drawn[i].text)
            continue;
        Accessible text;
        text.id = 0x7465787400000000ull | drawn[i].id; // "text", above any address
        text.role = Accessible::Role::label;           // its words are its value, as AccessKit reads a label
        text.value = drawn[i].words;
        Rect r = drawn[i].pixels;
        const size_t first = i;
        auto nextLine = [&] (const Layout::Drawn& line, const Layout::Drawn& next) { // of a wrapped paragraph
            return next.text
                   && (next.id == line.id
                       || (std::abs (next.pixels.x - drawn[first].pixels.x) < 1
                           && std::abs (next.pixels.y - (line.pixels.y + line.pixels.h)) < line.pixels.h / 4));
        };
        while (i + 1 < drawn.size() && nextLine (drawn[i], drawn[i + 1]))
        {
            ++i;
            text.value += " " + drawn[i].words;
            const float right = std::max (r.x + r.w, drawn[i].pixels.x + drawn[i].pixels.w),
                        bottom = drawn[i].pixels.y + drawn[i].pixels.h;
            r.x = std::min (r.x, drawn[i].pixels.x), r.w = right - r.x, r.h = bottom - r.y;
        }
        text.x = r.x, text.y = r.y, text.width = r.w, text.height = r.h;
        bool spoken = false;
        for (const Entry& e : entries)
            spoken |= e.node.width > 0
                      && inside ({ e.node.x, e.node.y, e.node.width, e.node.height }, r.x + r.w / 2, r.y + r.h / 2);
        if (!spoken)
            entries.push_back ({ text, float (first), 0, false });
    }
    // What's inside a window is that window's: the frontmost one around it.
    for (auto w = windowOrder.rbegin(); w != windowOrder.rend(); ++w)
    {
        const std::optional<Rect> r = box (*w);
        // Not one that closed this frame: it didn't describe itself, and a
        // child of a node that isn't there is an error to AccessKit.
        const auto dialog =
            std::find_if (windowDialogs.begin(), windowDialogs.end(), [&] (auto& d) { return d.first == *w; });
        if (!r || dialog == windowDialogs.end())
            continue;
        const uint64_t dialogId = dialog->second;
        for (Entry& e : entries)
            // (Not another window: two in the same place would each take the other.)
            if (!e.parent && e.node.id != dialogId && e.node.role != Accessible::Role::dialog && e.node.width > 0
                && contains (*r, { e.node.x, e.node.y, e.node.width, e.node.height }))
                e.parent = dialogId;
    }
    // label, description, hidden: on the control whose element they name, or
    // that was drawn inside it (a knob in its box). A tooltip's description
    // gives way to one set by hand.
    for (const Said& w : said)
    {
        const std::optional<Rect> r = box (w.element);
        for (Entry& e : entries)
        {
            const Rect at { e.node.x, e.node.y, e.node.width, e.node.height };
            const bool mine = e.node.id == uint64_t (w.element)
                              || (r && e.parent == 0 && (w.everything ? contains (*r, at) : within (e.node, *r)));
            // (Not a context menu open over it: that's named for the control.)
            if (!mine || (w.everything ? false : e.node.role == Accessible::Role::label)
                || e.node.role == Accessible::Role::menu)
                continue;
            if (w.hidden)
                e.hidden = true;
            if (w.menu)
                e.node.actions |= Accessible::showMenu;
            if (!w.label.empty())
                e.node.name = w.label;
            if (!w.description.empty() && !(w.fromTooltip && !e.node.description.empty()))
                e.node.description = w.description;
            if (e.node.description == e.node.name)
                e.node.description.clear(); // said once is enough
            if (!w.everything)
                break;
        }
    }

    // Hidden, and so is all that's in it (a window's children with it).
    for (bool more = true; more;)
    {
        more = false;
        for (Entry& e : entries)
            if (!e.hidden && e.parent)
                for (const Entry& p : entries)
                    if (p.node.id == e.parent && p.hidden)
                        e.hidden = more = true;
    }

    Accessibility tree;
    Accessible& view = tree.nodes.emplace_back();
    view.id = viewNode, view.role = Accessible::Role::window;
    view.width = viewWidth * s, view.height = viewHeight * s;
    std::vector<const Entry*> top;
    for (const Entry& e : entries)
    {
        if (e.hidden)
            continue;
        tree.nodes.push_back (e.node);
        if (!e.parent)
            top.push_back (&e);
    }
    std::stable_sort (top.begin(), top.end(), [] (const Entry* a, const Entry* b) { return a->order < b->order; });
    reading.clear();
    for (const Entry& e : entries)
        reading.push_back ({ e.node.id, e.order });
    for (const Entry* e : top)
        tree.nodes.front().children.push_back (e->node.id);
    std::vector<const Entry*> inner; // a group's, a list's or a window's children, in reading order too
    for (const Entry& e : entries)
        if (e.parent && !e.hidden)
            inner.push_back (&e);
    std::stable_sort (inner.begin(), inner.end(), [] (const Entry* a, const Entry* b) { return a->order < b->order; });
    for (const Entry* e : inner)
        for (Accessible& n : tree.nodes)
            if (n.id == e->parent)
            {
                n.children.push_back (e->node.id);
                break;
            }
    // A live region: screen readers read it as it changes. Once published an
    // announcement is cleared, so the same words again are a change too, and
    // they don't stay on the page as a label. The same words as are shown wait
    // a frame, for the clearing to go first.
    const bool waiting = std::any_of (accessTree.nodes.begin(), accessTree.nodes.end(),
        [&] (const Accessible& n) { return n.live && !announcement.empty() && n.value == announcement; });
    if (announcer || !announcement.empty())
    {
        Accessible& live = tree.nodes.emplace_back();
        live.id = 3, live.role = Accessible::Role::label, live.live = true;
        if (!waiting)
            live.value = std::exchange (announcement, {});
        tree.nodes.front().children.push_back (live.id);
        settle |= !live.value.empty() || waiting; // a frame more, to clear it
        announcer = true;
    }
    tree.focus = viewNode;
    for (const Accessible& n : tree.nodes)
        if (n.id == accessId (focus) || (editing && n.id == uint64_t (editing)))
            tree.focus = n.id; // or the field being edited
    // A focused radio group: its checked radio has the focus, as the
    // WAI-ARIA radio group pattern has it.
    for (const Accessible& n : tree.nodes)
        if (n.id == tree.focus && n.role == Accessible::Role::radioGroup)
            for (uint64_t child : n.children)
                for (const Accessible& c : tree.nodes)
                    if (c.id == child && c.checked == 1)
                        tree.focus = c.id;
    // A focused group with a chosen part (a step sequencer's bar, a piano's
    // key), or a menu with a highlighted item: that has the focus, as
    // aria-activedescendant has it.
    for (const Accessible& n : tree.nodes)
        if (n.id == tree.focus && (n.role == Accessible::Role::group || n.role == Accessible::Role::menu)
            && n.activeDescendant)
        {
            tree.focus = n.activeDescendant;
            break;
        }
#ifndef NDEBUG
    // Every control needs a name a screen reader can say (WCAG 4.1.2).
    for (const Accessible& n : tree.nodes)
        if (n.name.empty() && n.actions & Accessible::focus && n.role != Accessible::Role::window)
        {
            static std::vector<uint64_t> warned;
            if (std::find (warned.begin(), warned.end(), n.id) != warned.end())
                continue;
            warned.push_back (n.id);
            std::fprintf (stderr, "chardraw: a %s has no name for screen readers: give it ui.label(\"...\")\n",
                roleName (n.role));
        }
#endif
    const bool unchanged = tree.focus == accessTree.focus && tree.nodes.size() == accessTree.nodes.size()
                           && std::equal (tree.nodes.begin(), tree.nodes.end(), accessTree.nodes.begin(), same);
    if (unchanged)
        return;
    tree.version = accessTree.version + 1;
    accessTree = std::move (tree);
}

void Ui::accessAction (uint64_t id, Accessible::Action action, double value)
{
    const auto d =
        std::find_if (described.begin(), described.end(), [&] (const Described& d) { return d.node.id == id; });
    if (d == described.end())
        return;
    const void* me = reinterpret_cast<const void*> (uintptr_t (id));
    switch (action)
    {
        case Accessible::focus:
            focus = me, focusVisible = true;
            break;
        case Accessible::click:
            focus = me, accessClick = me;
            break;
        case Accessible::increment: // as the arrow keys would, on the focused control
        case Accessible::decrement:
            if (d->node.role == Accessible::Role::splitter) // its size, more or less, whichever way it lies
            {
                focus = me, focusVisible = true;
                splitterAsked = me,
                splitterTo = std::clamp (
                    d->node.number + (action == Accessible::increment ? 16 : -16), d->node.min, d->node.max);
                break;
            }
            if (d->parent && d->param) // a part of one control (an XY pad's axis): step it directly
            {
                const double by = action == Accessible::increment ? 0.01 : -0.01;
                set (*d->params, *d->param, moved (*d->param, by, false));
                break;
            }
            if (d->parent && d->node.role == Accessible::Role::slider) // a bar or a point: its control steps it
            {
                asked.push_back ({ id, action, value });
                break;
            }
            focus = me, focusVisible = true;
            keys.push_back ({ action == Accessible::increment ? View::Key::up : View::Key::down, 0 });
            break;
        case Accessible::setValue:
            if (d->param)
                set (*d->params, *d->param, value);
            else if (d->parent && d->node.role == Accessible::Role::slider)
                asked.push_back ({ id, action, value });
            // A splitter takes it when it's next drawn, from its caller's size as it is then.
            if (d->node.role == Accessible::Role::splitter)
                splitterAsked = me, splitterTo = std::clamp (value, d->node.min, d->node.max);
            break;
        case Accessible::showMenu: // its context menu, as the Menu key opens it
            focus = me, focusVisible = true;
            keys.push_back ({ View::Key::contextMenu, 0 });
            break;
        case Accessible::expand: // a dropdown, drawer or submenu, opened by its click when it's closed
        case Accessible::collapse:
            focus = me;
            // (A menu item's submenu closes as focus comes back to the item.)
            if (d->node.expanded != (action == Accessible::expand)
                && !(action == Accessible::collapse && d->node.role == Accessible::Role::menuItem))
                accessClick = me;
            break;
    }
}

} // namespace gpu
