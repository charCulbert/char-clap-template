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
#include <vector>

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
// keyboard focus, and Tab moves it on. A focused knob or slider steps with
// the arrows (Shift for fine), Page Up/Down and Home/End; Enter presses a
// button or toggle; the arrows move a segmented control; Enter opens and
// closes a dropdown and the arrows choose in it.
namespace gpu {

// The default look follows Slide: flat greys, hairlines, one warm accent, in
// a dark and a light version (Theme::dark(), Theme::light()).
struct Theme
{
    Color background{0.184f, 0.184f, 0.184f}; // #2f2f2f
    Color panel{0.227f, 0.227f, 0.227f};      // #3a3a3a: raised areas
    Color text{0.871f, 0.871f, 0.871f};       // #dedede
    Color dim{0.647f, 0.647f, 0.647f};        // #a5a5a5: labels, 4.5:1 on the panel
    Color edge{0.333f, 0.333f, 0.333f};       // #555555: hairlines, outlines
    Color track{0.333f, 0.333f, 0.333f};      // unfilled parts of knobs and sliders
    Color accent{0.867f, 0.753f, 0.494f};     // #ddc07e
    Color shadow{0, 0, 0, 0.45f};             // under menus and tooltips
    Color whiteKey{0.929f, 0.918f, 0.886f};   // #edeae2: a piano's keys, as a piano's in either palette
    Color blackKey{0.110f, 0.110f, 0.110f};   // #1c1c1c
    int labelFont = 0, valueFont = 0;         // Draw2D fonts for names and for numbers
    float textSize = 11, radius = 2;          // points

    static Theme dark() { return {}; }
    static Theme light()
    {
        Theme t;
        t.background = {0.804f, 0.804f, 0.804f}; // #cdcdcd
        t.panel = {0.761f, 0.761f, 0.761f};      // #c2c2c2
        t.text = {0.110f, 0.110f, 0.110f};       // #1c1c1c
        t.dim = {0.310f, 0.310f, 0.310f};        // #4f4f4f
        t.edge = t.track = {0.620f, 0.620f, 0.620f}; // #9e9e9e
        t.accent = {0.459f, 0.329f, 0.039f};     // #75540a
        t.shadow = {0, 0, 0, 0.22f};
        return t;
    }
    // The palette for the system's appearance, keeping fonts, sizes and key colours.
    Theme withColors(bool darkMode) const
    {
        Theme t = darkMode ? dark() : light();
        t.labelFont = labelFont, t.valueFont = valueFont, t.textSize = textSize, t.radius = radius;
        t.whiteKey = whiteKey, t.blackKey = blackKey;
        return t;
    }
};

struct TextEdit; // stb_textedit's state, in ui.cpp

class Ui
{
public:
    Ui();
    ~Ui();
    Draw2D draw;
    Layout layout;
    Theme theme;
    std::function<void(bool)> lockPointer;      // View::lockPointer: knob and slider drags lock the pointer
    std::function<void(float, float)> resizeTo; // View::resizeTo: for resizeGrip
    std::function<void(const std::string &)> copyText; // View::copyText: for copy and cut in text fields
    std::function<void(const std::vector<std::string> &)> dragFiles; // View::dragFiles: for dragSource

    void pointer(const View::PointerEvent &event);
    bool key(View::Key key, unsigned modifiers);
    bool text(const char *utf8);
    // True while something waits to be drawn without input (a tooltip's delay).
    bool animating() const;
    // True while a control is held: an edit isn't finished until it's let go.
    bool dragging() const { return active != nullptr; }
    bool editingText() const { return editing != 0; }
    View::Caret textCaret() const { return editing ? caretAt : View::Caret{}; } // a text field's, in pixels
    // View::files: true over a dropTarget declared last frame.
    bool files(View::FileDrag type, float x, float y, const std::vector<std::string> &paths);

    // Starts a frame of controls and its layout; the canvas in pixels.
    void begin(float width, float height, float scale);
    // Lays out and draws the frame. onCustom gets your own custom boxes
    // (Layout::custom) as they come up in drawing order.
    void end(const wgpu::RenderPassEncoder &pass, const std::function<void(int kind, void *data, Rect pixels)> &onCustom = {});
    // end() in two halves, for View::record and View::draw: record lays out
    // and records the frame, returning false if it looks like the last one
    // drawn; flush draws it.
    bool record(const std::function<void(int kind, void *data, Rect pixels)> &onCustom = {});
    void flush(const wgpu::RenderPassEncoder &pass) { draw.flush(pass); }

    // A piano's note-on and note-off this frame, -1 for none.
    struct Notes { int on = -1, off = -1; };
    // An envelope's breakpoint, 0..1 across and up.
    struct Point { float x, y; };

    // ---- In the layout: Clay sizes and places these, so declare them inside
    // CLAY({...}) blocks. Each returns true when the user changed something.
    bool button(const char *label, int index = 0);
    bool toggle(const char *label, bool &on, int index = 0);
    bool segmented(const char *const *items, int count, int &selected, int index = 0);
    bool dropdown(const char *const *items, int count, int &selected, int index = 0);
    // The same three for a parameter, sending its gesture (begin, change, end)
    // when clicked. A toggle is on in the upper half of the range; a segmented
    // control or dropdown's item i is the value min + i.
    bool toggle(Params &params, Param *p, const char *label);
    bool segmented(Params &params, Param *p, const char *const *items, int count);
    bool dropdown(Params &params, Param *p, const char *const *items, int count);
    // Edits text; returns true when the edit is committed (Enter, or a click
    // elsewhere). Escape abandons it.
    bool textField(std::string &value, float widthPoints = 120, int index = 0);
    // Takes files dropped on the element: true when some land on it, with
    // their paths in `files`. fileOver: files are being dragged over it.
    bool dropTarget(Clay_ElementId id, std::vector<std::string> &files);
    bool fileOver(Clay_ElementId id) const;
    // Drags files out of the element: once a press on it moves 4 points,
    // `make` is called for the files (so it can write them then) and the
    // presenter drags them. Natively only; elsewhere nothing happens.
    void dragSource(Clay_ElementId id, const std::function<std::vector<std::string>()> &make);
    // Shows text near the control declared just before, after a moment's hover.
    void tooltip(const char *text);
    // Drag up or down to change, Shift for fine; double-click to type a value.
    void numberBox(Params &params, Param *p, const char *format, bool log = false, float widthPoints = 64);
    // A header that opens and closes the section under it: declare the
    // section while this returns true.
    bool drawer(const char *title, bool &open, int index = 0);
    // Controls of their own size, in the layout.
    void knob(Params &params, Param *p, const char *format, bool log = false, float diameter = 44);
    void slider(Params &params, Param *p, const char *format, bool log = false);
    void vslider(Params &params, Param *p, const char *format, bool log = false, float heightPoints = 140);
    void xyPad(Params &params, Param *x, Param *y); // fills the space it's given
    void meter(float level, float peak = -1, float widthPoints = 6); // 0..1, filled from the bottom
    // The envelope and steps editors, filling the space they're given.
    // Their changes show from the next frame's call (drawing happens at end()).
    bool envelope(std::vector<Point> &points);
    bool steps(std::vector<float> &values);
    // A piano keyboard filling the width, heightPoints tall. Its notes come
    // back from the next frame's call (drawing happens at end()).
    Notes piano(int lowest, int highest, float heightPoints = 64, const std::function<bool(int)> &lit = {});
    // A filmstrip knob at the size of one frame (in points, at scale 1 a pixel each).
    void imageKnob(Params &params, Param *p, int image, int frames, bool log = false, bool across = false);

    // ---- At a rectangle you choose (pixels): for drawing inside a Layout::custom
    // box, or anywhere else Clay doesn't place things.
    // A rotary control with its value and name below; drag up/down to change.
    // log: logarithmic mapping (min > 0).
    void knob(Params &params, Param *p, Rect r, const char *format, bool log = false);
    // A horizontal slider, its name and value above it; drag left/right.
    void slider(Params &params, Param *p, Rect r, const char *format, bool log = false);
    // A knob drawn from a filmstrip: frames of a Draw2D image, stacked down
    // (or across) from the minimum to the maximum. Drags like knob.
    void imageKnob(Params &params, Param *p, int image, int frames, Rect r, bool log = false, bool across = false);
    // An upright slider, its value above and name below; drag up/down.
    void vslider(Params &params, Param *p, Rect r, const char *format, bool log = false);
    // A point dragged in two dimensions: x and y each a parameter.
    void xyPad(Params &params, Param *x, Param *y, Rect r);
    void meter(float level, float peak, Rect r);
    // Breakpoints, 0..1 across and up, sorted by x: drag a point (between its
    // neighbours; the ends keep their x), double-click empty space to add
    // one, double-click a point to remove it. Returns true on any change.
    bool envelope(std::vector<Point> &points, Rect r);
    // Bars, each 0..1: drag across them to paint values, as a step
    // sequencer's. Returns true on any change.
    bool steps(std::vector<float> &values, Rect r);
    // A piano keyboard from note lowest to highest (MIDI numbers; best from
    // and to a white key). Press a key, slide across keys; lit says which
    // notes to show held (e.g. from incoming MIDI).
    Notes piano(int lowest, int highest, Rect r, const std::function<bool(int)> &lit = {});

    // What a quick second press is: a double-click (the default: knobs reset,
    // fields select a word), or just another press, so a button, toggle or
    // menu reacts to every one.
    enum Presses { doubleClicks, eachPress };
    // A custom region (pixels): reports press, drag and release on it, plus
    // double-clicks and the wheel over it.
    struct Area
    {
        bool pressed = false, held = false, released = false, clicked = false, doubleClicked = false;
        float x = 0, y = 0, wheel = 0;
        unsigned modifiers = 0;
    };
    Area area(const void *id, float x, float y, float w, float h, Presses presses = doubleClicks);
    // The same for a Clay element declared in this layout, hit-tested by Clay.
    Area element(Clay_ElementId id, Presses presses = doubleClicks);

    // A grip in the bottom-right corner of a view `width` x `height` pixels:
    // dragging it resizes the window (through resizeTo; drawn only if set).
    // Hosts without resizing of their own, such as AU hosts, need one.
    void resizeGrip(float width, float height);

    float s = 1; // pixels per point for this frame
    float viewWidth = 0, viewHeight = 0; // the view's size in points, for placing popups

private:
    enum Builtin { knobBox = -1, sliderBox = -2, chevronBox = -3, chevronRightBox = -4, vsliderBox = -5, xyBox = -6, meterBox = -7,
                   imageKnobBox = -8, pianoBox = -9, envelopeBox = -10, stepsBox = -11 };
    struct Placed
    {
        Params *params;
        Param *p;
        const char *format;
        bool log;
        Param *second = nullptr; // an XY pad's y
        float level = 0, peak = -1; // a meter's
        int image = -1, frames = 0;  // a filmstrip knob's
        bool across = false;
        int lowest = 0, highest = 0; // a piano's
        std::function<bool(int)> lit;
        std::vector<Point> *points = nullptr; // an envelope's
        std::vector<float> *values = nullptr; // steps'
    };
    std::deque<Placed> placed; // knobs and sliders the layout places, until end()

    // 0..1 position of a value, and back.
    static double toUnit(const Param &p, double v, bool log);
    static double fromUnit(const Param &p, double u, bool log);
    // Handles all input for the control with rectangle x,y,w,h. Returns true
    // while that control is being dragged.
    static const void *controlId(const Param &p, float x, float y);
    bool control(Params &params, Param &p, const void *me, float x, float y, float w, float h, bool vertical, float travel, bool log);
    // A whole edit: begin, change and end at once (wheel, keys, double-click).
    void set(Params &params, Param &p, double value);
    bool over(float x, float y, float w, float h) const;
    Area press(const void *id, bool over, Presses presses);
    bool focusShown(const void *id) const { return focus == id && focusVisible; }
    bool focusShown(Clay_ElementId element) const;
    std::vector<View::Key> focusable(Clay_ElementId element, const Area &a);
    void drawGrip();
    Rect grip{}; // resizeGrip's view size this frame
    Clay_TextElementConfig *textConfig(Color color, int font = -1);
    // textX: where the field's text starts, in points, for placing the caret
    // with the pointer; NaN where the text isn't left-aligned.
    bool editKeys(uint32_t field, std::string &value, bool overField, float textX = NAN);
    static std::string plainNumber(double value);
    bool valueEntry(Params &params, Param &p, const void *id, Rect r, Draw2D::Align align);
    void startEditing(uint32_t field, std::string text, bool selectAll = false);
    void fieldText(const std::string &shown, bool edit);
    Clay_ElementId id(const char *kind, int index);

    struct
    {
        float x = 0, y = 0, wheel = 0;
        float scroll = 0; // the wheel as the system scrolls content (natural scrolling flips it)
        unsigned modifiers = 0;
        bool down = false, pressed = false, released = false, doubleClicked = false;
    } mouse;
    std::chrono::steady_clock::time_point lastPress;
    float lastPressX = -1e9f, lastPressY = -1e9f;

    const void *active = nullptr; // the control being dragged
    bool activeSeen = false;      // it was declared this frame
    struct Held
    {
        Params *params = nullptr;
        Param *first = nullptr, *second = nullptr; // the gesture it began
        bool locked = false;                       // it locked the pointer
    } held;
    float grabX = 0, grabY = 0;
    float gripOffsetX = 0, gripOffsetY = 0; // from the pointer to the view's corner
    double grabUnit = 0;
    bool grabFine = false;

    const void *focus = nullptr; // the control keys go to
    bool focusVisible = false;   // after a key, until the next click
    unsigned focusUses = 0;      // the keys the focused control uses: a bit per View::Key
    bool focusTakesDigits = false; // it's a number box: typing a number starts editing it
    std::string typedToFocus;    // that typing, until the number box takes it
    bool pressFocused = false;   // this frame's press focused a control
    bool wheelUsed = false;      // a control turned with this frame's wheel
    bool scrollWaiting = false;  // the wheel scrolls, in the next frame
    std::optional<View::PointerEvent> heldMove; // after a press not yet seen; see pointer()
    std::vector<std::pair<View::Key, unsigned>> keys; // this frame's
    struct Edit { std::string text; View::Key key; unsigned modifiers; }; // typed text, or else a key
    std::vector<Edit> edits;                          // this frame's, for the text field
    std::vector<const void *> order;                  // controls in drawing order, for Tab

    uint32_t openMenu = 0;      // the open dropdown's menu (a Clay id), or 0
    bool menuSeen = false;      // the open menu was declared this frame
    bool overPopup = false;     // the pointer is over the open menu, so not over what is under it
    uint32_t lastElement = 0;   // the control declared last, for tooltip
    uint32_t hovered = 0;       // the control under the pointer, and since when
    std::chrono::steady_clock::time_point hoveredSince;
    bool tooltipWaiting = false;
    bool settle = false;        // input arrived this frame: draw one more

    int playing = -1;           // the piano note held down, or -1
    Notes pianoNotes;           // what the piano drawn last frame reported
    int grabbedPoint = -1;      // the envelope point being dragged
    uint32_t numberDrag = 0;    // the number box being dragged
    bool numberMoved = false;   // past its dead zone
    double numberStart = 0;     // its value when the drag began, for Escape
    int lastStep = -1;          // the step painted last in a drag
    std::vector<const void *> changedNow, changedLast; // envelopes and steps (by their data) changed in this end() and the last
    struct
    {
        bool over = false, dropped = false; // being dragged over us, or let go this frame
        float x = 0, y = 0;                 // pixels
        std::vector<std::string> paths;
    } incoming;                                    // files dragged in from elsewhere
    std::vector<uint32_t> dropTargets, dropTargetsNow; // declared last frame, and this one
    bool dragArmed = false;                        // a press on a dragSource that hasn't moved far yet
    float dragFromX = 0, dragFromY = 0;
    bool inside(Clay_ElementId id, float x, float y) const; // in last frame's box; pixels
    uint32_t editing = 0;       // the text field being edited, or 0
    float fieldScroll = 0;      // how far its text is scrolled left (points)
    View::Caret caretAt;        // where its caret is (pixels), for input methods
    std::string draft;          // its text while editing
    size_t caret = 0;           // byte offsets in draft: the caret, and the selection
    size_t selectFrom = 0, selectTo = 0;
    std::unique_ptr<TextEdit> textEdit;
    uint32_t pendingField = 0;  // a field left by clicking another, and its edit
    std::string pendingDraft;
};

}
