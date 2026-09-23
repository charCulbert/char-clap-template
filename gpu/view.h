#pragma once
#include <webgpu/webgpu_cpp.h>
#include "../webview/messages.h"
#include <functional>
#include <memory>

// A plugin's GPU-drawn interface. The same View runs natively (gpu/gui.cpp,
// Dawn) and in a WCLAP host's page (gpu/web/main.cpp, the browser's WebGPU).
// Presenters own the device and surface; a View records draw calls, handles
// the pointer, and exchanges the plugin's messages.
namespace gpu {

class View
{
public:
    enum class Pointer { down, move, up };

    virtual ~View() = default;
    virtual void prepare(const wgpu::Device &device, wgpu::TextureFormat format) = 0;
    // Sizes are in pixels; scale is pixels per logical point (for line widths).
    virtual void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float scale) = 0;
    virtual void pointer(Pointer type, float x, float y) = 0; // pixels
    virtual void receive(const webview::Value &message) = 0;
    // Presenters draw only after input, a message or a resize, plus every
    // frame while this is true (e.g. during an animation).
    virtual bool animating() const { return false; }

    // Set by the presenter: delivers a message to the plugin.
    std::function<void(const webview::Value &)> send;
};

// Implemented by the plugin (ui/FilterView.cpp).
std::unique_ptr<View> createView();

}
