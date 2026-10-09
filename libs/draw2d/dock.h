#pragma once
#include "ui.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// A dock, as Dear ImGui's: panels in a tree of splits whose leaves are groups
// of tabs, and groups floating in windows over it.
//
// - A tab dragged along its bar moves along it; pulled out of the bar it
//   becomes a window that follows the pointer.
// - While a window moves (that one, or any floating window by its header),
//   drop targets show: a cross of five over the group under the pointer (the
//   middle joins it as a tab, a side splits it), one over a floating window
//   (join it), and one at each of the dock's edges (a side of the whole
//   dock). Where it would go shows as one is pointed at; let go there to
//   dock it, anywhere else to leave it floating.
// - Without the mouse (WCAG 2.5.7): on a focused tab list, Alt and an arrow
//   move the shown panel to that side, Alt and Enter float it; closing a
//   floating window docks its panels back.
//
// The layout is plain data, the caller's to keep and save; panels it doesn't
// mention join the first group. Built on Ui's public surface: tabs, split,
// window, paint, pointerAt.
namespace gpu {

class Dock
{
public:
    struct Panel
    {
        const char *name; // unique; also its tab's and window's title
        std::function<void()> contents;
    };
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
        Node group; // a group of tabs
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
        strings.clear(), groups.clear();
        adopt(panels);
        dockId = Clay_GetElementIdWithIndex(text(std::string(name) + " dock"), 0);
        CLAY({.id = dockId, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) { tree(ui, layout.root, panels, 0); }
        for (size_t i = 0; i < layout.floating.size(); ++i) window(ui, i, panels);
        moving(ui);
        // Changes to the tree wait until it's all drawn: each panel is drawn
        // once a frame, where it was.
        if (later) later(ui);
        later = nullptr;
    }

private:
    enum class Side { middle, left, right, top, bottom };
    static constexpr float button = 24, gap = 4; // the drop targets, points
    struct Group
    {
        Node *node;
        Rect box; // points, the last frame's
    };
    struct Target
    {
        Node *node = nullptr; // a group in the tree, or
        int floating = -1;    // a floating window, or neither: the whole dock
        Side side = Side::middle;
        Rect preview{};       // where it would go, points
    };
    // What the pointer is doing: a tab held (moving along its bar), or a
    // window moving (pulled out of a bar and carried, or by its header).
    enum class Mode { none, tab, carried, header };
    struct Drag
    {
        Mode mode = Mode::none;
        std::string panel;    // tab: the tab; carried or header: the window's group's first panel
        Node *from = nullptr; // tab: its group
        float grabX = 0, grabY = 0; // carried: where in the window it's held, points
    };

    Clay_String text(std::string s) { return {false, int32_t(strings.emplace_back(std::move(s)).size()), strings.back().c_str()}; }

    void adopt(const std::vector<Panel> &panels)
    {
        for (const Panel &p : panels)
        {
            bool known = find(layout.root, p.name) != nullptr;
            for (Floating &f : layout.floating) known |= find(f.group, p.name) != nullptr;
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
    static Rect box(Clay_ElementId id)
    {
        const Clay_ElementData d = Clay_GetElementData(id);
        return d.found ? Rect{d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height} : Rect{};
    }

    void tree(Ui &ui, Node &n, const std::vector<Panel> &panels, int depth)
    {
        if (n.children.size() == 2)
        {
            if (n.size <= 0) n.size = 200;
            const char *label = strings.emplace_back(names(n.children[0]) + " and " + names(n.children[1])).c_str();
            ui.split(label, n.size, [&] { tree(ui, n.children[0], panels, depth + 1); }, [&] { tree(ui, n.children[1], panels, depth + 1); },
                     {.stacked = n.stacked}, int(groups.size()) * 16 + depth);
            return;
        }
        const int index = int(groups.size());
        const Clay_ElementId gid = Clay_GetElementIdWithIndex(text("DockGroup"), uint32_t(index));
        groups.push_back({&n, box(gid)});
        CLAY({.id = gid, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            group(ui, n, panels, 900 + index, true);
        }
    }

    // A group: its tab bar (a lone floating panel has its window's title
    // instead), then the shown panel.
    void group(Ui &ui, Node &n, const std::vector<Panel> &panels, int index, bool bar)
    {
        n.shown = std::clamp(n.shown, 0, std::max(0, int(n.tabs.size()) - 1));
        if (bar && !n.tabs.empty())
        {
            std::vector<const char *> titles;
            for (const std::string &t : n.tabs) titles.push_back(t.c_str());
            const Ui::Tabs t = ui.tabs(titles.data(), int(titles.size()), n.shown, index);
            if (t.held >= 0) holdTab(ui, n, t);
            if (t.altKey)
            {
                Node *at = &n;
                const View::Key key = t.key;
                later = [this, at, key](Ui &u) { keyMove(u, *at, key); };
            }
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

    // A tab held: along its bar it moves there; out of it, it's pulled into a
    // window of its own (once this frame is drawn). Ui knows a held tab by its
    // place, so after a move this follows the panel by name.
    void holdTab(Ui &ui, Node &n, const Ui::Tabs &t)
    {
        if (drag.mode == Mode::none) drag = {Mode::tab, n.tabs[size_t(t.held)], &n};
        if (drag.mode != Mode::tab || drag.from != &n) return;
        const auto at = std::find(n.tabs.begin(), n.tabs.end(), drag.panel);
        if (at == n.tabs.end()) return;
        const int from = int(at - n.tabs.begin());
        if (t.over >= 0 && t.over != from) // along the bar: it takes that place
        {
            std::string moved = std::move(n.tabs[size_t(from)]);
            n.tabs.erase(n.tabs.begin() + from);
            n.tabs.insert(n.tabs.begin() + t.over, std::move(moved));
            n.shown = t.over;
        }
        if (t.released)
        {
            drag = {};
            return;
        }
        const float off = std::max(t.bar.y - t.y, t.y - (t.bar.y + t.bar.h));
        if (t.bar.h > 0 && off > t.bar.h) // out of the bar: a window, carried
        {
            const std::string panel = drag.panel;
            Node *source = &n;
            const float x = t.x / ui.s, y = t.y / ui.s;
            const Rect size = groupBox(source);
            later = [this, panel, source, x, y, size](Ui &) {
                take(panel, source);
                const float w = std::clamp(size.w, 220.0f, 480.0f), h = std::clamp(size.h, 140.0f, 360.0f);
                layout.floating.push_back({Node{{panel}}, {x - 40, y - 12, w, h}});
                tidy();
                drag = {Mode::carried, panel, nullptr, 40, 12};
            };
        }
    }

    Rect groupBox(const Node *n) const
    {
        for (const Group &g : groups)
            if (g.node == n && g.box.w > 0) return g.box;
        return {0, 0, 280, 200};
    }

    void window(Ui &ui, size_t i, const std::vector<Panel> &panels)
    {
        Floating &f = layout.floating[i];
        if (f.group.tabs.empty()) return;
        f.group.shown = std::clamp(f.group.shown, 0, int(f.group.tabs.size()) - 1);
        bool open = true;
        Ui::Area header;
        ui.window(f.group.tabs[size_t(f.group.shown)].c_str(), f.frame, open,
                  [&] { group(ui, f.group, panels, 950 + int(i), f.group.tabs.size() > 1); }, {}, 0, &header);
        if (!open) // closed: its panels go back into the dock
        {
            const std::vector<std::string> tabs = f.group.tabs;
            later = [this, tabs](Ui &u) {
                for (const std::string &t : tabs) first(layout.root).tabs.push_back(t);
                std::erase_if(layout.floating, [&](const Floating &g) { return g.group.tabs == tabs; });
                u.announce((tabs.front() + " docked").c_str());
            };
            return;
        }
        if (header.held && drag.mode == Mode::none) drag = {Mode::header, f.group.tabs.front()};
        if (drag.mode == Mode::header && drag.panel == f.group.tabs.front() && !header.held) drag = {};
    }

    int floatingWith(const std::string &panel) const
    {
        for (size_t i = 0; i < layout.floating.size(); ++i)
            if (!layout.floating[i].group.tabs.empty() && layout.floating[i].group.tabs.front() == panel) return int(i);
        return -1;
    }

    // A window moving: it follows the pointer if carried; the targets show;
    // let go over one, it docks there.
    void moving(Ui &ui)
    {
        if (drag.mode != Mode::carried && drag.mode != Mode::header) return;
        const int moved = floatingWith(drag.panel);
        if (moved < 0)
        {
            drag = {};
            return;
        }
        const Ui::PointerAt p = ui.pointerAt();
        const float px = p.x / ui.s, py = p.y / ui.s;
        if (drag.mode == Mode::carried)
        {
            Rect &frame = layout.floating[size_t(moved)].frame;
            frame.x = px - drag.grabX, frame.y = py - drag.grabY;
        }
        Target target;
        const bool over = targets(ui, moved, px, py, target);
        if (p.down) return;
        if (over) later = [this, moved, target](Ui &u) { dockFloating(u, size_t(moved), target); };
        drag = {};
    }

    // The targets for what's under the pointer, and the dock's edges: drawn,
    // and the one pointed at (if any) in target.
    bool targets(Ui &ui, int moved, float px, float py, Target &target)
    {
        auto inside = [](const Rect &r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; };
        Rect under{};
        Target base;
        for (int i = int(layout.floating.size()) - 1; i >= 0 && under.w == 0; --i) // windows first: they're on top
            if (i != moved && inside(layout.floating[size_t(i)].frame, px, py)) under = layout.floating[size_t(i)].frame, base.floating = i;
        for (const Group &g : groups)
            if (under.w == 0 && inside(g.box, px, py)) under = g.box, base.node = g.node;
        auto part = [](Rect r, Side side, float share) {
            if (side == Side::left) r.w *= share;
            if (side == Side::right) r.x += r.w * (1 - share), r.w *= share;
            if (side == Side::top) r.h *= share;
            if (side == Side::bottom) r.y += r.h * (1 - share), r.h *= share;
            return r;
        };
        struct Button { Rect r; Target t; };
        std::vector<Button> buttons;
        if (under.w > 0) // the cross: the middle, then the sides (a window takes tabs only)
        {
            const float cx = under.x + under.w / 2 - button / 2, cy = under.y + under.h / 2 - button / 2, d = button + gap;
            const Side sides[] = {Side::middle, Side::left, Side::right, Side::top, Side::bottom};
            const float dx[] = {0, -d, d, 0, 0}, dy[] = {0, 0, 0, -d, d};
            for (int i = 0; i < (base.floating >= 0 ? 1 : 5); ++i)
            {
                Target t = base;
                t.side = sides[i], t.preview = part(under, sides[i], 0.5f);
                buttons.push_back({{cx + dx[i], cy + dy[i], button, button}, t});
            }
        }
        const Rect whole = box(dockId);
        if (whole.w > 0 && inside(whole, px, py)) // the dock's edges: a side of all of it
            for (Side side : {Side::left, Side::right, Side::top, Side::bottom})
            {
                const float x = side == Side::left ? whole.x + gap : side == Side::right ? whole.x + whole.w - gap - button
                                                                                          : whole.x + whole.w / 2 - button / 2;
                const float y = side == Side::top ? whole.y + gap : side == Side::bottom ? whole.y + whole.h - gap - button
                                                                                          : whole.y + whole.h / 2 - button / 2;
                Target t;
                t.side = side, t.preview = part(whole, side, 1.0f / 3);
                buttons.push_back({{x, y, button, button}, t});
            }
        bool found = false;
        for (const Button &b : buttons)
            if (inside(b.r, px, py)) target = b.t, found = true;
        // Over everything: where it would go, then the buttons, each filled
        // on the side it means.
        const Theme theme = ui.theme;
        const Target shown = target;
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
              .floating = {.zIndex = 150, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH, .attachTo = CLAY_ATTACH_TO_ROOT},
              .custom = {.customData = ui.paint([&ui, theme, buttons, shown, found](Rect) {
                  const float s = ui.s;
                  const Color a = theme.accent;
                  if (found)
                      ui.draw.rect(shown.preview.x * s, shown.preview.y * s, shown.preview.w * s, shown.preview.h * s, 2 * s,
                                   {a.r, a.g, a.b, 0.25f}, 2 * s, a);
                  for (const Button &b : buttons)
                  {
                      const bool hot = found && b.t.side == shown.side && b.t.node == shown.node && b.t.floating == shown.floating;
                      const Rect r{b.r.x * s, b.r.y * s, b.r.w * s, b.r.h * s};
                      ui.draw.rect(r.x, r.y, r.w, r.h, 3 * s, hot ? a : theme.panel, 1 * s, theme.text);
                      Rect mark{r.x + 4 * s, r.y + 4 * s, r.w - 8 * s, r.h - 8 * s}; // the part it fills
                      if (b.t.side == Side::left || b.t.side == Side::right) mark.w /= 2;
                      if (b.t.side == Side::right) mark.x += mark.w;
                      if (b.t.side == Side::top || b.t.side == Side::bottom) mark.h /= 2;
                      if (b.t.side == Side::bottom) mark.y += mark.h;
                      ui.draw.rect(mark.x, mark.y, mark.w, mark.h, 1 * s, hot ? theme.background : theme.dim);
                  }
              })}}) {}
        return found;
    }

    // Out of its group, which may then be empty: tidy() after.
    static void take(const std::string &panel, Node *from)
    {
        if (!from) return;
        // It keeps showing what it showed, or the tab before the one taken.
        const auto at = std::find(from->tabs.begin(), from->tabs.end(), panel);
        if (at == from->tabs.end()) return;
        const int index = int(at - from->tabs.begin());
        if (index < from->shown || (index == from->shown && index > 0)) --from->shown;
        from->tabs.erase(at);
    }

    // Empty groups give their place to the other side of their split; empty
    // windows go.
    void tidy()
    {
        prune(layout.root);
        std::erase_if(layout.floating, [](const Floating &f) { return f.group.tabs.empty(); });
    }
    static void prune(Node &n)
    {
        for (Node &c : n.children) prune(c);
        if (n.children.size() != 2) return;
        auto empty = [](const Node &c) { return c.children.empty() && c.tabs.empty(); };
        if (empty(n.children[0])) n = Node(std::move(n.children[1]));
        else if (empty(n.children[1])) n = Node(std::move(n.children[0]));
    }

    // into becomes a split: the group on one side, what it held on the other.
    static void split(Node &into, Node group, Side side, float size)
    {
        Node rest = std::move(into);
        into = Node{};
        into.stacked = side == Side::top || side == Side::bottom;
        into.size = size > 0 ? size : 200;
        if (side == Side::left || side == Side::top) into.children = {std::move(group), std::move(rest)};
        else into.children = {std::move(rest), std::move(group)};
    }

    void dockFloating(Ui &ui, size_t index, const Target &t)
    {
        if (index >= layout.floating.size()) return;
        Node group = std::move(layout.floating[index].group);
        const std::string name = group.tabs.empty() ? "" : group.tabs[size_t(group.shown)];
        layout.floating.erase(layout.floating.begin() + long(index));
        const float size = t.side == Side::left || t.side == Side::right ? t.preview.w : t.preview.h;
        auto join = [&](Node &g) {
            g.shown = int(g.tabs.size()) + group.shown;
            g.tabs.insert(g.tabs.end(), group.tabs.begin(), group.tabs.end());
        };
        if (t.floating >= 0) join(layout.floating[size_t(t.floating) - (size_t(t.floating) > index ? 1 : 0)].group); // a window's tabs
        else if (!t.node) split(layout.root, std::move(group), t.side, size); // a side of the whole dock
        else if (t.side == Side::middle) join(*t.node);
        else split(*t.node, std::move(group), t.side, size);
        tidy();
        ui.announce((name + " docked").c_str());
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
            layout.floating.push_back({Node{{panel}}, {80, 80, 280, 200}});
            ui.announce((panel + " floating; close it to dock it again").c_str());
        }
        else if (side != Side::middle)
        {
            Node &into = n.tabs.size() > 1 ? n : layout.root;
            take(panel, &n);
            split(into, Node{{panel}}, side, 0);
            static const char *const sides[] = {"", "left", "right", "top", "bottom"};
            ui.announce((panel + " docked " + sides[int(side)]).c_str());
        }
        tidy();
    }

    std::deque<std::string> strings; // this frame's names, alive until it ends
    std::vector<Group> groups;       // this frame's groups in the tree, in drawing order
    Clay_ElementId dockId{};
    Drag drag;
    std::function<void(Ui &)> later; // a change to the tree, once the frame is drawn
};

}
