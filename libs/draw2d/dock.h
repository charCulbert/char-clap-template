#pragma once
#include "ui.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// Dockable windows, as Dear ImGui's. Each panel is a window of its own until
// it's docked: drag a window by its title, and targets show over the window
// or dock space under the pointer. Let go on one: the middle makes them one
// window of tabs, a side splits it, a dock space's edges put it along that
// side of the whole space. Let go anywhere else and it just moves.
//
// - A window of several panels has no title: its tab bars are. Drag a bar's
//   empty part to move the window; drag a tab along the bar to move it there,
//   or out of the bar to pull its panel into a window of its own.
// - A dock space (optional) is part of the page panels can be docked into.
// - Without the mouse (WCAG 2.5.7): on a focused tab list, Alt and an arrow
//   move the shown panel to that side of its group, Alt and Enter pull it into
//   a window of its own; a lone panel's window moves with its title's keys,
//   and closing it docks it into the dock space, if there is one.
//
// The layout is plain data, the caller's to keep and save; a panel it doesn't
// mention starts in a window of its own. Built on Ui's public surface: tabs,
// split, window, paint, pointerAt.
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
    struct Window
    {
        Node root;
        Rect frame; // points
    };
    struct Layout
    {
        Node space; // what's docked in the dock space
        std::vector<Window> windows;
    } layout;

    // A dock space: fills the box it's declared in. Optional; call it where
    // it goes on the page, before windows().
    void space(Ui &ui, const char *name, const std::vector<Panel> &panels)
    {
        begin();
        spaceId = Clay_GetElementIdWithIndex(text(std::string(name) + " space"), 0);
        hasSpace = true;
        CLAY({.id = spaceId, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) { tree(ui, layout.space, panels, -1, 0); }
    }

    // The windows, over everything: call it once a frame, at the top level
    // of the layout (where Ui::window may be called).
    void windows(Ui &ui, const std::vector<Panel> &panels)
    {
        begin();
        adopt(panels);
        for (size_t i = 0; i < layout.windows.size(); ++i) window(ui, i, panels);
        if (!std::exchange(held, false) && (drag.mode == Mode::tab || drag.mode == Mode::bar)) drag = {}; // let go elsewhere
        moving(ui);
        // Changes wait until everything's drawn: each panel is drawn once a
        // frame, where it was.
        if (later) later(ui);
        later = nullptr;
        drawn = false, hasSpace = false;
    }

private:
    enum class Side { middle, left, right, top, bottom };
    static constexpr float button = 24, gap = 4; // the drop targets, points
    struct Group
    {
        Node *node;
        Rect box;   // points, the last frame's
        int window; // its window, or -1 in the dock space
    };
    struct Target
    {
        Node *node = nullptr; // a group, or (none) a side of the dock space
        Side side = Side::middle;
        Rect preview{}; // where it would go, points
    };
    // What the pointer is doing: a tab held (moving along its bar), or a
    // window moving (by its title, by a tab bar, or pulled out and carried).
    enum class Mode { none, tab, title, bar, carried };
    struct Drag
    {
        Mode mode = Mode::none;
        std::string panel; // tab: the tab; else a panel in the moving window, which names it
        Node *from = nullptr; // tab: its group
        float grabX = 0, grabY = 0; // bar or carried: where in the window it's held, points
    };

    void begin()
    {
        if (drawn) return;
        drawn = true;
        strings.clear(), groups.clear();
    }
    Clay_String text(std::string s) { return {false, int32_t(strings.emplace_back(std::move(s)).size()), strings.back().c_str()}; }

    void adopt(const std::vector<Panel> &panels)
    {
        for (const Panel &p : panels)
        {
            bool known = find(layout.space, p.name) != nullptr;
            for (Window &w : layout.windows) known |= find(w.root, p.name) != nullptr;
            if (known) continue;
            const float step = 24 * float(layout.windows.size() % 8); // cascaded, as new windows are
            layout.windows.push_back({Node{{p.name}}, {40 + step, 60 + step, 300, 220}});
        }
    }
    static Node *find(Node &n, const std::string &panel)
    {
        if (std::find(n.tabs.begin(), n.tabs.end(), panel) != n.tabs.end()) return &n;
        for (Node &c : n.children)
            if (Node *f = find(c, panel)) return f;
        return nullptr;
    }
    int windowWith(const std::string &panel)
    {
        for (size_t i = 0; i < layout.windows.size(); ++i)
            if (find(layout.windows[i].root, panel)) return int(i);
        return -1;
    }
    static Node &first(Node &n) { return n.children.empty() ? n : first(n.children[0]); }
    static void panelsOf(const Node &n, std::vector<std::string> &out)
    {
        out.insert(out.end(), n.tabs.begin(), n.tabs.end());
        for (const Node &c : n.children) panelsOf(c, out);
    }
    static std::string names(const Node &n)
    {
        std::vector<std::string> all;
        panelsOf(n, all);
        std::string joined;
        for (const std::string &p : all) joined += (joined.empty() ? "" : ", ") + p;
        return joined;
    }
    static Rect box(Clay_ElementId id)
    {
        const Clay_ElementData d = Clay_GetElementData(id);
        return d.found ? Rect{d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height} : Rect{};
    }

    void tree(Ui &ui, Node &n, const std::vector<Panel> &panels, int window, int depth)
    {
        if (n.children.size() == 2)
        {
            if (n.size <= 0) n.size = 160;
            const char *label = strings.emplace_back(names(n.children[0]) + " and " + names(n.children[1])).c_str();
            ui.split(label, n.size, [&] { tree(ui, n.children[0], panels, window, depth + 1); },
                     [&] { tree(ui, n.children[1], panels, window, depth + 1); }, {.stacked = n.stacked}, int(groups.size()) * 16 + depth);
            return;
        }
        const int index = int(groups.size());
        const Clay_ElementId gid = Clay_GetElementIdWithIndex(text("DockGroup"), uint32_t(index));
        groups.push_back({&n, box(gid), window});
        CLAY({.id = gid, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            group(ui, n, panels, window, 900 + index);
        }
    }

    // A group: its tab bar, then the shown panel.
    void group(Ui &ui, Node &n, const std::vector<Panel> &panels, int window, int index)
    {
        n.shown = std::clamp(n.shown, 0, std::max(0, int(n.tabs.size()) - 1));
        if (!n.tabs.empty())
        {
            std::vector<const char *> titles;
            for (const std::string &t : n.tabs) titles.push_back(t.c_str());
            const Ui::Tabs t = ui.tabs(titles.data(), int(titles.size()), n.shown, index);
            if (t.held >= 0) holdTab(ui, n, t);
            if (t.barHeld && window >= 0) holdBar(ui, window, n, t);
            if (t.altKey)
            {
                Node *at = &n;
                const View::Key key = t.key;
                later = [this, at, window, key](Ui &u) { keyMove(u, *at, window, key); };
            }
        }
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(8), .childGap = 8,
                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = clayColor(ui.theme.panel), .clip = {.vertical = true}})
        {
            if (!n.tabs.empty()) contents(n.tabs[size_t(n.shown)], panels);
        }
    }

    static void contents(const std::string &name, const std::vector<Panel> &panels)
    {
        for (const Panel &p : panels)
            if (name == p.name && p.contents) p.contents();
    }

    // A tab held: along its bar it moves there; out of it, it's pulled into a
    // window of its own (once this frame is drawn). Ui knows a held tab by its
    // place, so after a move this follows the panel by name.
    void holdTab(Ui &ui, Node &n, const Ui::Tabs &t)
    {
        held = true;
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
            later = [this, panel, source, x, y](Ui &) {
                take(panel, source);
                layout.windows.push_back({Node{{panel}}, {x - 60, y - 10, 300, 220}});
                tidy();
                drag = {Mode::carried, panel, nullptr, 60, 10};
            };
        }
    }

    // A tab bar's empty part held: the window moves with it.
    void holdBar(Ui &ui, int window, const Node &n, const Ui::Tabs &t)
    {
        held = true;
        Rect &frame = layout.windows[size_t(window)].frame;
        if (drag.mode == Mode::none) drag = {Mode::bar, n.tabs.front(), nullptr, t.x / ui.s - frame.x, t.y / ui.s - frame.y};
    }

    void window(Ui &ui, size_t i, const std::vector<Panel> &panels)
    {
        Window &w = layout.windows[i];
        const bool lone = w.root.children.empty() && w.root.tabs.size() == 1; // one panel: an ordinary window
        bool open = true;
        Ui::Area title;
        if (lone)
        {
            const std::string &name = w.root.tabs.front();
            ui.window(name.c_str(), w.frame, open, [&] { contents(name, panels); }, {.closable = hasSpace}, 0, &title);
            // Its box, for targets: the window itself.
            groups.push_back({&w.root, w.frame, int(i)});
        }
        else
            ui.window(strings.emplace_back(names(w.root)).c_str(), w.frame, open, [&] { tree(ui, w.root, panels, int(i), 0); },
                      {.minWidth = 200, .minHeight = 140, .header = false}, 0);
        if (!open) // closed: into the dock space
        {
            const std::string panel = w.root.tabs.front();
            later = [this, panel](Ui &u) {
                const int at = windowWith(panel);
                if (at < 0) return;
                take(panel, &layout.windows[size_t(at)].root);
                first(layout.space).tabs.push_back(panel);
                tidy();
                u.announce((panel + " docked").c_str());
            };
            return;
        }
        if (title.held && drag.mode == Mode::none) drag = {Mode::title, w.root.tabs.front()};
        if (drag.mode == Mode::title && lone && drag.panel == w.root.tabs.front() && !title.held) drag = {};
    }

    // A window moving: by its title (Ui moves it), or its tab bar or pulled
    // out (this moves it). The targets show; let go on one, it docks there.
    void moving(Ui &ui)
    {
        if (drag.mode != Mode::title && drag.mode != Mode::bar && drag.mode != Mode::carried) return;
        const int moved = windowWith(drag.panel);
        if (moved < 0)
        {
            drag = {};
            return;
        }
        const Ui::PointerAt p = ui.pointerAt();
        const float px = p.x / ui.s, py = p.y / ui.s;
        if (drag.mode != Mode::title)
        {
            Rect &frame = layout.windows[size_t(moved)].frame;
            frame.x = px - drag.grabX, frame.y = py - drag.grabY;
        }
        Target target;
        const bool over = targets(ui, moved, px, py, target);
        if (p.down) return;
        if (over) later = [this, moved, target](Ui &u) { dock(u, size_t(moved), target); };
        drag = {};
    }

    // The targets for the group under the pointer (in a window, or the dock
    // space), and the dock space's edges: drawn, and the one pointed at (if
    // any) in target.
    bool targets(Ui &ui, int moved, float px, float py, Target &target)
    {
        auto inside = [](const Rect &r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; };
        auto part = [](Rect r, Side side, float share) {
            if (side == Side::left) r.w *= share;
            if (side == Side::right) r.x += r.w * (1 - share), r.w *= share;
            if (side == Side::top) r.h *= share;
            if (side == Side::bottom) r.y += r.h * (1 - share), r.h *= share;
            return r;
        };
        // What's under it: a window's group (the later windows are on top), else the dock space's.
        const Group *under = nullptr;
        for (int w = int(layout.windows.size()) - 1; w >= 0 && !under; --w)
        {
            if (w == moved || !inside(layout.windows[size_t(w)].frame, px, py)) continue;
            for (const Group &g : groups)
                if (g.window == w && inside(g.box, px, py)) under = &g;
            if (!under) return false; // over a window, but not a group in it (its edge): nothing
        }
        for (const Group &g : groups)
            if (!under && g.window < 0 && inside(g.box, px, py)) under = &g;
        struct Button { Rect r; Target t; };
        std::vector<Button> buttons;
        if (under && under->box.w > 0) // the cross: the middle, then the sides
        {
            const Rect &u = under->box;
            const float cx = u.x + u.w / 2 - button / 2, cy = u.y + u.h / 2 - button / 2, d = button + gap;
            const Side sides[] = {Side::middle, Side::left, Side::right, Side::top, Side::bottom};
            const float dx[] = {0, -d, d, 0, 0}, dy[] = {0, 0, 0, -d, d};
            for (int i = 0; i < 5; ++i)
                buttons.push_back({{cx + dx[i], cy + dy[i], button, button}, {under->node, sides[i], part(u, sides[i], 0.5f)}});
        }
        const Rect whole = hasSpace ? box(spaceId) : Rect{};
        if (whole.w > 0 && inside(whole, px, py) && !(under && under->window >= 0)) // the space's edges, when it's what's under
            for (Side side : {Side::left, Side::right, Side::top, Side::bottom})
            {
                const float x = side == Side::left ? whole.x + gap : side == Side::right ? whole.x + whole.w - gap - button
                                                                                          : whole.x + whole.w / 2 - button / 2;
                const float y = side == Side::top ? whole.y + gap : side == Side::bottom ? whole.y + whole.h - gap - button
                                                                                          : whole.y + whole.h / 2 - button / 2;
                buttons.push_back({{x, y, button, button}, {nullptr, side, part(whole, side, 1.0f / 3)}});
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
                                   {a.r, a.g, a.b, 0.2f}, 2 * s, a);
                  for (const Button &b : buttons)
                  {
                      const bool hot = found && b.t.side == shown.side && b.t.node == shown.node;
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
        prune(layout.space);
        for (Window &w : layout.windows) prune(w.root);
        std::erase_if(layout.windows, [](const Window &w) { return w.root.children.empty() && w.root.tabs.empty(); });
    }
    static void prune(Node &n)
    {
        for (Node &c : n.children) prune(c);
        if (n.children.size() != 2) return;
        auto empty = [](const Node &c) { return c.children.empty() && c.tabs.empty(); };
        if (empty(n.children[0])) n = Node(std::move(n.children[1]));
        else if (empty(n.children[1])) n = Node(std::move(n.children[0]));
    }

    // into becomes a split: what's added on one side, what it held on the other.
    static void split(Node &into, Node added, Side side, float size)
    {
        Node rest = std::move(into);
        into = Node{};
        into.stacked = side == Side::top || side == Side::bottom;
        into.size = size > 0 ? size : 160;
        if (side == Side::left || side == Side::top) into.children = {std::move(added), std::move(rest)};
        else into.children = {std::move(rest), std::move(added)};
    }

    // A window's panels docked at a target. Its tree is taken first, leaving
    // the window empty (tidy removes it), so the target's place stays put.
    void dock(Ui &ui, size_t index, const Target &t)
    {
        if (index >= layout.windows.size()) return;
        Node moved = std::move(layout.windows[index].root);
        layout.windows[index].root = Node{};
        std::vector<std::string> all;
        panelsOf(moved, all);
        const float size = t.side == Side::left || t.side == Side::right ? t.preview.w : t.preview.h;
        if (!t.node) split(layout.space, std::move(moved), t.side, size); // a side of the dock space
        else if (t.side == Side::middle) // its tabs join the group's
        {
            t.node->shown = int(t.node->tabs.size());
            t.node->tabs.insert(t.node->tabs.end(), all.begin(), all.end());
        }
        else split(*t.node, std::move(moved), t.side, size);
        tidy();
        if (!all.empty()) ui.announce((all.front() + " docked").c_str());
    }

    // Alt and an arrow: the shown panel to that side of its group (of its
    // whole window or dock space, if it's the group's only one); Alt and
    // Enter: into a window of its own.
    void keyMove(Ui &ui, Node &n, int window, View::Key key)
    {
        if (n.tabs.empty()) return;
        const std::string panel = n.tabs[size_t(n.shown)];
        const Side side = key == View::Key::left ? Side::left : key == View::Key::right ? Side::right : key == View::Key::up ? Side::top
                        : key == View::Key::down ? Side::bottom : Side::middle;
        Node &whole = window >= 0 ? layout.windows[size_t(window)].root : layout.space;
        if (key == View::Key::enter)
        {
            take(panel, &n);
            layout.windows.push_back({Node{{panel}}, {80, 80, 300, 220}});
            ui.announce((panel + " in a window of its own").c_str());
        }
        else if (side != Side::middle)
        {
            Node &into = n.tabs.size() > 1 ? n : whole;
            take(panel, &n);
            split(into, Node{{panel}}, side, 0);
            static const char *const sides[] = {"", "left", "right", "top", "bottom"};
            ui.announce((panel + " docked " + sides[int(side)]).c_str());
        }
        tidy();
    }

    std::deque<std::string> strings; // this frame's names, alive until it ends
    std::vector<Group> groups;       // this frame's groups, in drawing order
    Clay_ElementId spaceId{};
    bool hasSpace = false, drawn = false, held = false;
    Drag drag;
    std::function<void(Ui &)> later; // a change to the layout, once the frame is drawn
};

}
