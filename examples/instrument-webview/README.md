# Instrument WebView

An instrument: a polyphonic synth (saw, square or triangle) through a lowpass filter (Filter cutoff, Release, Waveform: a stepped parameter), with a peak meter. Its interface is a web page.

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
- `resources/`: the files the plugin ships. Its page is in `resources/page/`
  (`index.html`, `app.js`, `style.css`), in chardraw's palette, dark or
  light as the system is; it talks to the plugin with
  `page/lib/messages.js`, which the build adds from `libs/webview/js/`. Add
  images, sounds or anything else beside it and read them with
  `core::readResource("/path")` (on the main thread).
- Hosts that support `clap.webview` (like WCLAP browser hosts) show the page
  themselves; elsewhere `libs/webview` shows it in a WebView inside the host's
  window: CHOC's on macOS and Windows, WebKitGTK in a helper process on Linux.

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
| macOS | `build/Instrument WebView.clap` | `build/Instrument WebView.vst3` | `build/Instrument WebView.app` | `build/Instrument WebView.component` |
| Windows | `build/CLAP/Release/Instrument WebView.clap` | `build/Release/Instrument WebView.vst3` | `build/Release/Instrument WebView.exe` | |
| Linux | `build/Instrument WebView.clap` | `build/Release/Instrument WebView.vst3` | `build/Instrument WebView` | |

**WCLAP** (for browser hosts) needs the [WASI SDK 33](https://github.com/WebAssembly/wasi-sdk/releases):

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/InstrumentWebView.wclap.tar.gz
```

**The iOS app with its AUv3:** `cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS`
(signing for a device needs your team in Xcode). It isn't built in CI.

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/Instrument WebView.clap"                       # CLAP
pluginval --validate "build/Instrument WebView.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aumu InWv ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
