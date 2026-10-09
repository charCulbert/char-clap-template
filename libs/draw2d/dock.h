#pragma once
#include "ui.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// A dock, as ImGui's: panels in a tree of splits whose leaves are groups of
// tabs, and panels floating in windows over it. Drag a tab, or a floating
// window's header, over the dock: where it would land shows, and letting go
// puts it there: in the middle of a group as a tab, near a group's edge in a
// new split beside it. A tab let go anywhere else floats. Without the mouse
// (WCAG 2.5.7): on a focused tab list, Alt and an arrow move the shown panel
// to that side, Alt and Enter float it; closing a floating window docks it
// back. Built on Ui's public surface: tabs, split, window.
namespace gpu {

class Dock
{
public:
    struct Panel
    {
        const char *name; // unique; also its tab's and window's title
        std::function<void()> contents;
    };
    // The arrangement: plain data, the caller's to keep and save. Panels it
    // doesn't mention join the first group.
    struct Node
    {
        std::vector<std::string> tabs; // a group: its panels (when it has no children)
        int shown = 0;
        bool stacked = false; // a split: one above the other, else side by side
        float size = 0;       // the first child's width or height, points
        std::vector<Node> children; // a split's two, or none
    };
    struct Floating
    {
        std::string panel;
        Rect frame; // points
    };
    struct Layout
    {
        Node root;
        std::vector<Floating> floating;
    } layout;

    // The dock fills the box it's declared in.
    void draw(Ui &ui, const char *name, const std::vector<Panel> &panels)
    {
        strings.clear();
        groups.clear();
        adopt(panels);
        const Clay_ElementId did = Clay_GetElementIdWithIndex(text(std::string(name) + " dock"), 0);
        CLAY({.id = did, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) { node(ui, layout.root, panels, 0); }
        reshaped = false;
        floating(ui, panels);
        dragging(ui);
        // Changes from keys wait until everything's drawn: a panel is drawn
        // once a frame, where it was.
        if (keyed.key != View::Key::tab && !reshaped) keyMove(ui, *keyed.node, keyed.key); // (not after a drop: the tree moved)
        keyed = {};
    }

private:
    enum class Side { middle, left, right, top, bottom };
    struct Group
    {
        Node *node;
        Clay_ElementId id;
    };
    struct Drag
    {
        std::string panel;
        Node *from = nullptr; // its group, or null when it floats
        float pressX = 0, pressY = 0, x = 0, y = 0; // pixels
        bool moving = false, released = false;
    };

    Clay_String text(std::string s) { return {false, int32_t(strings.emplace_back(std::move(s)).size()), strings.back().c_str()}; }

    // Panels the layout doesn't mention go into the first group.
    void adopt(const std::vector<Panel> &panels)
    {
        for (const Panel &p : panels)
        {
            bool known = std::any_of(layout.floating.begin(), layout.floating.end(), [&](const Floating &f) { return f.panel == p.name; });
            known = known || find(layout.root, p.name);
            if (!known) first(layout.root).tabs.push_back(p.name);
        }
    }
    static Node *find(Node &n, const std::string &panel)
    {
        if (std::find(n.tabs.begin(), n.tabs.end(), panel) != n.tabs.end()) return &n;
        for (Node &c : n.children)
            if (Node *f = find(c, panel)) return f;
        return nullptr;
    }
    static Node &first(Node &n) { return n.children.empty() ? n : first(n.children[0]); }
    static std::string names(const Node &n) { return n.children.empty() ? (n.tabs.empty() ? "" : n.tabs[0]) : names(n.children[0]); }

    void node(Ui &ui, Node &n, const std::vector<Panel> &panels, int depth)
    {
        if (n.children.size() == 2)
        {
            if (n.size <= 0) n.size = 200;
            const std::string label = names(n.children[0]) + " and " + names(n.children[1]);
            ui.split(strings.emplace_back(label).c_str(), n.size, [&] { node(ui, n.children[0], panels, depth + 1); },
                     [&] { node(ui, n.children[1], panels, depth + 1); }, {.stacked = n.stacked}, int(groups.size()) * 16 + depth);
            return;
        }
        const int index = int(groups.size());
        const Clay_ElementId gid = Clay_GetElementIdWithIndex(text("DockGroup"), uint32_t(index));
        groups.push_back({&n, gid});
        std::vector<const char *> titles;
        for (const std::string &t : n.tabs) titles.push_back(t.c_str());
        n.shown = std::clamp(n.shown, 0, std::max(0, int(n.tabs.size()) - 1));
        CLAY({.id = gid, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            if (!titles.empty())
            {
                const Ui::Tabs t = ui.tabs(titles.data(), int(titles.size()), n.shown, 900 + index);
                if (t.held >= 0) hold(n.tabs[size_t(t.held)], &n, t.x, t.y, t.released);
                if (t.altKey) keyed = {&n, t.key};
            }
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(8), .childGap = 8,
                             .layoutDirection = CLAY_TOP_TO_BOTTOM},
                  .backgroundColor = clayColor(ui.theme.panel), .clip = {.vertical = true}})
            {
                if (!n.tabs.empty())
                    for (const Panel &p : panels)
                        if (n.tabs[size_t(n.shown)] == p.name && p.contents) p.contents();
            }
        }
    }

    void floating(Ui &ui, const std::vector<Panel> &panels)
    {
        for (size_t i = 0; i < layout.floating.size(); ++i)
        {
            Floating &f = layout.floating[i];
            const Panel *p = nullptr;
            for (const Panel &q : panels)
                if (f.panel == q.name) p = &q;
            bool open = true;
            Ui::Area header;
            ui.window(f.panel.c_str(), f.frame, open, [&] { if (p && p->contents) p->contents(); }, {}, 0, &header);
            if (!open) // closed: back into the dock
            {
                first(layout.root).tabs.push_back(f.panel);
                ui.announce((f.panel + " docked").c_str());
                layout.floating.erase(layout.floating.begin() + long(i));
                reshaped = true;
                return;
            }
            if (header.held) hold(f.panel, nullptr, header.x, header.y, header.released);
        }
    }

    // A tab or a floating window's header held down: past 6 points it's a drag.
    void hold(const std::string &panel, Node *from, float x, float y, bool released)
    {
        if (drag.panel != panel || drag.from != from) drag = {panel, from, x, y, x, y};
        drag.x = x, drag.y = y, drag.released = released;
        seen = true;
    }

    // Where a drag would land: the group under the pointer and the side of it.
    bool target(const Ui &ui, Group &into, Side &side, Rect &box) const
    {
        const float px = drag.x / ui.s, py = drag.y / ui.s; // Clay's boxes are in points
        for (const Group &g : groups)
        {
            const Clay_ElementData d = Clay_GetElementData(g.id);
            const Clay_BoundingBox &b = d.boundingBox;
            if (!d.found || px < b.x || py < b.y || px >= b.x + b.width || py >= b.y + b.height) continue;
            const float u = (px - b.x) / b.width, v = (py - b.y) / b.height;
            const float edge = std::min({u, 1 - u, v, 1 - v});
            side = edge > 0.25f ? Side::middle : edge == u ? Side::left : edge == 1 - u ? Side::right : edge == v ? Side::top : Side::bottom;
            into = g;
            box = {b.x, b.y, b.width, b.height};
            if (side == Side::left || side == Side::right) box.w /= 2;
            if (side == Side::right) box.x += box.w;
            if (side == Side::top || side == Side::bottom) box.h /= 2;
            if (side == Side::bottom) box.y += box.h;
            return true;
        }
        return false;
    }

    void dragging(Ui &ui)
    {
        if (!std::exchange(seen, false)) // nothing held this frame
        {
            drag = {};
            return;
        }
        if (!drag.moving && std::hypot(drag.x - drag.pressX, drag.y - drag.pressY) > 6 * ui.s) drag.moving = true;
        if (!drag.moving) return;
        Group into{};
        Side side = Side::middle;
        Rect box{};
        const bool over = target(ui, into, side, box);
        if (over && !drag.released) // where it would land
        {
            Color a = ui.theme.accent;
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(box.w), CLAY_SIZING_FIXED(box.h)}},
                  .backgroundColor = clayColor({a.r, a.g, a.b, 0.25f}),
                  .floating = {.offset = {box.x, box.y}, .zIndex = 150, .attachTo = CLAY_ATTACH_TO_ROOT},
                  .border = {.color = clayColor(a), .width = CLAY_BORDER_OUTSIDE(2)}}) {}
        }
        if (!drag.released) return;
        reshaped = true;
        if (over) move(drag.panel, drag.from, *into.node, side, box);
        else if (drag.from) // let go off the dock: it floats there
        {
            take(drag.panel, drag.from);
            layout.floating.push_back({drag.panel, {drag.x / ui.s - 40, drag.y / ui.s - 12, 280, 200}});
            prune(layout.root);
        }
        drag = {};
    }

    // Out of its group (or the floating list), not yet tidied.
    void take(const std::string &panel, Node *from)
    {
        if (from)
        {
            // It keeps showing what it showed, or the tab before the one taken.
            const auto at = std::find(from->tabs.begin(), from->tabs.end(), panel);
            if (at == from->tabs.end()) return;
            const int index = int(at - from->tabs.begin());
            if (index < from->shown || (index == from->shown && index > 0)) --from->shown;
            from->tabs.erase(at);
        }
        else std::erase_if(layout.floating, [&](const Floating &f) { return f.panel == panel; });
    }

    void move(const std::string &panel, Node *from, Node &into, Side side, Rect half)
    {
        take(panel, from);
        if (side == Side::middle)
        {
            into.tabs.push_back(panel);
            into.shown = int(into.tabs.size()) - 1;
        }
        else split(into, panel, side, side == Side::left || side == Side::right ? half.w : half.h);
        prune(layout.root);
    }

    // into becomes a split: the panel on one side, what it held on the other.
    static void split(Node &into, const std::string &panel, Side side, float size)
    {
        Node mine{{panel}}, rest = std::move(into);
        into = Node{};
        into.stacked = side == Side::top || side == Side::bottom;
        into.size = size > 0 ? size : 200;
        if (side == Side::left || side == Side::top) into.children = {std::move(mine), std::move(rest)};
        else into.children = {std::move(rest), std::move(mine)};
    }

    // An empty group gives its place to the other side of its split.
    static void prune(Node &n)
    {
        for (Node &c : n.children) prune(c);
        if (n.children.size() != 2) return;
        auto empty = [](const Node &c) { return c.children.empty() && c.tabs.empty(); };
        if (empty(n.children[0])) n = Node(std::move(n.children[1]));
        else if (empty(n.children[1])) n = Node(std::move(n.children[0]));
    }

    // Alt and an arrow: the shown panel to that side of its group (of the
    // whole dock, if it's the group's only one); Alt and Enter: it floats.
    void keyMove(Ui &ui, Node &n, View::Key key)
    {
        if (n.tabs.empty()) return;
        const std::string panel = n.tabs[size_t(n.shown)];
        const Side side = key == View::Key::left ? Side::left : key == View::Key::right ? Side::right : key == View::Key::up ? Side::top
                        : key == View::Key::down ? Side::bottom : Side::middle;
        if (key == View::Key::enter)
        {
            take(panel, &n);
            layout.floating.push_back({panel, {80, 80, 280, 200}});
            ui.announce((panel + " floating; close it to dock it again").c_str());
        }
        else if (side != Side::middle)
        {
            Node &into = n.tabs.size() > 1 ? n : layout.root;
            take(panel, &n);
            split(into, panel, side, 0);
            prune(layout.root);
            static const char *const sides[] = {"", "left", "right", "top", "bottom"};
            ui.announce((panel + " docked " + sides[int(side)]).c_str());
        }
    }

    std::deque<std::string> strings; // this frame's names, alive until it ends
    std::vector<Group> groups;       // this frame's groups, in drawing order
    Drag drag;
    bool seen = false;     // a tab or header was held this frame
    bool reshaped = false; // the tree changed this frame
    struct Keyed { Node *node = nullptr; View::Key key = View::Key::tab; } keyed; // an Alt key on a group's tabs (tab: none)
};

}
