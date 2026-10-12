# Changelog

What changed in each release of the template's `libs/`, for plugins that fetch
it (see README.md, "Using the template as a dependency"). Releases are tagged
`v<version>`. Until 1.0, a minor version (0.1 to 0.2) may break a plugin; a
patch version won't.

Each release lists:

- **Breaking:** what a plugin must change, in its code, CMake or CI, to bump to it.
- **Added:** new libraries, functions and options.
- **Fixed:** bugs fixed.
- **Dependencies:** pins that moved (each `libs/*/*.cmake` pins its own).

## Unreleased

## [0.1.0] - 2026-10-12

The first release.

### Added

- `libs/core`: the CLAP SDK and clap-wrapper (CLAP, VST3, AUv2, standalone,
  WCLAP, AAX, and an iOS app with an AUv3, from one CLAP plugin), the CBOR message codec, the
  resource reader, and `core_ship_resources`, which ships each plugin's files
  and the licences of everything it downloads.
- `libs/webview`: a web page interface, in a CHOC WebView or the host's
  `clap.webview`.
- `libs/gpu`: a WebGPU interface, drawn with Dawn natively and as an
  Emscripten page in a WCLAP, with an accessibility tree for screen readers.
- `libs/chardraw`: drawing, Clay layout, controls, themes and views (waveform,
  scope, spectrum, spectrogram) for a GPU interface; colour schemes as data.
- Five examples to start from, the GUI Gallery, and `tools/new-plugin.py`,
  which makes a project that fetches this template at a release.

### Dependencies

CLAP SDK 1.2.10; clap-wrapper at 2bc329c (its `next` branch); CHOC at 9606a66;
Dawn v20260922.191850; AccessKit 0.23.1; stb at 2c980bb, Clay at 7f767d2 and
nanosvg at 239e102. WCLAP builds need WASI SDK 33, and Emscripten 4.0.11 for a
GPU interface; CMake doesn't download those.
