# GPU Spectrogram

A scrolling spectrogram, as Listener's: it passes audio through and shows
how its spectrum changes over time. The interface uses only `libs/draw2d`'s
parts, with no drawing code of its own; for a drawing of your own, see
`examples/gpu-analyzer`.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, state, the hand-off of
  the analysis to the interface, and the `clap.gui` and `clap.webview` extensions.
- `signalsmith-dsp.cmake`: pins Signalsmith Audio's DSP headers, for their FFT.
- `chardsp_Stft.h`: the analysis, a short-time Fourier transform as Listener's
  default: a Hann-windowed 2048-sample FFT every 512 samples, in dB relative to
  a full-scale sine. The audio thread only copies samples into a ring.
- `ui/SpectrogramView.cpp`: the interface, a `gpu::View`: a `gpu::Spectrogram`
  (`draw2d/views.h`) filling a box that draws itself (`Ui::paint`), and Ui
  controls above it, laid out with Clay.
- Palette, Scale and Floor are settings saved in the plugin's state, not CLAP
  parameters, since they only change the display.
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
and a C++20 compiler (on Linux also `libasound2-dev` and `libx11-dev`). `--config Release` is for
Windows, where Visual Studio would build Debug; elsewhere the build is Release
already. The plugins land here:

| | CLAP | VST3 | Standalone | AU |
|---|---|---|---|---|
| macOS | `build/GPU Spectrogram.clap` | `build/GPU Spectrogram.vst3` | `build/GPU Spectrogram.app` | `build/GPU Spectrogram.component` |
| Windows | `build/CLAP/Release/GPU Spectrogram.clap` | `build/Release/GPU Spectrogram.vst3` | `build/Release/GPU Spectrogram.exe` | |
| Linux | `build/GPU Spectrogram.clap` | `build/Release/GPU Spectrogram.vst3` | `build/GPU Spectrogram` | |

**WCLAP** (for browser hosts) needs the [WASI SDK](https://github.com/WebAssembly/wasi-sdk/releases) and [Emscripten](https://emscripten.org) (`emcmake` on your PATH) for the page:

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/GpuSpectrogram.wclap.tar.gz
```

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/GPU Spectrogram.clap"                       # CLAP
pluginval --validate "build/GPU Spectrogram.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aufx GpSg ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
