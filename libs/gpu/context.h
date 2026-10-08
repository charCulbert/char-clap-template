#pragma once
#include "view.h"

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

    // Natively this blocks until the device exists; it returns false without one.
    bool createDevice();
    // Picks a format for the surface and configures it at width x height pixels.
    void configure(uint32_t width, uint32_t height);
    // Unless the view says nothing changed (View::record): clears, lets the
    // view draw, and presents. Reconfigures a lost surface.
    // damaged: the system lost what's on screen, so draw even if the view
    // looks just as it did.
    void frame(View &view, float scale, bool damaged = false);
};

}
