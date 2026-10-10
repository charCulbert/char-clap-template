# GPU Spectrogram

A scrolling spectrogram, as Listener's: it passes audio through and shows
how its spectrum changes over time. The interface uses only `libs/draw2d`'s
parts, with no drawing code of its own; for a drawing of your own, see
`examples/gpu-analyzer`.

- `entry.cpp`: the CLAP entry point, which makes the plugin.
- `plugin.cpp`: the whole CLAP plugin: audio ports, state, the hand-off of
  the analysis to the interface, and the `clap.gui` and `clap.webview` extensions.
- `signalsmith-dsp.cmake`: pins Signalsmith Audio's DSP headers, for their FFT.
- `chardsp_Stft.h`: the analysis, a short-time Fourier transform as
  Listener's, in dB relative to a full-scale sine. The audio thread only
  copies both channels into a ring; the main thread sets up the FFT and
  window and reads the ring.
- `chardsp_ConstantQ.h`: the constant-Q transform (`ConstantQStream`), a
  copy of chardsp's (chardsp main, commit 4761447) with only its include of
  Signalsmith's FFT changed to this example's `dsp/fft.h`. It follows
  [CiCueTea](https://github.com/jdsierral/CiCueTea) (MIT, Juan Sierra);
  `licenses/CiCueTea.txt` is its licence, which ships with the plugin's
  other third-party licences. Its ring works as the STFT's.
- `ui/SpectrogramView.cpp`: the interface, a `gpu::View`: a
  `gpu::Spectrogram` (`draw2d/views.h`) fills the window, frequencies up its
  left edge and seconds along the bottom drawn over it, a Pause toggle in its
  top right corner, and the settings drawer's tab (`Ui::drawer`, an overlay)
  on its right edge. Open, the drawer's panel covers the plot's right side
  with the settings as readouts (`Ui::readout`).
- `resources/`: the files the plugin ships (its font, IBM Plex Mono). Add images, sounds or
  anything else here and read them with `core::readResource("/path")` (on the
  main thread), or `gpu::loadAsset` in the view.
- Natively `libs/gpu` draws the view with Dawn in the host's window. In WCLAP
  the same view is compiled with Emscripten into the page the host shows.

## Settings

Listener's spectrogram settings, with its ranges and defaults. They are saved
in the plugin's state, not CLAP parameters, since hosts have no use for
automating them.

| Setting | Values | Default |
|---|---|---|
| FFT size | 256 to 16384, powers of two | 2048 |
| Hop size | 1 sample to the FFT size | 512 |
| Window | rectangular, Hann, Hamming or Blackman | Hann |
| Channel | Mix, Left or Right | Mix |
| Smoothing | 0 (off) to 0.9: each bin keeps that much of its last value (in dB) | 0 |
| Frequency range | lower 0 to 500 Hz, upper 100 Hz to Nyquist | 20 Hz to 24 kHz |
| Level range | floor −160 to −60 dB, ceiling −20 to 12 dB | −100 to 0 dB |
| Scale | linear, log or mel | log |
| Palette | System, Bone, Inferno, Viridis or Magma | System |
| Scroll rate | 25 to 200 points a second, whatever the hop | 100 |
| Piano strip, key tone, peak snap, cursor readout | on or off | on |
| Transform | FFT or CQT | FFT |
| CQT bands per octave | 12, 24 or 36 | 24 |
| A4 tuning | 400 to 480 Hz | 440 Hz |
| CQT frequency limits | lower 27.5 Hz (A0) to 220 Hz, upper 1 kHz to 0.4 × the sample rate | 55 Hz to 16 kHz |
| CQT update rate | 15, 30 or 60 a second | 30 |
| CQT reading | Mean, Peak or Centre | Mean |

With CQT as the transform the spectrogram shows a constant-Q transform:
bands spaced by pitch, A4 tuning × 2^(k / bands per octave), so the low end
has resolution without an enormous FFT. Its settings show in place of the
FFT size, hop and window. The plot's frequency range stops at its bands, and
the cursor readout, peak snap, piano strip and key tone all go by the same
bands and tuning. A band's level over each update is its power over that
time (Mean), its loudest moment (Peak, which shows a click at its own level
but reads noise higher in the high bands), or its value at the update's
middle (Centre, as Listener reads it). The panel shows the delay from a
sound to its column: half the transform's block, about 341 ms at 48 kHz with
the defaults (the block holds 24 periods of 55 Hz). A lower floor, down to
A0 (27.5 Hz), resolves the bass's lowest notes at a cost: the block, and so
the delay, doubles below about 35 Hz at 24 bands an octave (683 ms). The
display's upper limit goes down to 100 Hz so the plot can zoom in on them;
the range stays an octave at least. The CQT runs from 44.1
to 96 kHz, as Listener's; at other sample rates the plugin shows the FFT and
the panel says so. A4 tuning also sets the piano strip's keys and the pitch
names with the FFT.

The settings are in a drawer. The tab on the window's right edge (named
**Settings** for screen readers, and in its tooltip) slides its panel out
over the plot, which keeps its size, so the time axis and scrolling stay as
they are. The panel is opaque, with one setting a row, 24 points apart; at
the default window size (720 by 480 points) every setting fits, and in a
smaller window the panel scrolls. There the wheel scrolls the panel and turns
a readout only while it's focused. The tab then rides on
the panel's left edge and closes it again; so does Escape from anywhere in
the panel, and focus goes back to the tab. Tab goes from the tab into the
panel, which scrolls to show the setting focused. Drag the panel's left
edge (or focus the edge and use the arrows) to resize it, from 240 to 480
points. Whether it is open and its width are saved with the plugin's state.
Pause moves left of the open panel, so it stays in view.

Drag a readout up or down (Shift for fine) or scroll over it to change it;
Escape during the drag puts it back. Double-click it, press Delete or
Backspace, or choose Reset to Default from its context menu, to reset it;
click one with a list of values (FFT size, Window, Channel, Scale, Palette,
the switches) to step to the next. With a readout focused, the arrows, Page
Up and Down, Home and End step it. Screen readers see each as a spin button.

Hovering over the spectrogram shows the frequency, the nearest pitch in cents
and the level under the pointer. Peak snap follows the nearest peak in that
column within 10 points (keeping one it follows within 18), as Listener's;
hold Command to bypass it.

Space, or the **Pause** toggle, freezes the display; the analysis keeps
running. The colours, level and frequency ranges and the scale still apply to
the frozen picture, and resuming starts a fresh history. Space goes to a
focused button, toggle or the drawer's tab first, which it presses. Screen
readers hear "Display paused" and "Display resumed".

Hold a key of the piano strip to hear a quiet sine (−18 dBFS) at its pitch,
drag to another key to change note, and let go to stop; a line marks the
exact pitch across the plot while it's held. The plugin has no output of its
own, so the tone is mixed into its audio output, fading in and out over a
few milliseconds. The analysis takes its input before the tone is added, so
the spectrogram never shows it. Turn **Key tone** off to keep the guide line
without the sound. To screen readers the strip is one picture.

A short hop with a large FFT is more work than the main thread can do in
real time: the plugin analyses at most 8 columns each time it runs (60 times
a second) and skips older ones, so the picture keeps time.

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
