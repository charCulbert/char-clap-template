#pragma once
#include "view.h"
#include <atomic>
#include <memory>

// One WebGPU device and surface, and the per-frame steps both presenters share.
namespace gpu {

struct Context
{
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::PresentMode presentMode = wgpu::PresentMode::Fifo; // Mailbox where the surface has it
    uint32_t width = 0, height = 0; // pixels
    // Set when the device is lost (a driver reset, a GPU removed): the
    // presenter then starts again with a new one. Not for our own teardown.
    std::shared_ptr<std::atomic<bool>> lost = std::make_shared<std::atomic<bool>>(false);
    // WebGPU validation errors so far (each is also printed): tests expect none.
    std::shared_ptr<std::atomic<int>> errors = std::make_shared<std::atomic<int>>(0);

    // Natively this blocks until the device exists; it returns false without one.
    bool createDevice();
    // Picks a format for the surface and configures it at width x height pixels.
    void configure(uint32_t width, uint32_t height);
    // Unless the view says nothing changed (View::record): clears, lets the
    // view draw, and presents. Reconfigures a lost surface.
    // damaged: the system lost what's on screen, so draw even if the view
    // looks just as it did.
    // Returns false when it had a frame to show but couldn't (no surface
    // texture this time): the caller tries again with the next one.
    bool frame(View &view, float scale, bool damaged = false);
};

}
