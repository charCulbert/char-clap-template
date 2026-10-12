#pragma once
#include <webgpu/webgpu_cpp.h>
#include "../core/messages.h"
#include "accessibility.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// A plugin's GPU-drawn interface. The same View runs natively (gpu/gui.cpp,
// Dawn) and in a WCLAP host's page (gpu/web/main.cpp, the browser's WebGPU).
// Presenters own the device and surface; a View records draw calls, handles
// the pointer, and exchanges the plugin's messages.
namespace gpu
{

class View
{
public:
    enum class Pointer
    {
        down,
        move,
        up,
        wheel,
        leave
    }; // leave: the pointer left the view
    enum Modifier : unsigned
    {
        shift = 1,
        control = 2,
        alt = 4,
        command = 8
    };
    struct PointerEvent
    {
        Pointer type;
        float x, y;             // pixels
        unsigned modifiers = 0; // Modifier bits
        float wheel = 0;        // for Pointer::wheel: notches, positive away from the user
        bool inverted = false;  // and the system scrolls content the other way (macOS natural scrolling)
        // For Pointer::down and up: 0 the primary button, 1 the secondary
        // (right, or Ctrl+click on macOS: a context menu), 2 the middle.
        // Moves, wheel and leave are 0. A view that only knows the primary
        // button ignores presses that aren't 0.
        int button = 0;
    };
    // The keys controls and text fields use; other keys go back to the host.
    // contextMenu is the Menu key or Shift+F10. The last five are the
    // platform's shortcuts for them (Cmd or Ctrl + Z, Shift+Z or Y, A, X, C).
    // Paste comes as text().
    enum class Key
    {
        left,
        right,
        up,
        down,
        pageUp,
        pageDown,
        home,
        end,
        tab,
        enter,
        escape,
        backspace,
        deleteForward,
        contextMenu,
        undo,
        redo,
        selectAll,
        cut,
        copy
    };

    virtual ~View() = default;
    virtual void prepare (const wgpu::Device& device, wgpu::TextureFormat format) = 0;
    // Optional: builds the frame without the GPU and returns false if it
    // would look just like the one on screen, which the presenter then leaves
    // there (no acquire, submit or present). draw() follows only after true.
    virtual bool record (float /*width*/, float /*height*/, float /*scale*/)
    {
        return true;
    }
    // Sizes are in pixels; scale is pixels per logical point (for line widths).
    virtual void draw (const wgpu::RenderPassEncoder& pass, float width, float height, float scale) = 0;
    virtual void pointer (const PointerEvent& event) = 0;
    // Returns true if the key was used; presenters pass unused keys to the host.
    virtual bool key (Key, unsigned /*modifiers*/)
    {
        return false;
    }
    // Typed text (UTF-8); returns true if used, e.g. by a focused text field.
    virtual bool text (const char* /*utf8*/)
    {
        return false;
    }
    virtual void receive (const core::Value& message) = 0;
    // Presenters draw only after input, a message or a resize, plus every
    // frame while this is true (e.g. during an animation).
    virtual bool animating() const
    {
        return false;
    }
    // True while a text field has the keys, so a host that acts on keys
    // before the focused window (REAPER: Space plays) leaves them alone.
    virtual bool editingText() const
    {
        return false;
    }
    // Where its caret is (pixels), so the system can put an input method's
    // candidate window by it; height 0 when not known.
    struct Caret
    {
        float x = 0, y = 0, height = 0;
    };
    virtual Caret textCaret() const
    {
        return {};
    }
    // Files dragged in from elsewhere (UTF-8 paths) moving over the view, let
    // go on it, or taken away again; pixels. Return true where files would be
    // taken (the presenter shows a copy cursor) and when a drop was used.
    enum class FileDrag
    {
        over,
        drop,
        leave
    };
    virtual bool files (FileDrag, float /*x*/, float /*y*/, const std::vector<std::string>& /*paths*/)
    {
        return false;
    }
    // What a screen reader sees, as of the last record(); nullptr if the view
    // offers nothing. A screen reader's request comes back as accessAction,
    // on the main thread, and the presenter draws again after it.
    virtual const Accessibility* accessibility() const
    {
        return nullptr;
    }
    virtual void accessAction (uint64_t /*id*/, Accessible::Action, double /*value*/) {}

    // Set by the presenter: the system's light or dark appearance. It draws
    // again when this changes.
    bool darkMode = true;

    // Set by the presenter: delivers a message to the plugin.
    std::function<void (const core::Value&)> send;
    // Set by the presenter: while locked, the cursor is hidden and stays put,
    // and pointer() positions keep moving with the mouse past the view's and
    // the screen's edges. Where the platform refuses, drags work as before.
    std::function<void (bool locked)> lockPointer;
    // Set by the presenter where it can (natively): the pointer's shape over
    // the view, until it's set again.
    enum class Cursor
    {
        arrow,
        text,
        hand,
        move,
        resizeLeftRight,
        resizeUpDown,
        resizeDiagonal
    };
    std::function<void (Cursor)> setCursor;
    // Set by the presenter where the view can ask for a new size (natively;
    // not in WCLAP pages): pixels, as draw() gets them. The plugin's
    // clap.gui.adjust_size limits it, and the host resizes the window.
    std::function<void (float width, float height)> resizeTo;
    // Set by the presenter: puts text on the clipboard (Key::copy and cut).
    std::function<void (const std::string& utf8)> copyText;
    // Set by the presenter where it can (natively): drags files out of the
    // view, to the desktop or a host's track. Call while the pointer is held;
    // the drag has the pointer until it's let go.
    std::function<void (const std::vector<std::string>& paths)> dragFiles;
};

// Implemented by the plugin's interface code (e.g. ui/AnalyzerView.cpp).
std::unique_ptr<View> createView();

// Reads a file the plugin ships (its resources/ folder), e.g. "/fonts/Inter.ttf".
// Natively it comes from the bundle's resources; in WCLAP it is built into the
// wasm. Returns empty when missing. Implemented by each presenter.
std::string loadAsset (const char* path);

} // namespace gpu
