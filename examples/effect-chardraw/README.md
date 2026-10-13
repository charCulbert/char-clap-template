# Effect chardraw

An effect: stereo gain and pan (Gain, Pan) with a peak meter, laid out as a mixer channel strip. Its interface is drawn with chardraw on WebGPU.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin, in the same sections as every
  starter's: descriptor, audio ports, note ports, parameters, recalled
  settings, latency and tail, process, state, and the UI connection. Every
  extension is there, with a comment on the unused ones saying what goes
  in them, so turning it into another kind of plugin means filling in
  bodies.
- The meter: the audio thread keeps each channel's peak in an atomic, and
  about 30 times a second asks the main thread to send them to the
  interface as a `meter` message. The window's size is a recalled setting:
  saved in the state, not a CLAP parameter.
- `ui/PluginView.cpp`: the interface, a `gpu::View` laid out with Clay in
  chardraw's palette, dark or light as the system is.
- `resources/`: the files the plugin ships (its font). Add images, sounds or
  anything else here and read them with `core::readResource("/path")` (on the
  main thread), or `gpu::loadAsset` in the view.
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
| macOS | `build/Effect chardraw.clap` | `build/Effect chardraw.vst3` | `build/Effect chardraw.app` | `build/Effect chardraw.component` |
| Windows | `build/CLAP/Release/Effect chardraw.clap` | `build/Release/Effect chardraw.vst3` | `build/Release/Effect chardraw.exe` | |
| Linux | `build/Effect chardraw.clap` | `build/Release/Effect chardraw.vst3` | `build/Effect chardraw` | |

**WCLAP** (for browser hosts) needs the [WASI SDK 33](https://github.com/WebAssembly/wasi-sdk/releases) and [Emscripten 4.0.11](https://emscripten.org) (`emcmake` on your PATH) for the page:

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/EffectChardraw.wclap.tar.gz
```

**iOS:** coming next. It needs an iOS backend in `libs/gpu`.

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/Effect chardraw.clap"                       # CLAP
pluginval --validate "build/Effect chardraw.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aufx EfCd ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
