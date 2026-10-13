# CLAP plugin starters

CLAP plugins to start new ones from: an effect and an instrument, each with
a web page or a chardraw interface, and an effect with none. Each builds, with
[clap-wrapper](https://github.com/free-audio/clap-wrapper), from one source to
the same targets, all built in CI:

- **CLAP, VST3 and a standalone app**, on macOS, Windows and Linux
- **AUv2**, on macOS
- **WCLAP**, for browser hosts

The web page starters also set up **the iOS app with its AUv3** (not built in
CI). The chardraw ones get it next: it needs an iOS backend in `libs/gpu`.

| Starter | Type | Interface |
|---|---|---|
| [`examples/effect-headless`](examples/effect-headless) | Gain and pan | None: the host shows its own controls |
| [`examples/effect-webview`](examples/effect-webview) | Gain and pan, peak meter | Web page |
| [`examples/effect-chardraw`](examples/effect-chardraw) | Gain and pan, peak meter | chardraw on WebGPU |
| [`examples/instrument-webview`](examples/instrument-webview) | Polyphonic saw synth with a lowpass filter, peak meter | Web page |
| [`examples/instrument-chardraw`](examples/instrument-chardraw) | Polyphonic saw synth with a lowpass filter, peak meter | chardraw on WebGPU |

Every starter's `plugin.cpp` is raw CLAP, written out, with the same sections
in the same order and the same names: descriptor and features, audio ports,
note ports, parameters, recalled settings, latency and tail, process (events,
then DSP), state, and the UI connection. Compare two and only the real
differences show: an effect and an instrument differ in their ports, DSP and
parameters; a web page and a chardraw starter only in the UI connection.
Every starter has every extension, the unused ones returning 0 or false with
a comment saying what goes there, so one kind of plugin becomes another by
filling in bodies.


## Getting started

### Make a plugin

```sh
python3 tools/new-plugin.py --from instrument-chardraw --name "My Plugin" --vendor "Your Name" \
    --dest ../my-plugin
```

`--from` is the starter's folder in `examples/`. Leave an option out and it asks. It also
takes `--id`, `--au-subtype`, `--au-manufacturer`, `--url`, `--license`
(MIT unless `none`) and `--template-version` (the newest release tag
unless given; from a GitHub archive download, which has no tags, the version
in the folder's name, like `char-clap-template-0.1.0`, else you must pass it).
It needs Python 3.8 or later (`py` on Windows).

You get the example renamed to your plugin, a CI workflow, a LICENSE and
THIRD_PARTY_NOTICES.md. Its `CMakeLists.txt` fetches this repository's
`libs/` at a release tag (see
[Updating the template](#updating-the-template)).
Make it from a checkout of that tag, so the example matches the libraries; it
warns when you don't.

```sh
cd ../my-plugin
cmake -B build && cmake --build build --config Release
git init && git add -A && git commit -m "Start My Plugin"
```

### Build

You need CMake 3.28 or newer and a C++20 compiler (Xcode, Visual Studio 2022,
or GCC or Clang). On Linux, also:

```sh
sudo apt install libasound2-dev libx11-dev libxrandr-dev
```

A web page interface needs no GTK or WebKit to build. To show the page, the
machine running the plugin needs WebKitGTK (`libwebkit2gtk-4.1-0` on Debian
and Ubuntu) and glibc: the page runs in a helper process, started through
glibc's dynamic loader. Without WebKitGTK the plugin still loads, with no
interface.

`cmake -B build && cmake --build build --config Release` builds CLAP, VST3,
the standalone app, and AU on macOS, into `build/` (on Windows into
`build/CLAP/Release/` and `build/Release/`, and the VST3 on Linux into
`build/Release/`). The first build downloads its
dependencies and takes a few minutes. On Windows and Linux, copy the
`.clap.resources` folder along with the `.clap`.

### Where to edit

All in `plugin.cpp`, unless it says otherwise:

- **DSP:** `PluginRenderAudio`, and `PluginNoteOn` and `PluginNoteOff` for
  notes.
- **Parameters:** the `P_*` defines and `extensionParams`.
- **Window size:** `guiWidth`, `guiHeight`, and the `GUI_MIN_*` and `GUI_MAX_*`
  defines under Recalled settings.
- **Interface:** `ui/` for a chardraw interface, `resources/page/` for a web
  page.
- **Instrument or effect:** start from the right one. To switch, change all of:
  - `pluginFeatures`: `CLAP_PLUGIN_FEATURE_INSTRUMENT` or `AUDIO_EFFECT`.
  - The port counts: an instrument has a note input and no audio input; the
    comments in `extensionAudioPorts` and `extensionNotePorts` say what to
    return.
  - `PluginNoteOn`, `PluginNoteOff` and `PluginEndFinishedVoices`: empty in an
    effect.
  - The AU type, `aumu` or `aufx`: `AUV2_INSTRUMENT_TYPE` in
    `CMakeLists.txt`, and `AU:` in your project's `.github/workflows/build.yml`
    (here, the starter's own workflow, `.github/workflows/<starter>.yml`).

## Updating the template

Your plugin's `CMakeLists.txt` fetches this repository's `libs/` at a release
tag (`GIT_TAG v0.2.0`). To update, read the
[release notes](https://github.com/charCulbert/char-clap-template/releases)
from your version up, then change the tag. Before 1.0, a minor bump (0.1 to
0.2) may need changes in your plugin; a patch bump won't.

To work on the template and a plugin together, build the plugin against your
checkout:

```sh
cmake -B build -DFETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE=/abs/path/to/char-clap-template
```

## What's where

```
examples/<name>/
  plugin.cpp        the plugin: descriptor, ports, parameters, state, GUI, audio
  entry.cpp         the CLAP entry point
  ui/               the interface's C++ (chardraw starters)
  resources/        files shipped with the plugin: page/, fonts/, ...
  CMakeLists.txt
libs/
  core/             CLAP SDK, clap-wrapper, the message codec, resources
  webview/          a web page interface
  gpu/              a WebGPU interface: Dawn natively, a web page in WCLAP
  chardraw/         drawing, layout and controls for a GPU interface
tests/              unit tests
tools/              new-plugin.py, check-templates.py
```

Each `libs/*/*.cmake` says at the top what it does, pins its own
dependencies, and gives you a function to add it to a plugin (`core_add_to`,
`webview_add_to`, `gpu_add_to`, `chardraw_add_to`).

## chardraw

A small immediate-mode GUI lib for a WebGPU interface, the same code natively and
in the browser:

- **Drawing:** shapes, text, images and SVG, gradients, shadows, clipping.
- **Layout:** [Clay](https://github.com/nicbarker/clay), with hit-testing,
  scrolling and popups.
- **Controls:** knobs, sliders, number boxes, buttons, toggles, menus and
  context menus, text fields, drawers, dockable windows.
- **Views:** waveform, scope, spectrum and spectrogram (`views.h`).
- **Themes:** light and dark.
- **Screen readers:** every control is in an accessibility tree, given to
  VoiceOver, Narrator, NVDA and Orca through AccessKit, and to browsers as
  ARIA elements over the canvas.

A knob, slider, number box or readout for a parameter takes a `Ui::Format`:
a printf format as before, or a function that writes the value, with an
optional short label shown in place of the parameter's name. Screen readers
still hear the full name. `gpu::Params::find` takes a name or a CLAP id.

```cpp
ui.knob (params, params.find (kAttack), { msOrSeconds, "A" }); // "5 ms", "1.20 s"
ui.knob (params, ratio, { "%.2f", "Ratio" });
```

`ui.piano` returns its notes from the call that declares it. A tap shorter
than a frame owes its note-off to the next call, and the piano asks for that
frame itself.

Linux GPU builds are x86_64 only: Dawn's and AccessKit's prebuilt libraries
have no other Linux architecture.

The chardraw Gallery ([`examples/chardraw-gallery`](examples/chardraw-gallery)) shows every feature, a page per area. It's
a showcase, not a starting point: `new-plugin.py` doesn't offer it.

## How the interface talks to the plugin

The interface sends the plugin small CBOR messages (`libs/core/messages.h`,
or `libs/webview/js/messages.js` in a page): `ready`, then `begin`, `change`
and `end` around each drag; the plugin answers with its `params` and
`values`, and the starters' peak meter gets a `meter` message about 30 times
a second. Each `plugin.cpp` lists its messages above `PluginReceiveMessage`.

How they get there depends on the host:

- **A host that shows the page itself**, over the `clap.webview` extension:
  every WCLAP host (a GPU interface is a page there too), and natively, for a
  web page interface, a host that supports it.
- **Otherwise, natively:** the plugin opens the interface in the host's window
  itself (a WebView, or Dawn for a GPU interface) and passes the messages
  straight to it.


## Resources

Files the plugin needs at run time go in its `resources/` folder: its page in
`page/`, fonts, images, sounds. The build copies it into each format:
`Contents/Resources/` in a bundle, `<plugin>.clap.resources/` beside a single
`.clap`, and the archive's root in a WCLAP. Read one with
`core::readResource("/sounds/kick.wav")` on the main thread, or
`gpu::loadAsset("/fonts/Inter.ttf")` in a GPU interface.

## WCLAP and other formats

Each needs its own build folder:

| Format | Needs | Configure |
|---|---|---|
| WCLAP | [WASI SDK 33](https://github.com/WebAssembly/wasi-sdk/releases); for GPU interfaces also [Emscripten 4.0.11](https://emscripten.org) | `cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake` |
| iOS app (AUv3), web page starters | Xcode | `cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS` |

A WCLAP comes out as `build-wclap/<Name>.wclap.tar.gz`. Load it in a browser
host: [browser-test-host](https://webclap.github.io/browser-test-host/),
[daw.charlieculbert.com](https://daw.charlieculbert.com) or
[openDAW](https://opendaw.studio).

## Test

```sh
ctest --test-dir build                                       # after building at the top of this repository
clap-validator validate build/My\ Plugin.clap                 # CLAP
pluginval --validate build/My\ Plugin.vst3 --strictness-level 5   # VST3, opens the GUI too
auval -v aufx MyPl YoNa                                      # AU: type, subtype, manufacturer
python3 tools/check-templates.py                             # a project from each starter, built and validated
```

The tests cover the resource reader, the message codec (C++ and JavaScript),
chardraw's layout and controls, and its drawing (checked pixel by pixel;
needs a WebGPU adapter, a software one will do). The chardraw Gallery
(`examples/chardraw-gallery`) is for looking: each cell draws one feature and
says what it should look like.

## CI

Each example has a workflow in `.github/workflows/`, named for its folder, that
runs when the example, a library it uses, or the workflow changes; `tests.yml`
runs the unit tests, and `new-plugin.yml` builds a project made with
`new-plugin.py` from each starter against the checkout. Each run builds the
plugins and the standalone app on macOS, Windows and Linux, checks the
plugins with clap-validator, pluginval and auval, builds the WCLAP, and
uploads the plugins under the run's Artifacts. A project made
with `new-plugin.py` has one workflow, `build.yml`, that runs on pushes to `main` and on
pull requests.

## Updating dependencies

Every version is pinned in one place:

| Dependency | Pinned in | Now |
|---|---|---|
| CLAP SDK | `libs/core/core.cmake` | 1.2.10 |
| clap-wrapper | `libs/core/core.cmake` | a commit on its `next` branch (for the AUv2 fixes in its PR #536) |
| CHOC | `libs/webview/webview.cmake` | a commit |
| Dawn | `libs/gpu/gpu.cmake` | v20260922.191850 |
| AccessKit | `libs/gpu/gpu.cmake` | 0.23.1 |
| stb, Clay, nanosvg | `libs/chardraw/chardraw.cmake` | commits |
| Emscripten | each chardraw example's workflow; a licence in `libs/gpu/licenses/` | 4.0.11 |
| WASI SDK | each workflow; its runtime's licences in `libs/core/licenses/wasi-sdk/` | 33 |

Downloads are checked against a SHA256 where CMake allows it. When you change
a pin, change its hash, then run `tools/check-templates.py`.

## Licences

This repository is MIT. Its dependencies are permissive: CLAP SDK and
clap-wrapper (MIT), CHOC (ISC), Dawn (BSD 3-Clause), AccessKit (MIT or
Apache-2.0, with a BSD 3-Clause notice from Chromium), stb (MIT or public
domain), Clay and nanosvg (zlib), Emscripten's runtime and emdawnwebgpu (MIT
or NCSA; BSD 3-Clause; musl and libc++), and the Inter, IBM Plex Mono and
Barlow Semi Condensed fonts (SIL Open Font License,
each beside its font). A WCLAP links the WASI SDK's runtime: wasi-libc,
libc++, libc++abi, the LLVM runtime and dlmalloc (licences in
`libs/core/licenses/wasi-sdk/`). clap-wrapper also downloads the VST3 SDK
(MIT), RtAudio and RtMidi (MIT-style), and on macOS the AudioUnit SDK
(Apache-2.0) and ghc::filesystem (MIT).

chardraw's colour schemes (`libs/chardraw/views.h`) are sampled from
matplotlib's data: Magma, Viridis, Inferno and Plasma by Nathaniel Smith and
Stéfan van der Walt (CC0); Cividis by Jamie Nuñez, Christopher Anderton and
Ryan Renslow (Copyright (c) 2017, Battelle Memorial Institute, BSD-style, from
[pnnl/cmaputil](https://github.com/pnnl/cmaputil)); and Turbo by Anton
Mikhailov (Copyright 2019 Google LLC, Apache-2.0). The last two's licences
are in `libs/chardraw/licenses/`.

The examples' `plugin.cpp` follows the layout of nakst's
[CLAP tutorial](https://nakst.gitlab.io/tutorial/clap-part-1.html).

Every build copies the licences of what it downloaded into
`THIRD_PARTY_LICENSES/` beside the plugin, and a project made with
`new-plugin.py` gets a `THIRD_PARTY_NOTICES.md` listing what it uses.
AccessKit's release lists no licences for the Rust crates in its libraries:
its `LICENSE-MIT` and `LICENSE.chromium` are all that ships for it.
