// Runs a gpu::View in a WCLAP host's page: the browser's WebGPU draws into
// #canvas, and messages travel as clap.webview/3 describes (ArrayBuffers via
// window.parent.postMessage, and 'message' events back), encoded as libs/core/messages.h describes.
//
// For screen readers the View's accessibility tree is mirrored as elements
// with WAI-ARIA roles over the canvas (as Flutter's web build does): they take
// no pointer events, so the mouse still reaches the canvas, and DOM focus
// follows the View's. Files dropped on the canvas are written to /drop in
// Emscripten's file system, and their paths given to the View.
#include "../context.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

gpu::Context context;
std::unique_ptr<gpu::View> view;
bool ready = false;
bool dirty = true; // something changed since the last frame

EM_JS(void, postToPlugin, (const unsigned char *data, int size), {
    window.parent.postMessage(HEAPU8.slice(data, data + size).buffer, '*');
});

float pixelRatio() { return float(emscripten_get_device_pixel_ratio()); }

EM_JS(bool, prefersDark, (), { return (Module.darkQuery ??= matchMedia('(prefers-color-scheme: dark)')).matches; });

// Pointer lock needs a recent click (it comes from a knob press) and, inside
// an iframe, the host's sandbox must allow it; if refused, drags stay plain.
EM_JS(void, setPointerLock, (bool locked), {
    const canvas = document.getElementById('canvas');
    if (locked) try { Promise.resolve(canvas.requestPointerLock()).catch(() => {}); } catch {}
    else if (document.pointerLockElement === canvas) document.exitPointerLock();
});
EM_JS(bool, pointerLocked, (), {
    return document.pointerLockElement === document.getElementById('canvas');
});
float pointerX = 0, pointerY = 0; // pixels; keeps moving while the pointer is locked

EM_JS(void, copyToClipboard, (const char *utf8, int size), {
    const text = new TextDecoder().decode(HEAPU8.slice(utf8, utf8 + size));
    if (navigator.clipboard) navigator.clipboard.writeText(text).catch(() => {});
});

EM_JS(void, setCursorCss, (const char *css), { document.getElementById('canvas').style.cursor = UTF8ToString(css); });

// The ARIA role for each of ours (no default: a new role must be given one).
const char *ariaRole(gpu::Accessible::Role role)
{
    using Role = gpu::Accessible::Role;
    switch (role)
    {
    case Role::window: return "group"; // the view itself: the page is the window
    case Role::group: return "group";
    case Role::button: return "button";
    case Role::checkBox: return "checkbox";
    case Role::radioGroup: return "radiogroup";
    case Role::radioButton: return "radio";
    case Role::comboBox: return "combobox";
    case Role::slider: return "slider";
    case Role::spinButton: return "spinbutton";
    case Role::textInput: return "textbox";
    case Role::label: return ""; // just its words
    case Role::image: return "img";
    case Role::listBox: return "listbox";
    case Role::option: return "option";
    case Role::splitter: return "separator"; // focusable, with a value: APG's window splitter
    case Role::dialog: return "dialog";
    case Role::tabList: return "tablist";
    case Role::tab: return "tab";
    case Role::tabPanel: return "tabpanel";
    }
    return "group";
}

void jsonString(std::ostringstream &out, const std::string &text)
{
    out << '"';
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            out << escaped;
        }
        else out << char(c);
    }
    out << '"';
}

// The mirror: one element per node, keyed by id, changed only where the
// tree differs; positions in CSS pixels, relative to the parent's.
EM_JS(void, accessUpdate, (const char *json, float ratio), {
    const access = Module.access ??= {elements: new Map(), focus: "", layer: null, syncing: false};
    const canvas = document.getElementById('canvas');
    if (!access.layer)
    {
        access.layer = document.createElement('div');
        access.layer.style.cssText = 'position:absolute;inset:0;pointer-events:none;overflow:hidden;color:transparent;font-size:1px';
        document.body.appendChild(access.layer);
        canvas.setAttribute('aria-hidden', 'true'); // the mirror says what it shows
    }
    const tree = JSON.parse(UTF8ToString(json));
    const byId = new Map(tree.nodes.map(node => [node.id, node]));
    const seen = new Set();
    const set = (element, name, value) => {
        if (value === null || value === undefined) element.removeAttribute(name);
        else if (element.getAttribute(name) !== String(value)) element.setAttribute(name, value);
    };
    const send = (id, action) => {
        const bytes = new TextEncoder().encode(id + '\0');
        const pointer = _malloc(bytes.length);
        HEAPU8.set(bytes, pointer);
        _gpuAccessAction(pointer, action, 0);
        _free(pointer);
    };
    const element = id => {
        let e = access.elements.get(id);
        if (e) return e;
        e = document.createElement('div');
        e.id = 'access-' + id;
        e.style.cssText = 'position:absolute;outline:none;overflow:hidden;white-space:nowrap';
        // A screen reader's press arrives as a click (the mouse can't: pointer-events none).
        e.addEventListener('click', event => {
            const node = e.node;
            if (!node) return;
            event.preventDefault();
            if (node.actions & 2) send(id, 2);
            else if (node.actions & (32 | 64)) send(id, node.expanded ? 64 : 32);
        });
        // A screen reader moving focus: the View's focus follows.
        e.addEventListener('focus', () => { if (!access.syncing && id !== access.focus) send(id, 1); });
        access.elements.set(id, e);
        return e;
    };
    const place = (node, parent, index, parentX, parentY) => {
        const e = element(node.id);
        e.node = node;
        seen.add(node.id);
        if (parent.children[index] !== e) parent.insertBefore(e, parent.children[index] || null);
        const x = node.x / ratio, y = node.y / ratio;
        e.style.left = (x - parentX) + 'px';
        e.style.top = (y - parentY) + 'px';
        e.style.width = node.w / ratio + 'px';
        e.style.height = node.h / ratio + 'px';
        set(e, 'role', node.role || null); // "" for words: no role
        const words = node.role === "" || node.role === "textbox";
        set(e, 'aria-label', !words && node.name ? node.name : null);
        if (node.role === 'textbox') set(e, 'aria-label', node.name || null);
        set(e, 'aria-description', node.description || null);
        set(e, 'aria-live', node.live ? 'polite' : null);
        const ranged = node.numeric && ['slider', 'spinbutton', 'separator'].includes(node.role);
        set(e, 'aria-valuenow', ranged ? node.number : null);
        set(e, 'aria-valuemin', ranged ? node.min : null);
        set(e, 'aria-valuemax', ranged ? node.max : null);
        set(e, 'aria-valuetext', ranged && node.value ? node.value : null);
        set(e, 'aria-orientation', node.orientation === 1 ? 'horizontal' : node.orientation === 2 ? 'vertical' : null);
        set(e, 'aria-checked', node.checked < 0 ? null : node.checked ? 'true' : 'false');
        set(e, 'aria-selected', ['option', 'tab'].includes(node.role) ? String(node.selected) : null);
        const expandable = node.role === 'combobox' || (node.actions & (32 | 64));
        set(e, 'aria-expanded', expandable ? String(node.expanded) : null);
        set(e, 'aria-haspopup', node.role === 'combobox' ? 'listbox' : null);
        set(e, 'aria-activedescendant', node.active ? 'access-' + node.active : null);
        set(e, 'tabindex', node.actions ? '-1' : null); // focusable from script; Tab goes through the View
        if (words && e.textContent !== node.value) e.textContent = node.value;
        const children = (node.children || []).map(id => byId.get(id)).filter(Boolean);
        children.forEach((child, i) => place(child, e, i, x, y));
        while (!words && e.children.length > children.length) e.lastElementChild.remove();
    };
    if (tree.nodes.length) place(tree.nodes[0], access.layer, 0, 0, 0);
    for (const [id, e] of access.elements)
        if (!seen.has(id)) e.remove(), access.elements.delete(id);
    // DOM focus follows the View's, while the page has it.
    if (tree.focus !== access.focus)
    {
        access.focus = tree.focus;
        const e = access.elements.get(tree.focus);
        const ours = document.activeElement === document.body || document.activeElement === canvas || access.layer.contains(document.activeElement);
        if (e && e.hasAttribute('tabindex') && document.hasFocus() && ours && document.activeElement !== e)
        {
            access.syncing = true;
            e.focus({preventScroll: true});
            access.syncing = false;
        }
    }
});

unsigned accessVersion = 0;

void mirrorAccessibility()
{
    const gpu::Accessibility *tree = view->accessibility();
    if (!tree || tree->version == accessVersion) return;
    accessVersion = tree->version;
    std::ostringstream out;
    out << "{\"focus\":\"" << tree->focus << "\",\"nodes\":[";
    for (size_t i = 0; i < tree->nodes.size(); ++i)
    {
        const gpu::Accessible &n = tree->nodes[i];
        out << (i ? "," : "") << "{\"id\":\"" << n.id << "\",\"role\":\"" << ariaRole(n.role) << "\",\"name\":";
        jsonString(out, n.name);
        out << ",\"value\":";
        jsonString(out, n.value);
        out << ",\"description\":";
        jsonString(out, n.description);
        out << ",\"numeric\":" << (n.numeric ? "true" : "false") << ",\"number\":" << n.number << ",\"min\":" << n.min
            << ",\"max\":" << n.max << ",\"checked\":" << n.checked << ",\"selected\":" << (n.selected ? "true" : "false")
            << ",\"expanded\":" << (n.expanded ? "true" : "false") << ",\"live\":" << (n.live ? "true" : "false")
            << ",\"orientation\":" << n.orientation << ",\"actions\":" << n.actions << ",\"x\":" << n.x << ",\"y\":" << n.y
            << ",\"w\":" << n.width << ",\"h\":" << n.height << ",\"active\":\"" << (n.activeDescendant ? std::to_string(n.activeDescendant) : "")
            << "\",\"children\":[";
        for (size_t c = 0; c < n.children.size(); ++c) out << (c ? "," : "") << '"' << n.children[c] << '"';
        out << "]}";
    }
    out << "]}";
    accessUpdate(out.str().c_str(), pixelRatio());
}

bool frame(double, void *)
{
    double cssWidth, cssHeight;
    emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight);
    const auto width = uint32_t(cssWidth * pixelRatio()), height = uint32_t(cssHeight * pixelRatio());
    if (width != context.width || height != context.height)
    {
        emscripten_set_canvas_element_size("#canvas", int(width), int(height));
        context.configure(width, height);
        dirty = true;
    }
    if (const bool dark = prefersDark(); dark != view->darkMode) view->darkMode = dark, dirty = true;
    if (dirty || view->animating()) context.frame(*view, pixelRatio());
    dirty = false;
    mirrorAccessibility();
    return true; // keep the requestAnimationFrame loop going
}

template <class Event> unsigned modifiers(const Event &event)
{
    return (event.shiftKey ? gpu::View::shift : 0u) | (event.ctrlKey ? gpu::View::control : 0u)
         | (event.altKey ? gpu::View::alt : 0u) | (event.metaKey ? gpu::View::command : 0u);
}

bool wheel(int, const EmscriptenWheelEvent *event, void *)
{
    if (!ready) return false;
    // deltaY is positive towards the user, in pixels (about 100 per notch), lines or pages.
    const double perNotch = event->deltaMode == DOM_DELTA_PIXEL ? 100 : event->deltaMode == DOM_DELTA_LINE ? 3 : 1;
    view->pointer({gpu::View::Pointer::wheel, float(event->mouse.targetX) * pixelRatio(),
                   float(event->mouse.targetY) * pixelRatio(), modifiers(event->mouse), float(-event->deltaY / perNotch)});
    dirty = true;
    return true; // don't also scroll the page
}

bool key(int, const EmscriptenKeyboardEvent *event, void *)
{
    using Key = gpu::View::Key;
    static const std::pair<const char *, Key> keys[] = {
        {"ArrowLeft", Key::left}, {"ArrowRight", Key::right}, {"ArrowUp", Key::up}, {"ArrowDown", Key::down},
        {"PageUp", Key::pageUp}, {"PageDown", Key::pageDown}, {"Home", Key::home}, {"End", Key::end}, {"Tab", Key::tab},
        {"Enter", Key::enter}, {"Escape", Key::escape}, {"Backspace", Key::backspace}, {"Delete", Key::deleteForward}};
    if (!ready) return false;
    // Ctrl or Cmd + Z, Shift+Z or Y, A, X, C, for a text field being edited.
    // Cmd+V is left to the browser, whose paste event brings the text.
    if ((event->ctrlKey || event->metaKey) && !event->altKey && std::strlen(event->key) == 1)
    {
        const char letter = char(event->key[0] | 0x20);
        const Key k = letter == 'z' ? (event->shiftKey ? Key::redo : Key::undo) : letter == 'y' ? Key::redo
                    : letter == 'a' ? Key::selectAll : letter == 'x' ? Key::cut : Key::copy;
        if (std::strchr("zyaxc", letter) && view->key(k, modifiers(*event))) return dirty = true;
        return false;
    }
    for (auto [name, k] : keys)
        if (std::strcmp(event->key, name) == 0)
            return view->key(k, modifiers(*event)) && (dirty = true); // handled: the browser does nothing else with it
    // A typed character: key is the character itself (named keys are longer ASCII words).
    const size_t length = std::strlen(event->key);
    const bool typed = length == 1 || (length > 1 && static_cast<unsigned char>(event->key[0]) >= 0x80);
    if (typed && !event->ctrlKey && !event->metaKey && view->text(event->key)) return dirty = true;
    return false;
}

bool mouse(int type, const EmscriptenMouseEvent *event, void *)
{
    if (!ready) return false;
    const auto pointer = type == EMSCRIPTEN_EVENT_MOUSEDOWN ? gpu::View::Pointer::down
                       : type == EMSCRIPTEN_EVENT_MOUSEUP   ? gpu::View::Pointer::up
                                                            : gpu::View::Pointer::move;
    if (!pointerLocked())
        pointerX = float(event->targetX) * pixelRatio(), pointerY = float(event->targetY) * pixelRatio();
    else if (pointer == gpu::View::Pointer::move)
        pointerX += float(event->movementX) * pixelRatio(), pointerY += float(event->movementY) * pixelRatio();
    // Handling the press stops the browser focusing this page, so do it here: keys come to the focused page.
    if (pointer == gpu::View::Pointer::down) EM_ASM(window.focus());
    view->pointer({pointer, pointerX, pointerY, modifiers(*event)});
    dirty = true;
    return true;
}

void start()
{
    view->prepare(context.device, context.format); // sends "ready"
    ready = true;
    emscripten_set_mousedown_callback("#canvas", nullptr, true, mouse);
    // Moves and releases anywhere in the page, so a drag can leave the canvas.
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, mouse);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, mouse);
    emscripten_set_wheel_callback("#canvas", nullptr, true, wheel);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, true, key);
    emscripten_request_animation_frame_loop(frame, nullptr);
}

}

// The ASSETS folder (gpu_add_page) is built into gui.wasm under /assets.
std::string gpu::loadAsset(const char *path)
{
    std::ifstream file(std::string("/assets") + path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

extern "C" EMSCRIPTEN_KEEPALIVE int gpuPaste(const char *utf8)
{
    return ready && view->text(utf8) && (dirty = true);
}

// A screen reader's request, from the mirror: id as decimal text.
extern "C" EMSCRIPTEN_KEEPALIVE void gpuAccessAction(const char *id, int action, double value)
{
    if (!ready) return;
    view->accessAction(std::strtoull(id, nullptr, 10), gpu::Accessible::Action(action), value);
    dirty = true;
}

// Files dragged over the canvas (no names until they're dropped: the browser
// keeps them back), dropped (written to /drop; paths one per line), or gone.
extern "C" EMSCRIPTEN_KEEPALIVE int gpuFiles(int type, float x, float y, const char *paths)
{
    if (!ready) return 0;
    std::vector<std::string> list;
    std::istringstream lines(paths);
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) list.push_back(line);
    dirty = true;
    return view->files(gpu::View::FileDrag(type), x * pixelRatio(), y * pixelRatio(), list);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gpuReceive(const unsigned char *data, int size)
{
    if (auto message = core::decode(data, size); message && view) view->receive(*message), dirty = true;
}

int main()
{
    view = gpu::createView();
    view->send = [](const core::Value &message) {
        const auto bytes = core::encode(message);
        postToPlugin(bytes.data(), int(bytes.size()));
    };
    view->lockPointer = [](bool locked) { setPointerLock(locked); };
    view->copyText = [](const std::string &utf8) { copyToClipboard(utf8.data(), int(utf8.size())); };
    view->setCursor = [](gpu::View::Cursor cursor) {
        using Cursor = gpu::View::Cursor;
        switch (cursor) // as the macOS presenter's NSCursors
        {
        case Cursor::arrow: setCursorCss("default"); break;
        case Cursor::text: setCursorCss("text"); break;
        case Cursor::hand: setCursorCss("pointer"); break;
        case Cursor::move: setCursorCss("grab"); break;
        case Cursor::resizeLeftRight: setCursorCss("ew-resize"); break;
        case Cursor::resizeUpDown: setCursorCss("ns-resize"); break;
        case Cursor::resizeDiagonal: setCursorCss("nwse-resize"); break;
        }
    };
    EM_ASM({
        window.addEventListener('message', event => {
            if (!(event.data instanceof ArrayBuffer)) return;
            const bytes = new Uint8Array(event.data);
            const pointer = _malloc(bytes.length);
            HEAPU8.set(bytes, pointer);
            _gpuReceive(pointer, bytes.length);
            _free(pointer);
        });
        // Files from the desktop: over and leave as they move, then on the
        // drop each is written to /drop/<name> (MEMFS, in the page's memory)
        // and the paths given to the View.
        const canvas = document.getElementById('canvas');
        const files = (type, event, paths) => {
            const bytes = new TextEncoder().encode(paths.join('\n') + '\0');
            const pointer = _malloc(bytes.length);
            HEAPU8.set(bytes, pointer);
            const taken = _gpuFiles(type, event.offsetX, event.offsetY, pointer);
            _free(pointer);
            return taken;
        };
        const hasFiles = event => event.dataTransfer && Array.from(event.dataTransfer.types).includes('Files');
        canvas.addEventListener('dragover', event => {
            if (!hasFiles(event)) return;
            event.preventDefault();
            event.dataTransfer.dropEffect = files(0, event, []) ? 'copy' : 'none';
        });
        canvas.addEventListener('dragleave', event => { if (hasFiles(event)) files(2, event, []); });
        canvas.addEventListener('drop', async event => {
            if (!hasFiles(event)) return;
            event.preventDefault();
            const dropped = Array.from(event.dataTransfer.files);
            const at = {offsetX: event.offsetX, offsetY: event.offsetY};
            try { FS.mkdirTree('/drop'); } catch {}
            const paths = [];
            for (const file of dropped)
            {
                const path = '/drop/' + file.name.split('/').join('_');
                FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
                paths.push(path);
            }
            files(1, at, paths);
        });
        document.addEventListener('paste', event => {
            const text = event.clipboardData && event.clipboardData.getData('text/plain');
            if (!text) return;
            const bytes = new TextEncoder().encode(text + '\0');
            const pointer = _malloc(bytes.length);
            HEAPU8.set(bytes, pointer);
            if (_gpuPaste(pointer)) event.preventDefault();
            _free(pointer);
        });
    });

    context.instance = wgpu::CreateInstance();
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas;
    canvas.selector = "#canvas";
    wgpu::SurfaceDescriptor surfaceDescriptor;
    surfaceDescriptor.nextInChain = &canvas;
    context.surface = context.instance.CreateSurface(&surfaceDescriptor);

    // The browser answers asynchronously: adapter, then device, then start().
    wgpu::RequestAdapterOptions options;
    options.compatibleSurface = context.surface;
    context.instance.RequestAdapter(&options, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            if (status != wgpu::RequestAdapterStatus::Success)
            {
                emscripten_log(EM_LOG_ERROR, "No WebGPU adapter: %.*s", int(message.length), message.data);
                return;
            }
            context.adapter = std::move(adapter);
            context.adapter.RequestDevice(nullptr, wgpu::CallbackMode::AllowSpontaneous,
                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                    if (status != wgpu::RequestDeviceStatus::Success)
                    {
                        emscripten_log(EM_LOG_ERROR, "No WebGPU device: %.*s", int(message.length), message.data);
                        return;
                    }
                    context.device = std::move(device);
                    context.configure(1, 1);
                    start();
                });
        });
}
