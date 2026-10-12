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
  themselves; elsewhere `libs/webview` shows it in a WebView inside the host's
  window: CHOC's on macOS and Windows, WebKitGTK in a helper process on Linux. On iOS there is no interface yet.

## Build

```sh
cmake -B build && cmake --build build --config Release
```

That builds CLAP, VST3, AU (macOS) and a standalone app. It needs CMake 3.28+
and a C++20 compiler (on Linux also `libasound2-dev` and `libx11-dev`; to show its page, a Linux machine needs `libwebkit2gtk-4.1-0` and glibc). `--config Release` is for
Windows, where Visual Studio would build Debug; elsewhere the build is Release
already. The plugins land here:

| | CLAP | VST3 | Standalone | AU |
|---|---|---|---|---|
| macOS | `build/WebView Synth.clap` | `build/WebView Synth.vst3` | `build/WebView Synth.app` | `build/WebView Synth.component` |
| Windows | `build/CLAP/Release/WebView Synth.clap` | `build/Release/WebView Synth.vst3` | `build/Release/WebView Synth.exe` | |
| Linux | `build/WebView Synth.clap` | `build/Release/WebView Synth.vst3` | `build/WebView Synth` | |

**WCLAP** (for browser hosts) needs the [WASI SDK 33](https://github.com/WebAssembly/wasi-sdk/releases):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/WebViewSynth.wclap.tar.gz
```

**More formats**, each with one extra flag:

| Format | Configure with |
|---|---|
| iOS app with AUv3 | `cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS` (signing for a device needs your team in Xcode) |
| AAX | `cmake -B build-aax -DAAX_SDK_ROOT=<your AAX SDK>` (the SDK comes from Avid; shipping needs PACE signing) |

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/WebView Synth.clap"                       # CLAP
pluginval --validate "build/WebView Synth.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aumu WvSy ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
