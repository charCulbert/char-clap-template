# CLAP plugin starters

Four small CLAP plugins, each a starting point for a different kind of
interface, plus the shared code they build on and a script that copies one
into a new project of your own. Every plugin also builds as VST3, AU,
standalone and WCLAP (for browser hosts), from the same source.

| Example | What it is | Interface |
|---|---|---|
| [`examples/headless-gain`](examples/headless-gain) | Gain and pan | None: the host shows its own controls |
| [`examples/webview-synth`](examples/webview-synth) | Polyphonic sine synth | A web page (HTML, CSS, JS) |
| [`examples/gpu-analyzer`](examples/gpu-analyzer) | Spectrum analyzer | Our own WebGPU drawing (`libs/draw2d`) |
| [`examples/imgui-filter`](examples/imgui-filter) | Lowpass/bandpass/highpass filter | Dear ImGui on WebGPU |

Pick the one closest to what you want to make. The interfaces aren't tied to
the plugin types: a synth with an ImGui interface is the synth's `plugin.cpp`
with the filter's `ui/`.

## Start a new plugin

```sh
python3 tools/new-plugin.py --from filter --name "Warm Filter" --vendor "Charlie Culbert" \
    --dest ~/Development/warm-filter
```

`--from` is `gain`, `synth`, `analyzer` or `filter`. Leave any option out and it
asks. It also takes `--id` (default `com.<vendor>.<name>`), `--au-subtype` and
`--au-manufacturer` (4 letters each, made from the name and vendor if you don't
give them), `--url`, and `--license` (MIT unless you say `none`).

What you get in `--dest`:

```
warm-filter/
  plugin.cpp, entry.cpp, ui/, resources/, ...   the example, renamed to your plugin
  libs/                                          only the libraries it uses
  CMakeLists.txt                                 builds it (libs/ found beside it)
  .github/workflows/build.yml                    CI, runs on every push
  .vscode/tasks.json                             Build, and Run in clap-host (if clap-host is installed)
  README.md, LICENSE, THIRD_PARTY_NOTICES.md, .gitignore
```

It's yours from then on: nothing links back to this repository. It doesn't
make a git repository either; when you want one:

```sh
cd ~/Development/warm-filter
cmake -B build && cmake --build build        # check it builds
git init && git add -A && git commit -m "Start Warm Filter"
gh repo create warm-filter --private --source . --push     # or make it on github.com
```

Once it's on GitHub, every push runs its workflow (see [CI](#ci)).

## What's where

```
examples/<name>/
  plugin.cpp       the whole plugin: descriptor, ports, parameters, state, GUI
                   extensions, audio processing. Start reading here.
  entry.cpp        the CLAP entry point: the one function a host looks up,
                   which hands it plugin.cpp's plugin
  ui/              the interface's C++ (GPU examples)
  resources/       files the plugin ships with it (see below)
  chardsp_*.h      the DSP (analyzer, filter)
  CMakeLists.txt   builds it
libs/
  core/            what every plugin uses: the CLAP SDK and clap-wrapper
                   (pinned in core.cmake), the message codec for interface
                   messages and saved state, and the reader for resources/
  webview/         shows a web page: the host shows it itself if it can
                   (clap.webview), otherwise a WebView in the host's window
  gpu/             shows a WebGPU view: Dawn draws into the host's window
                   natively; in WCLAP the same view is compiled to a web page
  draw2d/          our own drawing for a GPU view: shapes, text, knobs,
                   sliders, and Clay for layout
tests/             tests for the libraries and the analyzer's DSP
tools/             new-plugin.py, check-templates.py
.github/workflows/ one workflow per example, plus tests.yml
```

Each `libs/*/*.cmake` file says at the top what it does. They download and
pin their own dependencies, and give you a function that adds the library to
your plugin (`core_add_to`, `webview_add_to`, `gpu_add_to`, `draw2d_add_to`).

### How the interface talks to the plugin

The interface never touches the plugin directly. It sends small messages
(maps of values, encoded as CBOR by `libs/core/messages.h`, or
`libs/webview/js/messages.js` in a page):

- the interface says `{type: "ready"}`; the plugin answers with its parameters
  (`params`) and their values (`values`), and sends `values` again whenever the
  host changes one
- a drag sends `begin`, then `change` with each new value, then `end`, so the
  host records one automation gesture

The same messages travel over `clap.webview`, whether the interface is a page
or a GPU view, native or WCLAP. Each `plugin.cpp` lists them above its
`PluginReceiveMessage`.

### Resources

Anything a plugin needs at run time goes in its `resources/` folder: a web
page (in `resources/page/`, which the host loads as `/page/index.html`), fonts,
images, sounds. The build copies the folder into every format:

| Format | Where it ends up |
|---|---|
| macOS bundles, VST3 (every OS) | `<plugin>/Contents/Resources/` |
| CLAP on Windows and Linux (a single file) | `<plugin>.clap.resources/` beside it |
| WCLAP | `resources/` inside the archive |

Read a file with `core::readResource("/sounds/kick.wav")` on the main thread
(in `init` or `activate`, never while processing audio). GPU views can use
`gpu::loadAsset("/fonts/Inter.ttf")`.

### Things you'll change first

- **Name, IDs, version**: `pluginDescriptor` in `plugin.cpp`, and
  `make_clapfirst_plugins(...)` and `project(... VERSION ...)` in `CMakeLists.txt`.
  (The generator does this for you.)
- **Parameters**: the `P_*` defines and `extensionParams.get_info` in `plugin.cpp`.
- **Window size and resizing**: `guiWidth`/`guiHeight` in `MyPlugin`, the
  `GUI_MIN_*`/`GUI_MAX_*` defines, and `extensionGui.get_resize_hints` in
  `plugin.cpp`. How the content fills the window is up to the interface (the
  page's CSS, Clay in the analyzer, ImGui in the filter).
- **Sound**: `PluginRenderAudio` in `plugin.cpp`.

## Build

You need CMake 3.28 or newer and a C++20 compiler: Xcode on macOS, Visual
Studio 2022 on Windows, GCC or Clang on Linux. On Linux, also:

```sh
sudo apt install libasound2-dev libx11-dev libgtk-3-dev libwebkit2gtk-4.1-dev
```

(`libgtk-3-dev` and `libwebkit2gtk-4.1-dev` are for the web page, `libx11-dev`
for the GPU examples.)

### The usual formats

In an example's folder (or your own project):

```sh
cmake -B build && cmake --build build
```

That builds CLAP, VST3, a standalone app, and AU on macOS, into `build/`.
The first build downloads the dependencies (roughly 65 to 100 MB) and takes a
few minutes; after that it only rebuilds what changed. To build one format:
`cmake --build build --target WarmFilter_clap` (or `_vst3`, `_auv2`,
`_standalone`).

To try them in a DAW, copy them where hosts look:

| | CLAP | VST3 | AU |
|---|---|---|---|
| macOS | `~/Library/Audio/Plug-Ins/CLAP/` | `~/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/Components/` |
| Windows | `C:\Program Files\Common Files\CLAP\` | `C:\Program Files\Common Files\VST3\` | |
| Linux | `~/.clap/` | `~/.vst3/` | |

On Windows and Linux copy the `.clap.resources` folder along with the `.clap`.
After copying an AU, `killall -9 AudioComponentRegistrar` makes macOS notice it.

### Other formats

Each needs one extra flag, in its own build folder:

| Format | Needs | Configure |
|---|---|---|
| WCLAP | [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases); for the GPU examples also [Emscripten](https://emscripten.org) (`emcmake` on your PATH) | `cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake` |
| AUv3 (macOS) | Xcode | `cmake -B build-xcode -G Xcode` |
| iOS (AUv3 in an app) | Xcode | `cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS` |
| AAX | Avid's AAX SDK | `cmake -B build-aax -DAAX_SDK_ROOT=<path to the SDK>` |

Then `cmake --build <folder>` as usual (add `--config Release` for the Xcode
builds). A WCLAP comes out as `build-wclap/<Name>.wclap.tar.gz`: load that in a
browser host. Only the WebView Synth sets up AUv3, iOS and AAX so far; copy its
`CMakeLists.txt` lines to add them to another plugin. Signing an iOS app for a
device needs your team in Xcode, and shipping AAX needs PACE signing.

### Everything at once

At the top of this repository, `cmake -B build && cmake --build build` builds all
four examples and the tests in one go (downloading once), and
`ctest --test-dir build` runs the tests.

## Test

The same checks CI runs, on your own machine:

```sh
clap-validator validate build/Warm\ Filter.clap                  # CLAP
pluginval --validate build/Warm\ Filter.vst3 --strictness-level 5   # VST3, opens the GUI too
auval -v aufx WaFi ChCu                                         # AU: type, subtype, manufacturer
```

`clap-host` (from `~/Development/nativeClapHost`) loads, plays, validates and
screenshots a CLAP from the shell: `clap-host plugin.clap -- validate.run`.

After changing an example, a library or the generator, run

```sh
python3 tools/check-templates.py          # or just some: ... filter gain
```

It makes a project from each example in `build-templates/`, builds it with
plain CMake, and validates it. It takes a while (every project downloads and
builds from scratch), so it isn't part of CI.

## CI

Each example has its own workflow in `.github/workflows/`, which runs only
when that example, a library it uses, or the workflow itself changes. Editing
`libs/gpu` runs the analyzer's and the filter's; editing
`examples/webview-synth` runs only the synth's. `tests.yml` runs the unit
tests when the libraries they cover change.

A workflow run has two jobs:

- **native**, on macOS, Windows and Linux: builds CLAP, VST3 and (macOS) AU,
  checks them with clap-validator, pluginval (including opening the GUI) and
  auval, then uploads them. If any check fails, nothing is uploaded.
- **wclap**, on Linux: builds the WCLAP and checks the archive's contents.

The results are on the run's page under Artifacts. In a project made with
`new-plugin.py` the workflow is `.github/workflows/build.yml` and runs on every
push, since the whole repository is the plugin. You can also start any
workflow by hand from GitHub's Actions tab.

## Updating dependencies

Every version is pinned in one place:

| Dependency | Pinned in | Now |
|---|---|---|
| CLAP SDK | `libs/core/core.cmake` | 1.2.10 |
| clap-wrapper | `libs/core/core.cmake` | a commit on its `next` branch (needed for the AUv2 fixes in its PR #536) |
| CHOC | `libs/webview/webview.cmake` | a commit |
| Dawn | `libs/gpu/gpu.cmake` | release v20260922.191850 |
| stb_truetype, Clay | `libs/draw2d/draw2d.cmake` | a commit, v0.14 |
| Dear ImGui | `examples/imgui-filter/CMakeLists.txt` | v1.92.9b |
| Signalsmith DSP (FFT) | `examples/gpu-analyzer/signalsmith-dsp.cmake` | a commit |

Downloads are checked against a SHA256 where CMake allows it (Dawn, stb, Clay,
ImGui, Signalsmith DSP). When you change a pin, change the hash too, then run the check above.

## Licences

The dependencies are all permissively licensed: CLAP SDK and clap-wrapper
(MIT), CHOC (ISC), Dawn (BSD 3-Clause), stb_truetype (MIT or public domain),
Clay (zlib), Dear ImGui (MIT), Signalsmith DSP (MIT), and the Inter font (SIL Open Font License, its
licence beside it in `resources/fonts/`). The VST3 and AudioUnit SDKs that
clap-wrapper downloads have their own terms. Projects made with
`new-plugin.py` get a `THIRD_PARTY_NOTICES.md` listing the ones they use.
