# Headless Gain

Gain and pan with no interface of its own: hosts show their own controls for
its parameters. The smallest starting point.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, parameters and state.

## Build

```sh
cmake -B build && cmake --build build --config Release
```

That builds CLAP, VST3, AU (macOS) and a standalone app. It needs CMake 3.28+
and a C++20 compiler (on Linux also `libasound2-dev`). `--config Release` is for
Windows, where Visual Studio would build Debug; elsewhere the build is Release
already. The plugins land here:

| | CLAP | VST3 | Standalone | AU |
|---|---|---|---|---|
| macOS | `build/Headless Gain.clap` | `build/Headless Gain.vst3` | `build/Headless Gain.app` | `build/Headless Gain.component` |
| Windows | `build/CLAP/Release/Headless Gain.clap` | `build/Release/Headless Gain.vst3` | `build/Release/Headless Gain.exe` | |
| Linux | `build/Headless Gain.clap` | `build/Release/Headless Gain.vst3` | `build/Headless Gain` | |

**WCLAP** (for browser hosts) needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/HeadlessGain.wclap.tar.gz
```

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/Headless Gain.clap"                       # CLAP
pluginval --validate "build/Headless Gain.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aufx HdGn ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
