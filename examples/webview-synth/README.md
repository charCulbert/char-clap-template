# WebView Synth

A polyphonic sine synth (Volume, Release) whose interface is a web page.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: note input (CLAP notes or MIDI), voices,
  parameters, state, and the `clap.gui` and `clap.webview` extensions.
- `resources/`: the files the plugin ships. Its page is in `resources/page/`
  (`index.html`, `app.js`, `style.css`); it talks to the plugin with
  `page/lib/messages.js`, which the build adds from `libs/webview/js/`. Add
  images, sounds or anything else beside it and read them with
  `core::readResource("/path")` (on the main thread).
- Hosts that support `clap.webview` (like WCLAP browser hosts) show the page
  themselves; elsewhere `libs/webview` shows it in a WebView (CHOC) inside the
  host's window. On iOS there is no interface yet.

## Build

```sh
cmake -B build && cmake --build build
```

That builds CLAP, VST3, AU (macOS) and a standalone app into `build/`. It needs CMake 3.28+ and a C++20 compiler (on Linux also `libgtk-3-dev` and `libwebkit2gtk-4.1-dev`).

**WCLAP** (for browser hosts) needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/WebViewSynth.wclap.tar.gz
```

**More formats**, each with one extra flag:

| Format | Configure with |
|---|---|
| AUv3 (macOS) | `cmake -B build-xcode -G Xcode` |
| iOS app with AUv3 | `cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS` (signing for a device needs your team in Xcode) |
| AAX | `cmake -B build-aax -DAAX_SDK_ROOT=<your AAX SDK>` (the SDK comes from Avid; shipping needs PACE signing) |

## Test

The workflow in `.github/workflows/` builds every push on macOS, Windows and
Linux, checks the plugins with clap-validator, pluginval and (macOS) auval,
and uploads them only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
