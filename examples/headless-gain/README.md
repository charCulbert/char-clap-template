# Headless Gain

Gain and pan with no interface of its own: hosts show their own controls for
its parameters. The smallest starting point.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, parameters and state.

## Build

```sh
cmake -B build && cmake --build build
```

That builds CLAP, VST3, AU (macOS) and a standalone app into `build/`. It needs CMake 3.28+ and a C++20 compiler.

**WCLAP** (for browser hosts) needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/HeadlessGain.wclap.tar.gz
```

## Test

The workflow in `.github/workflows/` builds every push on macOS, Windows and
Linux, checks the plugins with clap-validator, pluginval and (macOS) auval,
and uploads them only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
