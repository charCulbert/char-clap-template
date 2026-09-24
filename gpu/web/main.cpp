// Runs a gpu::View in a WCLAP host's page: the browser's WebGPU draws into
// #canvas, and messages travel as clap.webview/3 describes (ArrayBuffers via
// window.parent.postMessage, and 'message' events back), encoded like webUI/.
#include "../context.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <fstream>
#include <iterator>

namespace {

gpu::Context context;
std::unique_ptr<gpu::View> view;
bool ready = false;
bool dirty = true; // something changed since the last frame

EM_JS(void, postToPlugin, (const unsigned char *data, int size), {
    window.parent.postMessage(HEAPU8.slice(data, data + size).buffer, '*');
});

float pixelRatio() { return float(emscripten_get_device_pixel_ratio()); }

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
    if (dirty || view->animating()) context.frame(*view, pixelRatio());
    dirty = false;
    return true; // keep the requestAnimationFrame loop going
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
    view->pointer(pointer, pointerX, pointerY);
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
    emscripten_request_animation_frame_loop(frame, nullptr);
}

}

// ui/assets/ is embedded in gui.wasm under /assets (see CMakeLists.txt).
std::string gpu::loadAsset(const char *path)
{
    std::ifstream file(std::string("/assets") + path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

extern "C" EMSCRIPTEN_KEEPALIVE void gpuReceive(const unsigned char *data, int size)
{
    if (auto message = webview::decode(data, size); message && view) view->receive(*message), dirty = true;
}

int main()
{
    view = gpu::createView();
    view->send = [](const webview::Value &message) {
        const auto bytes = webview::encode(message);
        postToPlugin(bytes.data(), int(bytes.size()));
    };
    view->lockPointer = [](bool locked) { setPointerLock(locked); };
    EM_ASM({
        window.addEventListener('message', event => {
            if (!(event.data instanceof ArrayBuffer)) return;
            const bytes = new Uint8Array(event.data);
            const pointer = _malloc(bytes.length);
            HEAPU8.set(bytes, pointer);
            _gpuReceive(pointer, bytes.length);
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
