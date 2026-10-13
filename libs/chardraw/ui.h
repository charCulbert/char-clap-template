#pragma once
#include "draw2d.h"
#include "layout.h"
#include "../gpu/params.h"
#include "../gpu/view.h"
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

// Modelled on, where named below:
// compost https://github.com/charCulbert/compost
// char-spectrum https://github.com/charCulbert/char-spectrum

// Immediate-mode controls. Each frame: ui.begin, declare the interface as
// Clay boxes (layout.h) with controls among them, then ui.end. Forward
// View::pointer, key and text here.
//
// Controls declared in the layout (button, toggle, segmented, dropdown,
// textField, and the knob and slider overloads without a Rect) size and place
// themselves; Clay hit-tests them, so popups block what is under them. The
// Rect overloads of knob and slider draw at a rectangle you choose, e.g. in
// one of your own custom boxes.
//
// Knobs and sliders: drag (Shift for fine), scroll, double-click for the
// default, click the value to type one. The last control clicked has
// keyboard focus, and Tab moves it on; past the last control (Shift+Tab:
// before the first) Tab is the host's, which moves on to what's next
// outside the view. A focused knob or slider steps with the arrows (Shift
// for fine), Page Up/Down and Home/End, and Delete or Backspace reset it;
// Escape is left to close menus and drawers. Enter or a digit types a
// value, and Enter or Escape after typing (Escape abandoning it) leaves it
// focused. Enter presses a button or toggle; the arrows
// move a segmented control (wrapping); Enter opens and closes a dropdown
// and the arrows choose in it. A control given a contextMenu opens it on a
// secondary click, the Menu key or Shift+F10.
namespace gpu
{

// The default look follows Slide: flat greys, hairlines, one warm accent, in
// a dark and a light version (Theme::dark(), Theme::light()).
struct Theme
{
    Color background { 0.184f, 0.184f, 0.184f }; // #2f2f2f
    Color panel { 0.227f, 0.227f, 0.227f };      // #3a3a3a: raised areas
    Color text { 0.871f, 0.871f, 0.871f };       // #dedede
    Color dim { 0.647f, 0.647f, 0.647f };        // #a5a5a5: labels, 4.5:1 on the panel
    Color edge { 0.333f, 0.333f, 0.333f };       // #555555: hairlines, outlines
    Color track { 0.333f, 0.333f, 0.333f };      // unfilled parts of knobs and sliders
    Color accent { 0.867f, 0.753f, 0.494f };     // #ddc07e
    Color shadow { 0, 0, 0, 0.45f };             // under menus and tooltips
    Color whiteKey { 0.929f, 0.918f, 0.886f };   // #edeae2: a piano's keys, as a piano's in either palette
    Color blackKey { 0.110f, 0.110f, 0.110f };   // #1c1c1c
    int labelFont = 0, valueFont = 0;            // Draw2D fonts for names and for numbers
    float textSize = 13, radius = 2;             // points: macOS's control text size

    static Theme dark()
    {
        return {};
    }
    static Theme light()
    {
        Theme t;
        t.background = { 0.804f, 0.804f, 0.804f };     // #cdcdcd
        t.panel = { 0.761f, 0.761f, 0.761f };          // #c2c2c2
        t.text = { 0.110f, 0.110f, 0.110f };           // #1c1c1c
        t.dim = { 0.310f, 0.310f, 0.310f };            // #4f4f4f
        t.edge = t.track = { 0.620f, 0.620f, 0.620f }; // #9e9e9e
        t.accent = { 0.459f, 0.329f, 0.039f };         // #75540a
        t.shadow = { 0, 0, 0, 0.22f };
        return t;
    }
    // The palette for the system's appearance, keeping fonts, sizes and key colours.
    Theme withColors (bool darkMode) const
    {
        Theme t = darkMode ? dark() : light();
        t.labelFont = labelFont, t.valueFont = valueFont, t.textSize = textSize, t.radius = radius;
        t.whiteKey = whiteKey, t.blackKey = blackKey;
        return t;
    }
};

struct TextEdit; // stb_textedit's state, in ui_text.cpp

// How a Ui::window may be sized, and whether it can close (points).
struct WindowLimits
{
    float minWidth = 140, minHeight = 90, maxWidth = 1e9f, maxHeight = 1e9f;
    bool resizable = true, closable = true;
    bool header = true; // its title bar; without one, the caller moves it (a dock, by its tab bars)
};

// Where a Ui::drawer docks, and how far it can be resized (points).
struct DrawerOptions
{
    enum class Edge
    {
        top,
        bottom,
        left,
        right
    };
    Edge edge = Edge::top;
    bool resizable = true;
    float min = 40, max = 1e9f;
    // Over what's under it (a plot), not beside it: nothing moves when it
    // opens. Its header is then a tab, 24 by 48 points, with an arrow
    // pointing the way it opens: centred on the edge of the box the drawer is
    // declared in while closed, and riding on the panel's inner edge while
    // open, the arrow turned to close it. The panel (opaque unless opacity
    // says) fills that box along the edge; what's in it scrolls (the wheel
    // turns a knob or readout in it only while that's focused), and a
    // control the keys focus is scrolled into view. The title is the tab's name to screen
    // readers and its tooltip. Escape, with focus on the tab or in the
    // panel, closes it and focuses the tab (unless something in it uses it
    // first: a text edit, a value being typed, an open menu, a drag).
    bool overlay = false;
    // An overlay panel's background opacity: under 1, what's under it shows
    // through (a plot it floats on).
    float opacity = 1;
    // An overlay's tab: its short side in points, the long side twice it.
    // Under 24 is under WCAG 2.5.8's target size: for a tab that would
    // otherwise hide too much of a plot.
    float tab = 24;
    // An overlay's panel as big as what's in it, both ways, not the box's
    // length and a size: it has no resize handle then.
    bool fit = false;
    // Where along its edge an overlay's tab (and a fitted panel) sits: the
    // start (left, or top), the centre, or the end (right, or bottom).
    enum class Align
    {
        start,
        centre,
        end
    };
    Align align = Align::centre;
    // An overlay's tab without its border, its background faint, until the
    // pointer is over it or it has focus: for a plot it sits on.
    bool quietTab = false;
};

// How a Ui::split lays out and limits its two areas (points).
struct Split
{
    bool stacked = false;                       // one above the other, not side by side
    float min = 40, max = 1e9f, secondMin = 40; // the second area's least, so it never vanishes
};

class Ui
{
public:
    Ui();
    ~Ui();
    Draw2D draw;
    Layout layout;
    Theme theme;
    std::function<void (bool)> lockPointer;       // View::lockPointer: knob and slider drags lock the pointer
    std::function<void (View::Cursor)> setCursor; // View::setCursor: a resize arrow over a divider, an I-beam over text
    float menuOpacity = 1; // dropdowns' and context menus' background: under 1, what's under them shows through
    std::function<void (float, float)> resizeTo;                     // View::resizeTo: for resizeGrip
    std::function<void (const std::string&)> copyText;               // View::copyText: for copy and cut in text fields
    std::function<void (const std::vector<std::string>&)> dragFiles; // View::dragFiles: for dragSource

    void pointer (const View::PointerEvent& event);
    bool key (View::Key key, unsigned modifiers);
    bool text (const char* utf8);
    // True while something waits to be drawn without input (a tooltip's delay).
    bool animating() const;
    // Asks for another frame after this one: for a widget that moves on its own
    // (a scrolling spectrogram), drawn at the display's rate while it asks.
    void animate()
    {
        animateAsked = true;
    }
    // True while a control is held: an edit isn't finished until it's let go.
    bool dragging() const
    {
        return active != nullptr;
    }
    bool editingText() const
    {
        return editing != 0;
    }
    View::Caret textCaret() const
    {
        return editing ? caretAt : View::Caret {};
    } // a text field's, in pixels
    // View::files: true over a dropTarget declared last frame.
    bool files (View::FileDrag type, float x, float y, const std::vector<std::string>& paths);

    // Starts a frame of controls and its layout; the canvas in pixels.
    void begin (float width, float height, float scale);
    // Lays out and draws the frame; paint() boxes draw themselves as they
    // come up in drawing order.
    void end (const wgpu::RenderPassEncoder& pass);
    // end() in two halves, for View::record and View::draw: record lays out
    // and records the frame, returning false if it looks like the last one
    // drawn; flush draws it.
    bool record();
    void flush (const wgpu::RenderPassEncoder& pass)
    {
        draw.flush (pass);
    }

    // How a parameter's knob, slider, number box or readout shows it. Its
    // value's text: a printf format of the value ("%.1f dB"; a plain string
    // converts to this), or text, a function of the value ("5 ms" or
    // "1.20 s", an item's name, a percent of a 0..1 range). label: the name
    // shown with it in place of the parameter's, e.g. a short one under a
    // small knob; screen readers still hear the parameter's name (a
    // ui.label() after the control changes that). For example
    //   ui.knob (params, ratio, { "%.2f", "Ratio" });
    //   ui.knob (params, attack, { msOrSeconds, "A" }); // std::string msOrSeconds (double)
    struct Format
    {
        Format (const char* format = "%.2f", const char* label = nullptr)
            : format (format)
            , label (label ? label : "")
        {}
        template <typename F>
            requires std::is_invocable_r_v<std::string, const F&, double>
        Format (F text, const char* label = nullptr)
            : text (std::move (text))
            , label (label ? label : "")
        {}
        const char* format = "%.2f";
        std::function<std::string (double)> text; // when set, used in place of format
        std::string label;                        // empty: the parameter's name
        std::string operator() (double value) const;
        const std::string& name (const Param& p) const
        {
            return label.empty() ? p.name : label;
        }
    };

    // A piano's note-on and note-off this frame, -1 for none. The off is of
    // the note before the on (a slide from key to key): send it first.
    struct Notes
    {
        int on = -1, off = -1;
    };
    // An envelope's breakpoint, 0..1 across and up.
    struct Point
    {
        float x, y;
    };

    // How a slider drags, as compost's: absolute (the default, as HTML's range
    // input): a press puts the value where the pointer is and the drag
    // follows it; relative: the drag moves the value by how far it goes,
    // with the pointer held still. Knobs are always relative. Either way,
    // Shift or a second press drags fine, relative, for the rest of the drag.
    enum class Drag
    {
        absolute,
        relative
    };
    Drag sliderDrag = Drag::absolute;

    // ---- In the layout: Clay sizes and places these, so declare them inside
    // CLAY({...}) blocks. Each returns true when the user changed something.
    bool button (const char* label, int index = 0);
    bool toggle (const char* label, bool& on, int index = 0);
    // One of count items, as WAI-ARIA's radio group: a click (let go on it,
    // as WCAG 2.5.2 has it) chooses one; focused, the arrows move the choice
    // at once, wrapping at the ends.
    bool segmented (const char* const* items, int count, int& selected, int index = 0);
    bool dropdown (const char* const* items, int count, int& selected, int index = 0);
    // Tabs, as ARIA's tab list: one shown at a time. A click chooses one, as
    // a segmented control's; focused, the arrows (wrapping), Home and End
    // choose at once (APG's automatic activation). Also says, for a dock
    // moving them: the tab held
    // down (and where the pointer is), and a key pressed with Alt on the
    // focused list (Alt and the arrows, Alt and Enter).
    struct Tabs
    {
        bool changed = false;
        int held = -1;         // the tab held down, or -1
        int over = -1;         // the tab under the pointer, or -1
        bool barHeld = false;  // the bar's empty part held down (a dock moves its window by it)
        float x = 0, y = 0;    // the pointer while a tab or the bar is held, pixels
        Rect bar {};           // the tab list's box, pixels (the last frame's)
        bool released = false; // let go this frame
        bool altKey = false;
        View::Key key = View::Key::enter; // with altKey
    };
    Tabs tabs (const char* const* items, int count, int& selected, int index = 0);
    // A toggle, segmented control and dropdown for a parameter, sending its
    // gesture (begin, change, end) when clicked. A toggle is on in the upper
    // half of the range; a segmented control or dropdown's item i is the
    // value min + i.
    bool toggle (Params& params, Param* p, const char* label);
    bool segmented (Params& params, Param* p, const char* const* items, int count);
    bool dropdown (Params& params, Param* p, const char* const* items, int count);
    // Edits text; returns true when the edit is committed (Enter, or a click
    // elsewhere). Escape abandons it. After Enter or Escape it keeps focus:
    // Enter edits it again, and typing goes on at its end.
    bool textField (std::string& value, float widthPoints = 120, int index = 0);
    // Takes files dropped on the element: true when some land on it, with
    // their paths in `files`. fileOver: files are being dragged over it.
    bool dropTarget (Clay_ElementId id, std::vector<std::string>& files);
    bool fileOver (Clay_ElementId id) const;
    // Drags files out of the element: once a press on it moves 4 points,
    // `make` is called for the files (so it can write them then) and the
    // presenter drags them. Natively only; elsewhere nothing happens.
    void dragSource (Clay_ElementId id, const std::function<std::vector<std::string>()>& make);
    // Shows text near the control declared just before, after a moment's hover.
    void tooltip (const char* text);
    // A context menu for the control declared just before, as the WAI-ARIA
    // menu: a secondary click on the control opens it at the pointer; the
    // Menu key or Shift+F10 while the control has focus, or a screen reader's
    // showMenu, opens it below the control with its first item highlighted.
    // Focus is in the menu: Up and Down (wrapping), Home, End and a letter
    // move the highlight, Enter or Space choose; Escape, Tab, or a press
    // outside it (which goes no further) close it, and focus goes back to the
    // control. A nullptr item is a separator. Returns the index of the item
    // chosen, the frame it's chosen, else -1.
    int contextMenu (const char* const* items, int count);
    // The same for an element of your own, such as a paint() box; for keys,
    // a control drawn inside it must have focus.
    int contextMenu (Clay_ElementId element, const char* const* items, int count);
    // An item of a menu with submenus or check marks. One with a submenu
    // shows an arrow; hovering or clicking it opens the submenu beside it
    // (on the other side, or moved in, where there's no room), and the
    // pointer can cut across the items between on its way there. Right,
    // Enter or Space open it on its first item; Left or Escape close it, back
    // on the item. Choosing an item at any depth closes the whole menu.
    struct MenuItem
    {
        const char* label = nullptr; // nullptr: a separator
        int id = -1;                 // what contextMenu returns when it's chosen
        int checked = -1;            // 1: a check mark before it, 0: room for one (a menu item check box); -1: neither
        const MenuItem* submenu = nullptr; // opens these count items beside it (its id is then unused)
        int count = 0;
    };
    // The same menus of MenuItems: they return the chosen item's id, else -1.
    int contextMenu (const MenuItem* items, int count);
    int contextMenu (Clay_ElementId element, const MenuItem* items, int count);
    // Drag up or right to raise it, Shift for fine; double-click for the
    // default. Focused, the arrows step it (Shift fine, Alt large), as a
    // knob's, Delete or Backspace reset it, and Enter or a digit types a
    // value. Escape cancels a drag.
    void numberBox (Params& params, Param* p, const Format& format, bool log = false, float widthPoints = 64);
    // A parameter as a line of text, its label then its value, for laying
    // over a plot (char-spectrum's readouts): drag up or down (Shift for
    // fine) or scroll over it to change it, double-click for the default.
    // With items (item i is the value min + i) it shows the item, and a
    // click steps to the next, wrapping. To screen readers a spin button
    // named for its parameter; focused, the arrows step it (an item at a
    // time), Page Up and Down step further, Home and End go to the ends,
    // and Delete or Backspace reset it (Escape only cancels a drag).
    // widthPoints 0: as wide as its text; else the value is right-aligned in
    // that width. label nullptr: the format's label, else the parameter's name.
    void readout (
        Params& params, Param* p, const char* label, const Format& format, bool log = false, float widthPoints = 0);
    void readout (
        Params& params, Param* p, const char* label, const char* const* items, int count, float widthPoints = 0);
    // A header that opens and closes the section under it: declare the
    // section while this returns true.
    bool drawer (const char* title, bool& open, int index = 0);
    // Two areas side by side (stacked: one above the other) with a divider
    // between them, as the WAI-ARIA window splitter: drag it, or focus it
    // and use the arrows (16 points; Shift 1), Home and End for the limits,
    // Enter to collapse the first area and restore it. size is the first
    // area's, in points, kept from min to max and leaving the second
    // secondMin. first and second declare the areas' contents. Returns true
    // when size changed.
    bool split (const char* name, float& size, const std::function<void()>& first, const std::function<void()>& second,
        Split options = {}, int index = 0);
    // A drawer docked to an edge, as compost's: its header opens and closes
    // it (a disclosure), and a handle on its inner side resizes it, as a
    // split's divider (named "Resize <title>"). size is its contents' height
    // (a top or bottom drawer) or its width (left or right), in points.
    // contents declares what's in it, while it's open.
    bool drawer (const char* title, bool& open, float& size, const std::function<void()>& contents,
        DrawerOptions options = {}, int index = 0);
    // A window over the view, as compost's (a non-modal dialog): dragged by
    // its header, resized from its corner, kept inside the view, and brought
    // to the front by a press on it. frame is its place and size, in points.
    // Its header takes Tab: the arrows move it (16 points; Shift 1), Alt and
    // the arrows resize it, Escape closes it. Screen readers see a dialog
    // named by its title. contents declares what's in it, while it's open.
    // Returns true while it's open.
    struct Area; // below
    bool window (const char* title, Rect& frame, bool& open, const std::function<void()>& contents,
        WindowLimits limits = {}, int index = 0, Area* header = nullptr);
    // Controls of their own size, in the layout.
    void knob (Params& params, Param* p, const Format& format, bool log = false, float diameter = 44);
    void slider (Params& params, Param* p, const Format& format, bool log = false);
    void vslider (Params& params, Param* p, const Format& format, bool log = false, float heightPoints = 140);
    void xyPad (Params& params, Param* x, Param* y);                  // fills the space it's given
    void meter (float level, float peak = -1, float widthPoints = 6); // 0..1, filled from the bottom
    // The envelope and steps editors, filling the space they're given.
    // Their changes show from the next frame's call (drawing happens at end()).
    bool envelope (std::vector<Point>& points);
    bool steps (std::vector<float>& values);
    // A piano keyboard filling the width, heightPoints tall. Its notes come
    // back from this call, played where it was laid out last frame.
    Notes piano (int lowest, int highest, float heightPoints = 126,
        const std::function<bool (int)>& lit = {}, // 126: as compost's
        int index = 0);
    // A filmstrip knob at the size of one frame (in points, at scale 1 a pixel each).
    void imageKnob (Params& params, Param* p, int image, int frames, bool log = false, bool across = false);

    // ---- At a rectangle you choose (pixels): for drawing inside a paint()
    // box, or anywhere else Clay doesn't place things.
    // A rotary control with its value and name below; drag up/down to change.
    // log: logarithmic mapping (min > 0).
    void knob (Params& params, Param* p, Rect r, const Format& format, bool log = false);
    // A horizontal slider, its name and value above it; drag left/right.
    void slider (Params& params, Param* p, Rect r, const Format& format, bool log = false);
    // A knob drawn from a filmstrip: frames of a Draw2D image, stacked down
    // (or across) from the minimum to the maximum. Drags like knob.
    void imageKnob (Params& params, Param* p, int image, int frames, Rect r, bool log = false, bool across = false);
    // An upright slider, its value above and name below; drag up/down.
    void vslider (Params& params, Param* p, Rect r, const Format& format, bool log = false);
    // A point dragged in two dimensions: x and y each a parameter.
    void xyPad (Params& params, Param* x, Param* y, Rect r);
    void meter (float level, float peak, Rect r);
    // Breakpoints, 0..1 across and up, sorted by x: drag a point (between its
    // neighbours; the ends keep their x), double-click empty space to add
    // one, double-click a point to remove it. Returns true on any change.
    // Keys, with it focused: Left and Right choose a point, Home and End the
    // first and last; Up and Down move it (Shift finer, Page keys coarser),
    // Alt with Left or Right sideways; Enter adds one after it, Delete or
    // Backspace removes it. For screen readers a group of one slider per
    // point (its level; its time is in its value).
    bool envelope (std::vector<Point>& points, Rect r);
    // Bars, each 0..1: drag across them to paint values, as a step
    // sequencer's. Returns true on any change. Keys, with it focused: Left
    // and Right choose a bar, Home and End the first and last; Up and Down
    // change it (Shift finer, Page keys coarser), Delete or Backspace clears
    // it. For screen readers a group of one slider per bar.
    bool steps (std::vector<float>& values, Rect r);
    // A piano keyboard from note lowest to highest (MIDI numbers; best from
    // and to a white key). Press a key, slide across keys; lit says which
    // notes to show held (e.g. from incoming MIDI). Played with the pointer;
    // to screen readers a picture of a keyboard, with its range. Two pianos
    // on a page need two indexes: each holds its own note. One taken away
    // while a key is held sends that note's off from its next call.
    Notes piano (int lowest, int highest, Rect r, const std::function<bool (int)>& lit = {}, int index = 0);

    // What a quick second press is: a double-click (the default: knobs reset,
    // fields select a word), or just another press, so a button, toggle or
    // menu reacts to every one.
    enum Presses
    {
        doubleClicks,
        eachPress
    };
    // A custom region (pixels): reports press, drag and release on it, plus
    // double-clicks and the wheel over it.
    struct Area
    {
        bool pressed = false, held = false, released = false, clicked = false, doubleClicked = false;
        float x = 0, y = 0, wheel = 0;
        unsigned modifiers = 0;
    };
    Area area (const void* id, float x, float y, float w, float h, Presses presses = doubleClicks);
    // The same for a Clay element declared in this layout, hit-tested by Clay.
    Area element (Clay_ElementId id, Presses presses = doubleClicks);

    // A grip in the bottom-right corner of a view `width` x `height` pixels:
    // dragging it resizes the window (through resizeTo; drawn only if set).
    // Hosts without resizing of their own, such as AU hosts, need one.
    void resizeGrip (float width, float height);

    float s = 1;                         // pixels per point for this frame
    float viewWidth = 0, viewHeight = 0; // the view's size in points, for placing popups

    // Screen readers: what this frame's controls described of themselves
    // (forward it from View::accessibility), and a screen reader's request
    // (from View::accessAction), done by the next frame.
    const Accessibility* accessibility() const
    {
        return &accessTree;
    }
    void accessAction (uint64_t id, Accessible::Action action, double value);
    // Describes something drawn by hand at r (pixels) for screen readers, as
    // aria-label does: what it is, its name, its value if any, and more about
    // it if needed (aria-description).
    void accessible (
        Accessible::Role role, const char* name, const char* value, Rect r, const char* description = nullptr);
    // The same for an element you declared and press with element(): a screen
    // reader's click on it arrives as a click there (a button of your own).
    void accessible (
        Clay_ElementId element, Accessible::Role role, const char* name, const char* description = nullptr);
    // An element of your own (a plot) as a stop in the Tab order, as
    // tabindex="0" makes one: Tab reaches it (a click doesn't focus it), and
    // with a contextMenu for it the Menu key or Shift+F10 opens that. To
    // screen readers one node, of this role and name; a view drawn to fill
    // it (Ui::accessible at its box) gives that node its description rather
    // than a node of its own. True while it has focus from the keys: draw a
    // focus ring then.
    bool tabStop (Clay_ElementId element, Accessible::Role role, const char* name);
    // Where the pointer is (pixels) and whether it's down: for a drag that
    // outlives the control it began on (a dock's tab pulled into a window).
    struct PointerAt
    {
        float x, y;
        bool down;
    };
    PointerAt pointerAt() const
    {
        return { mouse.x, mouse.y, mouse.down };
    }
    // An id for a box you declare (CLAY({.id = ui.id("Name", n)})), mixed with
    // the scope pushId set, as ImGui's PushID: a widget of your own (a dock)
    // scopes its ids by itself, so its names and numbers can't clash with the
    // page's. Ui's controls take their ids from here too.
    Clay_ElementId id (const char* name, int index = 0) const;
    void pushId (const void* scope);
    void popId();
    // An id as declared inside pushId(scope), with no scope around it: for
    // finding it from outside (tests). Nested scopes mix with their parents'.
    static Clay_ElementId idIn (const void* scope, const char* name, int index = 0);
    // The last frame's box of an element (points; all 0 if it wasn't laid out),
    // and whether the pointer is over it.
    Rect bounds (Clay_ElementId element) const;
    bool pointerOver (Clay_ElementId element) const;
    // A box that draws itself: pass the result as .custom = {.customData = ...};
    // draw gets the box's rectangle in pixels when it's laid out. For views of
    // your own (views.h is made this way).
    void* paint (std::function<void (Rect)> drawing);
    // For screen readers, about the control declared just before, as the
    // aria- attributes do. Controls name themselves (a button by its label, a
    // knob by its parameter); a dropdown, text field or segmented control has
    // no name of its own, so give it one. A tooltip is its description.
    void label (const char* name);
    void description (const char* text);
    void hidden();                        // leave it out
    void hidden (Clay_ElementId element); // leave out an element and what's in it: decoration, or text said elsewhere
    // Read out now, without moving focus (aria-live): "Preset loaded".
    void announce (const char* text);

private:
    enum Builtin
    {
        knobBox = -1,
        sliderBox = -2,
        chevronBox = -3,
        chevronRightBox = -4,
        vsliderBox = -5,
        xyBox = -6,
        meterBox = -7,
        imageKnobBox = -8,
        pianoBox = -9,
        envelopeBox = -10,
        stepsBox = -11,
        gripBox = -12,
        closeBox = -13,
        paintBox = -14,
        chevronLeftBox = -15,
        chevronUpBox = -16,
        submenuBox = -17,
        tickBox = -18
    };
    struct Placed
    {
        Params* params;
        Param* p;
        Format format;
        bool log;
        Param* second = nullptr;    // an XY pad's y
        float level = 0, peak = -1; // a meter's
        int image = -1, frames = 0; // a filmstrip knob's
        bool across = false;
        int lowest = 0, highest = 0; // a piano's
        uint32_t keyboard = 0;       // and its id
        std::function<bool (int)> lit;
        std::vector<Point>* points = nullptr; // an envelope's
        std::vector<float>* values = nullptr; // steps'
    };
    std::deque<Placed> placed;                       // knobs and sliders the layout places, until end()
    std::deque<std::function<void (Rect)>> painters; // paint()'s, until end()

    // 0..1 position of a value, and back.
    static double toUnit (const Param& p, double v, bool log);
    static double fromUnit (const Param& p, double u, bool log);
    // Handles all input for the control with rectangle x,y,w,h. Returns true
    // while that control is being dragged.
    static const void* controlId (const Param& p, float x, float y);
    // ends: how far inside the box the track's ends are (half a thumb that
    // overhangs them), for an absolute drag to put the value under the pointer.
    bool control (Params& params, Param& p, const void* me, float x, float y, float w, float h, bool vertical,
        float travel, bool log, bool typable = false, bool absolute = false, float ends = 0);
    void typeInto (const void* me, const void* entry, const Param& p);
    // A whole edit: begin, change and end at once (wheel, keys, double-click).
    void set (Params& params, Param& p, double value);
    bool over (float x, float y, float w, float h) const;
    Area press (const void* id, bool over, Presses presses);
    // Screen readers: one entry per control this frame, and the tree made of them.
    struct Described
    {
        Accessible node;
        uint32_t element = 0; // a Clay element: its box is the bounds, once laid out
        Params* params = nullptr;
        Param* param = nullptr; // a parameter's control: set by setValue
        uint64_t parent = 0;    // 0: the view's
        float place = -1;       // its place among its siblings when said by hand, not by where it is drawn
        bool stop = false;      // a tabStop: takes the description of a view drawn at its box
    };
    // A drawer's header: the title after a chevron, or (arrow: a chevron
    // Builtin) an overlay drawer's tab with that arrow, upright (tall) or
    // lying along a top or bottom edge, floating where it's told.
    bool drawerHeader (const char* title, bool& open, int index, int arrow = 0, bool tall = true,
        Clay_FloatingElementConfig floating = {}, float tab = 24, bool quiet = false);
    // Scrolls a scroll container (declared next) so the focused control,
    // if it's in it and focus moved with the keys, is in view.
    void scrollToFocus (Clay_ElementId container);
    const void* scrolledTo = nullptr; // the control it last scrolled to
    bool wheelNeedsFocus = false;     // in a scrolling panel: the wheel turns a control only while it's focused
    // Open overlay drawers, as the last frame had them: their box and their
    // tab; and the one an Escape is to close (see key).
    std::vector<std::pair<uint32_t, uint32_t>> overlays, overlaysNow;
    uint32_t escapeCloses = 0;
    bool handle (Clay_ElementId hid, const char* name, float& size, float min, float most, bool vertical, float origin,
        int sign);
    void handleBox (Clay_ElementId hid, bool vertical);
    std::vector<Described> described;
    std::vector<std::pair<uint64_t, float>> reading; // each control's place in layout order, for Tab too
    // label, description and hidden, for a control's element: applied once
    // the frame is laid out (a knob is described as Clay draws it).
    struct Said
    {
        uint32_t element;
        std::string label, description;
        bool hidden = false, everything = false, fromTooltip = false;
        bool menu = false; // it has a context menu: screen readers can open it
    };
    std::vector<Said> said;
    std::string announcement; // to be read out, until it's published
    bool announcer = false;   // the live region exists: kept, so each announcement is a change to it
    Accessibility accessTree;
    const void* accessClick = nullptr; // clicked by a screen reader: press() reports it
    Described& describe (const void* me, Accessible::Role role, const char* name, uint64_t parent = 0);
    Described& describeParam (const void* me, Params& params, Param& p, const char* value, Rect r);
    void finishDescribing();
    void focusOwner();
    bool focusShown (const void* id) const
    {
        return focus == id && focusVisible;
    }
    bool focusShown (Clay_ElementId element) const;
    std::vector<View::Key> focusable (Clay_ElementId element, const Area& a);
    // A control made of parts (bars, points, keys): the part its keys are on,
    // and the id each is described by. A part a screen reader focused or
    // clicked is the control's, with the part chosen.
    int& within (const void* control);
    static const void* partId (const void* control, int part)
    {
        return reinterpret_cast<const char*> (control) + 2 * part + 1;
    }
    void takePartFocus (const void* control, int first, int last);
    // A screen reader's step or value for a part, kept for the control to apply.
    struct Asked
    {
        uint64_t id;
        Accessible::Action action;
        double value;
    };
    std::vector<Asked> asked;
    std::vector<std::pair<const void*, int>> withins;
    void drawGrip();
    Rect grip {}; // resizeGrip's view size this frame
    Clay_TextElementConfig* textConfig (Color color, int font = -1);
    // textX: where the field's text starts, in points, for placing the caret
    // with the pointer; NaN where the text isn't left-aligned.
    bool editKeys (uint32_t field, std::string& value, bool overField, float textX = NAN);
    static std::string plainNumber (double value);
    bool valueEntry (Params& params, Param& p, const void* control, const void* id, Rect r, Draw2D::Align align);
    void readout (Params& params, Param& p, const char* label, const Format& format, bool log, const char* const* items,
        int count, float widthPoints);
    void startEditing (uint32_t field, std::string text, bool selectAll = false);
    void keysToEdit();
    void fieldText (const std::string& shown, bool edit);

    struct
    {
        float x = 0, y = 0, wheel = 0;
        float scroll = 0; // the wheel as the system scrolls content (natural scrolling flips it)
        unsigned modifiers = 0;
        bool down = false, pressed = false, released = false, doubleClicked = false, moved = false;
        bool secondary = false; // the secondary button was pressed: a context menu opens
    } mouse;
    std::chrono::steady_clock::time_point lastPress;
    float lastPressX = -1e9f, lastPressY = -1e9f;

    const void* active = nullptr; // the control being dragged
    bool activeSeen = false;      // it was declared this frame
    struct Held
    {
        Params* params = nullptr;
        Param *first = nullptr, *second = nullptr; // the gesture it began
        bool locked = false;                       // it locked the pointer
    } held;
    float grabX = 0, grabY = 0;
    float gripOffsetX = 0, gripOffsetY = 0; // from the pointer to the view's corner
    double grabUnit = 0;
    bool grabFine = false;
    // A second press soon after the first (as compost's): released where it
    // was, a double-click (reset); moved, a fine drag.
    bool grabSecond = false, grabMoved = false;
    bool grabRelative = true; // the drag moves the value by how far it goes (not to where the pointer is)
    float pressX = 0, pressY = 0;

    const void* focus = nullptr;    // the control keys go to
    bool focusVisible = false;      // after a key, until the next click
    unsigned focusUses = 0;         // the keys the focused control uses: a bit per View::Key
    bool focusTakesDigits = false;  // it's a number box: typing a number starts editing it
    std::string typedToFocus;       // that typing, until the number box takes it
    bool focusTakesLetters = false; // it's a dropdown: letters jump to an option, Space opens or chooses
    bool focusTakesSpace = false;   // it's a button, toggle or drawer: Space presses it, as Enter does
    int highlight = -1;             // the open dropdown's highlighted option (the keys move it)
    float splitGrab = 0;            // a divider being dragged: from the pointer to the divider's start (points)
    std::vector<std::pair<uint32_t, float>> splitRestore; // a collapsed split's size before, by divider
    View::Cursor cursor = View::Cursor::arrow, cursorShown = View::Cursor::arrow; // wanted this frame, and set
    std::vector<uint32_t> windowOrder;                                            // windows back to front, by element
    std::vector<uint32_t> windowsNow;                                             // those declared this frame
    std::vector<std::pair<uint32_t, uint64_t>> windowDialogs; // this frame's open ones: element, and its dialog's node
    uint32_t windowUnder = 0;                   // the frontmost window under the pointer, from the last frame's layout
    float windowGrabX = 0, windowGrabY = 0;     // from the pointer to what's dragged (a header or a grip), points
    bool pressFocused = false;                  // this frame's press focused a control
    bool wheelUsed = false;                     // a control turned with this frame's wheel
    bool scrollWaiting = false;                 // the wheel scrolls, in the next frame
    std::optional<View::PointerEvent> heldMove; // after a press not yet seen; see pointer()
    std::vector<std::pair<View::Key, unsigned>> keys; // this frame's
    struct Edit
    {
        std::string text;
        View::Key key;
        unsigned modifiers;
    }; // typed text, or else a key
    std::vector<Edit> edits;        // this frame's, for the text field
    std::vector<const void*> order; // controls in drawing order, for Tab

    uint32_t openMenu = 0;  // the open dropdown's or context menu's menu (a Clay id), or 0
    bool menuSeen = false;  // the open menu was declared this frame
    bool overPopup = false; // the pointer is over the open menu, so not over what is under it
    // The open context menu: the element it's for (0 for a dropdown's menu),
    // the control focus goes back to, and where it opens (pixels: the
    // pointer, or the control's box; below it, or above where there's no room).
    uint32_t menuFor = 0;
    const void* menuReturn = nullptr;
    Rect menuAt {};
    bool focusHasMenu = false; // the focused control has a context menu: the Menu key is the view's
    // Its open submenus: the item each was opened from, level by level (the
    // deepest's highlighted item is highlight), and their boxes as drawn.
    std::vector<int> submenuPath;
    std::vector<uint32_t> submenuBoxes;
    // The pointer heading for an open submenu across other items: where it
    // was last frame, and since when it has rested on one of those (pixels).
    float aimX = 0, aimY = 0;
    bool aiming = false;
    std::chrono::steady_clock::time_point aimSince;
    void closeMenu();
    std::vector<const void*>::const_iterator tabFrom() const;
    bool tabLeaves (bool back) const;
    bool controlIn (const void* control, Clay_ElementId element) const;
    // The popup list of a dropdown or a context menu (see ui_controls.cpp).
    int popup (Clay_ElementId menu, const char* itemName, int itemBase, const char* const* items, int count, int marked,
        Clay_FloatingElementConfig floating, float minWidth);
    uint32_t lastElement = 0; // the control declared last, for tooltip
    uint32_t hovered = 0;     // the control under the pointer, and since when
    std::chrono::steady_clock::time_point hoveredSince;
    bool tooltipWaiting = false;
    bool settle = false;                 // input arrived this frame: draw one more
    bool animateAsked = false;           // animate() this frame
    const void* splitterAsked = nullptr; // a splitter a screen reader set, taken when it's next drawn
    double splitterTo = 0;               // to this size (points)
    std::vector<const void*> scopes;     // pushId's, innermost last
    bool leftAfterRelease = false;       // the pointer left the view after a release this frame hasn't seen

    // Each piano's, by its id: the note held down, a note-off owed since it
    // went away with a key held.
    struct Keyboard
    {
        uint32_t id;
        int playing = -1, owed = -1;
    };
    std::vector<Keyboard> keyboards;
    Keyboard& keyboardOf (uint32_t id);
    // play: takes the pointer and returns the notes; show: draws it.
    Notes keyboard (
        uint32_t id, int lowest, int highest, Rect r, const std::function<bool (int)>& lit, bool play, bool show);
    int grabbedPoint = -1;    // the envelope point being dragged
    uint32_t numberDrag = 0;  // the number box being dragged
    bool numberMoved = false; // past its dead zone
    double numberStart = 0;   // its value when the drag began, for Escape
    float itemWheel = 0;      // a readout's wheel turned less than an item's step, kept for the next turn
    int lastStep = -1;        // the step painted last in a drag
    std::vector<const void*> changedNow,
        changedLast; // envelopes and steps (by their data) changed in this end() and the last
    struct
    {
        bool over = false, dropped = false; // being dragged over us, or let go this frame
        float x = 0, y = 0;                 // pixels
        std::vector<std::string> paths;
    } incoming;                                        // files dragged in from elsewhere
    std::vector<uint32_t> dropTargets, dropTargetsNow; // declared last frame, and this one
    bool dragArmed = false;                            // a press on a dragSource that hasn't moved far yet
    float dragFromX = 0, dragFromY = 0;
    bool inside (Clay_ElementId id, float x, float y) const; // in last frame's box; pixels
    uint32_t editing = 0;                                    // the text field being edited, or 0
    uint32_t restingField = 0; // a text field with focus after its edit ended (Enter, Escape), not editing
    float fieldScroll = 0;     // how far its text is scrolled left (points)
    View::Caret caretAt;       // where its caret is (pixels), for input methods
    std::string draft;         // its text while editing
    size_t caret = 0;          // byte offsets in draft: the caret, and the selection
    size_t selectFrom = 0, selectTo = 0;
    std::unique_ptr<TextEdit> textEdit;
    uint32_t pendingField = 0; // a field left by clicking another, and its edit
    std::string pendingDraft;
};

} // namespace gpu
