# GPU Analyzer

A spectrum analyzer: it passes audio through and shows its spectrum, drawn with
WebGPU by our own drawing code.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, state, the spectrum
  analysis hand-off, and the `clap.gui` and `clap.webview` extensions.
- `signalsmith-dsp.cmake`: pins Signalsmith Audio's DSP headers, for their FFT.
- `chardsp_Spectrum.h`: the analysis, after Signalsmith's analyser: three
  window lengths (long for the lows, short for the highs) with a Blackman-Harris
  window, so tones draw as rounded peaks and the highs still react quickly.
- `ui/AnalyzerView.cpp`: the interface, a `gpu::View`: one fragment shader for
  the spectrum, and knobs and labels from `libs/draw2d`, laid out with Clay.
- The display matches FabFilter Pro-Q's: a logarithmic frequency axis
  (10 Hz to 30 kHz), levels in dB from 0 to -100, and a tilt of 4.5 dB per
  octave around 1 kHz by default, so music looks roughly level.
- Smoothing, Colour and Tilt are settings saved in the plugin's state, not
  CLAP parameters, since they only change the display.
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
| macOS | `build/GPU Analyzer.clap` | `build/GPU Analyzer.vst3` | `build/GPU Analyzer.app` | `build/GPU Analyzer.component` |
| Windows | `build/CLAP/Release/GPU Analyzer.clap` | `build/Release/GPU Analyzer.vst3` | `build/Release/GPU Analyzer.exe` | |
| Linux | `build/GPU Analyzer.clap` | `build/Release/GPU Analyzer.vst3` | `build/GPU Analyzer` | |

**WCLAP** (for browser hosts) needs the [WASI SDK 33](https://github.com/WebAssembly/wasi-sdk/releases) and [Emscripten 4.0.11](https://emscripten.org) (`emcmake` on your PATH) for the page:

```sh
cmake -B build-wclap -DCMAKE_TOOLCHAIN_FILE=<wasi-sdk>/share/cmake/wasi-sdk-p1.cmake
cmake --build build-wclap        # makes build-wclap/GpuAnalyzer.wclap.tar.gz
```

## Test

On your machine (paths for macOS; see the table above for the others):

```sh
clap-validator validate "build/GPU Analyzer.clap"                       # CLAP
pluginval --validate "build/GPU Analyzer.vst3" --strictness-level 5     # VST3, opens the interface too
auval -v aufx GpAn ChCu                                     # AU (macOS)
```

[clap-validator](https://github.com/free-audio/clap-validator) and
[pluginval](https://github.com/Tracktion/pluginval) are free downloads.

The workflow in `.github/workflows/` runs the same checks on every push, on
macOS, Windows and Linux, and uploads the plugins only if they pass.

`libs/` holds the reusable parts; see the comment at the top of each `.cmake`
file there.
