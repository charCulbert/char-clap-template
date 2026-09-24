# GPU Analyzer

A spectrum analyzer: it passes audio through and shows its spectrum, drawn with
WebGPU by our own drawing code.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, state, the spectrum
  analysis hand-off, and the `clap.gui` and `clap.webview` extensions.
- `chardsp_Spectrum.h`: the analysis, after Signalsmith's analyser.
- `ui/AnalyzerView.cpp`: the interface, a `gpu::View`: one fragment shader for
  the spectrum, and knobs and labels from `libs/draw2d`, laid out with Clay.
- Smoothing and Colour are settings saved in the plugin's state, not CLAP
  parameters, since they only change the display.
- `resources/`: the files the plugin ships (its font). Add images, sounds or
  anything else here and read them with `core::readResource("/path")` (on the
  main thread), or `gpu::loadAsset` in the view.
- Natively `libs/gpu` draws the view with Dawn in the host's window. In WCLAP
  the same view is compiled with Emscripten into the page the host shows.

## Build

```sh
cmake -B build && cmake --build build
```

That builds CLAP, VST3, AU (macOS) and a standalone app into `build/`. It needs CMake 3.28+ and a C++20 compiler (on Linux also `libx11-dev`).

**WCLAP** (for browser hosts) needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases) and [Emscripten](https://emscripten.org) (`emcmake` on your PATH) for the page:

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/GpuAnalyzer.wclap.tar.gz
```

## Test

The workflow in `.github/workflows/` builds every push on macOS, Windows and
Linux, checks the plugins with clap-validator, pluginval and (macOS) auval,
and uploads them only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
