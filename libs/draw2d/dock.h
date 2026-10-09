#pragma once
#include "ui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// Dockable windows, shown as macOS shows a tile. Each panel is a window of
// its own until it's docked: drag a window by its title over a dock space,
// and an outline shows where it would go: near a group's edge, beside it on
// that side; in its middle, among its tabs (a ghost tab shows); near the dock
// space's own edge, along that side of all of it. Let go to dock it there;
// anywhere else, it stays a window where it's let go. With
// Options::dockIntoWindows, windows dock into each other too, as ImGui's.
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

    // How it behaves; defaults as ImGui's and macOS's. The look follows the
    // Ui's theme (the outline is its accent).
    struct Options
    {
        bool dockIntoWindows = false; // windows dock into each other too (ImGui's way); else only into a dock space
        bool popOut = false;         // a button at the end of each tab bar that pops the shown panel out (or make your own: popOut())
        bool closeDocks = true;      // a lone panel's window has a close button that docks it into the dock space
        float pullOut = 8;           // points past its tab bar that pull a tab out into a window
        float edge = 80;             // points from a group's edge (at most 30% of it) that dock beside it
        float spaceEdge = 24;        // points from the dock space's own edge that dock along all of it
        float spaceShare = 1.0f / 3; // what a panel docked along the dock space's edge takes of it
        Rect window{40, 60, 300, 220}; // a new window's place and size, points (new ones cascade from it)
    } options;
    // A panel into a window of its own, or back into the dock space: for a
    // button or menu item of your own. Done once the frame's drawn.
    void popOut(const std::string &panel)
    {
        later = [this, panel](Ui &ui) {
            if (Node *n = find(layout.space, panel)) return keyMove(ui, *n, -1, View::Key::enter);
            for (size_t w = 0; w < layout.windows.size(); ++w)
                if (Node *n = find(layout.windows[w].root, panel))
                {
                    const bool alone = n == &layout.windows[w].root && n->tabs.size() == 1; // its own window already
                    if (!alone) keyMove(ui, *n, int(w), View::Key::enter);
                    return;
                }
        };
    }
    void dockBack(const std::string &panel)
    {
        later = [this, panel](Ui &ui) {
            for (Window &w : layout.windows)
                if (Node *n = find(w.root, panel))
                {
                    take(panel, n);
                    first(layout.space).tabs.push_back(panel);
                    tidy();
                    ui.announce((panel + " docked").c_str());
                    return;
                }
        };
    }

    // Called after the user rearranges it (a drop, a key, a pop-out, a close):
    // the time to save layout with the plugin's state.
    std::function<void()> changed;

    // A dock space: fills the box it's declared in. Optional; call it where
    // it goes on the page, before windows().
    void space(Ui &ui, const char *name, const std::vector<Panel> &panels)
    {
        begin();
        ui.pushId(this); // its ids are its own: they can't clash with the page's
        spaceId = ui.id(name);
        hasSpace = true;
        CLAY({.id = spaceId, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}}) { tree(ui, layout.space, panels, -1); }
        ui.popId();
    }

    // The windows, over everything: call it once a frame, at the top level
    // of the layout (where Ui::window may be called).
    void windows(Ui &ui, const std::vector<Panel> &panels)
    {
        begin();
        ui.pushId(this);
        adopt(panels);
        for (size_t i = 0; i < layout.windows.size(); ++i) window(ui, i, panels);
        if (!std::exchange(held, false) && (drag.mode == Mode::tab || drag.mode == Mode::bar)) drag = {}; // let go elsewhere
        moving(ui);
        // Changes wait until everything's drawn: each panel is drawn once a
        // frame, where it was.
        if (later) later(ui);
        later = nullptr;
        drawn = false, hasSpace = false;
        ui.popId();
    }

private:
    enum class Side { middle, left, right, top, bottom };
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
        Rect region{};  // what it goes beside, points: its split shares this
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
        strings.clear(), groups.clear(), splits = 0;
    }

    void adopt(const std::vector<Panel> &panels)
    {
        for (const Panel &p : panels)
        {
            bool known = find(layout.space, p.name) != nullptr;
            for (Window &w : layout.windows) known |= find(w.root, p.name) != nullptr;
            if (known) continue;
            const float step = 24 * float(layout.windows.size() % 8); // cascaded, as new windows are
            const Rect &w = options.window;
            layout.windows.push_back({Node{{p.name}}, {w.x + step, w.y + step, w.w, w.h}});
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

    void tree(Ui &ui, Node &n, const std::vector<Panel> &panels, int window)
    {
        if (n.children.size() == 2)
        {
            if (n.size <= 0) n.size = 160;
            const char *label = strings.emplace_back(names(n.children[0]) + " and " + names(n.children[1])).c_str();
            ui.split(label, n.size, [&] { tree(ui, n.children[0], panels, window); }, [&] { tree(ui, n.children[1], panels, window); },
                     {.stacked = n.stacked}, splits++);
            return;
        }
        const int index = int(groups.size());
        const Clay_ElementId gid = ui.id("DockGroup", index);
        groups.push_back({&n, ui.bounds(gid), window});
        CLAY({.id = gid, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            group(ui, n, panels, window, index);
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
            Ui::Tabs t;
            // The tabs, then a button that pops the shown one out into a window.
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM}}})
            {
                t = ui.tabs(titles.data(), int(titles.size()), n.shown, index);
                if (options.popOut) popOut(ui, n, window, index);
            }
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

    // A small button at the end of a tab bar: the shown panel into a window
    // of its own (as Alt and Enter do).
    void popOut(Ui &ui, Node &n, int window, int index)
    {
        const Clay_ElementId id = ui.id("DockPopOut", index);
        const Ui::Area a = ui.element(id, Ui::eachPress);
        const bool hot = ui.pointerOver(id);
        const std::string name = "Pop out " + n.tabs[size_t(n.shown)];
        const Theme theme = ui.theme;
        CLAY({.id = id, .layout = {.sizing = {CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(theme.textSize + 10)}},
              .backgroundColor = clayColor(hot ? theme.panel : Color{0, 0, 0, 0}), .cornerRadius = CLAY_CORNER_RADIUS(theme.radius),
              .custom = {.customData = ui.paint([&ui, theme, hot, name](Rect r) {
                  // A window with an arrow out of its corner.
                  const float s = ui.s, cx = r.x + r.w / 2, cy = r.y + r.h / 2, h = 5 * s;
                  const Color ink = hot ? theme.text : theme.dim;
                  ui.draw.rect(cx - h, cy - h + 2 * s, 2 * h - 2 * s, 2 * h - 2 * s, 1.5f * s, {0, 0, 0, 0}, 1.2f * s, ink);
                  ui.draw.line(cx - 1 * s, cy + 1 * s, cx + h + 1 * s, cy - h - 1 * s, 1.4f * s, ink);
                  const float tip[] = {cx + h - 3 * s, cy - h - 1 * s, cx + h + 1 * s, cy - h - 1 * s, cx + h + 1 * s, cy - h + 3 * s};
                  ui.draw.polyline(tip, 3, 1.4f * s, ink);
                  ui.accessible(Accessible::Role::button, name.c_str(), nullptr, r, "Puts the shown panel in a window of its own");
              })}}) {}
        if (a.clicked)
        {
            Node *at = &n;
            later = [this, at, window](Ui &u) { keyMove(u, *at, window, View::Key::enter); };
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
            if (changed) changed();
        }
        if (t.released)
        {
            drag = {};
            return;
        }
        // Out of the bar (8 points above or below it, or past an end): a window, carried.
        const float slack = options.pullOut * ui.s;
        const bool out = t.y < t.bar.y - slack || t.y > t.bar.y + t.bar.h + slack || t.x < t.bar.x - slack || t.x > t.bar.x + t.bar.w + slack;
        if (t.bar.h > 0 && out)
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
            ui.window(name.c_str(), w.frame, open, [&] { contents(name, panels); }, {.closable = hasSpace && options.closeDocks}, 0, &title);
            // Its box, for targets: the window itself.
            groups.push_back({&w.root, w.frame, int(i)});
        }
        else
            ui.window(strings.emplace_back(names(w.root)).c_str(), w.frame, open, [&] { tree(ui, w.root, panels, int(i)); },
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
        else if (changed) changed(); // it moved
        drag = {}, outlineAlpha = 0;
    }

    // Where it would go, by where the pointer is over the group under it (in
    // a window, or the dock space): near an edge, beside the group on that
    // side; in its middle, among its tabs; near the dock space's own edge,
    // along that side of all of it. Shown as macOS shows a tile: an outline
    // gliding to where it would go, with its name, and a ghost tab for a tab.
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
        // The nearest edge, and how near (points).
        auto nearest = [](const Rect &r, float x, float y, float &distance) {
            const float d[] = {x - r.x, r.x + r.w - x, y - r.y, r.y + r.h - y};
            const Side sides[] = {Side::left, Side::right, Side::top, Side::bottom};
            int best = 0;
            for (int i = 1; i < 4; ++i)
                if (d[i] < d[best]) best = i;
            distance = d[best];
            return sides[best];
        };
        // What's under it: a window's group (the later windows are on top), else the dock space's.
        const Group *under = nullptr;
        bool overWindow = false;
        for (int w = int(layout.windows.size()) - 1; w >= 0 && !overWindow; --w)
        {
            if (w == moved || !inside(layout.windows[size_t(w)].frame, px, py)) continue;
            overWindow = true; // a window on top: the space under it isn't where it goes
            if (options.dockIntoWindows)
                for (const Group &g : groups)
                    if (g.window == w && inside(g.box, px, py)) under = &g;
        }
        for (const Group &g : groups)
            if (!overWindow && !under && g.window < 0 && inside(g.box, px, py)) under = &g;
        bool found = false;
        const Rect whole = hasSpace ? ui.bounds(spaceId) : Rect{};
        float distance = 0;
        if (!overWindow && whole.w > 0 && inside(whole, px, py))
            if (const Side side = nearest(whole, px, py, distance); distance < options.spaceEdge) // the space's own edge
                target = {nullptr, side, part(whole, side, options.spaceShare), whole}, found = true;
        if (!found && under && under->box.w > 0)
        {
            const Rect &u = under->box;
            const Side side = nearest(u, px, py, distance);
            const float band = std::min(options.edge, 0.3f * (side == Side::left || side == Side::right ? u.w : u.h));
            target = distance < band ? Target{under->node, side, part(u, side, 0.5f), u} : Target{under->node, Side::middle, u, u};
            found = true;
        }
        // The outline glides to where it would go, and fades when there's nowhere.
        const float dt = std::min(0.05f, std::chrono::duration<float>(std::chrono::steady_clock::now() - glided).count());
        glided = std::chrono::steady_clock::now();
        const float k = 1 - std::exp(-dt * 22);
        if (found)
        {
            if (outlineAlpha <= 0.01f) outline = {px - 20, py - 14, 40, 28}; // grows from the pointer
            outline.x += (target.preview.x - outline.x) * k, outline.y += (target.preview.y - outline.y) * k;
            outline.w += (target.preview.w - outline.w) * k, outline.h += (target.preview.h - outline.h) * k;
        }
        outlineAlpha += ((found ? 1.0f : 0.0f) - outlineAlpha) * k;
        if (outlineAlpha < 0.01f) return found;
        ui.animate();
        const Theme theme = ui.theme;
        const Rect r = outline;
        const float alpha = outlineAlpha;
        const bool asTab = found && target.side == Side::middle;
        const std::string label = names(layout.windows[size_t(moved)].root);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
              .floating = {.zIndex = 150, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH, .attachTo = CLAY_ATTACH_TO_ROOT},
              .custom = {.customData = ui.paint([&ui, theme, r, alpha, asTab, label](Rect) {
                  const float s = ui.s, inset = 3;
                  const Color a = theme.accent;
                  const Rect o{(r.x + inset) * s, (r.y + inset) * s, (r.w - 2 * inset) * s, (r.h - 2 * inset) * s};
                  ui.draw.rect(o.x, o.y, o.w, o.h, 8 * s, {a.r, a.g, a.b, 0.18f * alpha}, 2 * s, {a.r, a.g, a.b, alpha});
                  const float size = theme.textSize * s;
                  const Color ink{theme.text.r, theme.text.g, theme.text.b, alpha};
                  if (asTab) // a ghost tab, where its tab would go
                  {
                      const float w = ui.draw.measure(size, label, theme.labelFont) + 20 * s, h = size + 10 * s;
                      ui.draw.rect(o.x + 8 * s, o.y + 6 * s, w, h, 4 * s, {a.r, a.g, a.b, 0.5f * alpha}, 1 * s, {a.r, a.g, a.b, alpha});
                      ui.draw.text(o.x + 18 * s, o.y + 6 * s + h * 0.7f, size, label, ink, Draw2D::Align::left, theme.labelFont);
                  }
                  else
                      ui.draw.text(o.x + o.w / 2, o.y + o.h / 2 + size * 0.35f, size, label, ink, Draw2D::Align::center, theme.labelFont);
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
        if (changed) changed();
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
        // The split keeps its first side's size: the new one's, or (on the
        // right or below) the rest of the room, less the divider.
        const bool across = t.side == Side::left || t.side == Side::right;
        float size = across ? t.preview.w : t.preview.h;
        if (t.side == Side::right || t.side == Side::bottom) size = (across ? t.region.w : t.region.h) - size - 7;
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
            layout.windows.push_back({Node{{panel}}, options.window});
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
    int splits = 0; // this frame's, for their ids
    bool hasSpace = false, drawn = false, held = false;
    Drag drag;
    Rect outline{};          // where the outline is now, points: gliding to where it would go
    float outlineAlpha = 0;  // and how much it shows
    std::chrono::steady_clock::time_point glided;
    std::function<void(Ui &)> later; // a change to the layout, once the frame is drawn
};

}
