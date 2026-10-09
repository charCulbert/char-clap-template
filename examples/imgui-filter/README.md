# ImGui Filter

A state-variable filter (Type: lowpass, bandpass or highpass; Cutoff;
Resonance) whose interface is stock Dear ImGui widgets.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, parameters, state, and the
  `clap.gui` and `clap.webview` extensions.
- `chardsp_StateVariableFilter.h`: the filter.
- `ui/FilterView.cpp`: the interface, a `gpu::View` drawn with Dear ImGui and
  its WebGPU renderer. Ctrl+click a slider to type a value.
- `resources/`: the files the plugin ships (its font). Add images, sounds or
  anything else here and read them with `core::readResource("/path")` (on the
  main thread), or `gpu::loadAsset` in the view.
- Screen readers see nothing of it: Dear ImGui keeps no accessibility tree. A
  `gpu::View` that wants one returns it from `accessibility()` (as the draw2d
  examples do with `Ui::accessibility()`), and the presenter hands it to
  VoiceOver, Narrator and the browser.
- Natively `libs/gpu` draws the view with Dawn in the host's window. In WCLAP
  the same view is compiled with Emscripten into the page the host shows.

## Build

```sh
cmake -B build && cmake --build build --config Release
```

That builds CLAP, VST3, AU (macOS) and a standalone app. It needs CMake 3.28+
and a C++20 compiler (on Linux also `libasound2-dev`, `libx11-dev` and `libxrandr-dev`). `--config Release` is for
Windows, where Visual Studio would build Debug; elsewhere the build is Release
already. The plugins land here:

| | CLAP | VST3 | Standalone | AU |
|---|---|---|---|---|
| macOS | `build/ImGui Filter.clap` | `build/ImGui Filter.vst3` | `build/ImGui Filter.app` | `build/ImGui Filter.component` |
| Windows | `build/CLAP/Release/ImGui Filter.clap` | `build/Release/ImGui Filter.vst3` | `build/Release/ImGui Filter.exe` | |
| Linux | `build/ImGui Filter.clap` | `build/Release/ImGui Filter.vst3` | `build/ImGui Filter` | |

**WCLAP** (for browser hosts) needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases) and [Emscripten](https://emscripten.org) (`emcmake` on your PATH) for the page:

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/ImGuiFilter.wclap.tar.gz
```

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/ImGui Filter.clap"                       # CLAP
pluginval --validate "build/ImGui Filter.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aufx ImFl ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
