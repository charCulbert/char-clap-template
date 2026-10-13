# Effect Headless

An effect: stereo gain and pan (Gain, Pan), with no interface of its own. Hosts show their own controls for its parameters. The smallest starting point.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin, in the same sections as every
  starter's: descriptor, audio ports, note ports, parameters, recalled
  settings, latency and tail, process, state, and the UI connection. Every
  extension is there, with a comment on the unused ones saying what goes
  in them, so turning it into another kind of plugin means filling in
  bodies.

## Build

```sh
cmake -B build && cmake --build build --config Release
```

That builds CLAP, VST3, AU (macOS) and a standalone app. It needs CMake 3.28+
and a C++20 compiler (on Linux also `libasound2-dev` and `libx11-dev`). `--config Release` is for
Windows, where Visual Studio would build Debug; elsewhere the build is Release
already. The plugins land here:

| | CLAP | VST3 | Standalone | AU |
|---|---|---|---|---|
| macOS | `build/Effect Headless.clap` | `build/Effect Headless.vst3` | `build/Effect Headless.app` | `build/Effect Headless.component` |
| Windows | `build/CLAP/Release/Effect Headless.clap` | `build/Release/Effect Headless.vst3` | `build/Release/Effect Headless.exe` | |
| Linux | `build/Effect Headless.clap` | `build/Release/Effect Headless.vst3` | `build/Effect Headless` | |

**WCLAP** (for browser hosts) needs the [WASI SDK 33](https://github.com/WebAssembly/wasi-sdk/releases):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/EffectHeadless.wclap.tar.gz
```

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/Effect Headless.clap"                       # CLAP
pluginval --validate "build/Effect Headless.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aufx EfHd ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
