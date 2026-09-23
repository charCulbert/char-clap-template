# Build and test

## Desktop (macOS / Windows / Linux)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target simpleCharClap_clap --config Debug
```

| `simpleCharClap_clap` | `clap-validator validate <plugin.clap> --only-failed`;
| `simpleCharClap_vst3` | `pluginval --validate <plugin.vst3> --strictness-level 5 --skip-gui-tests`
| `simpleCharClap_standalone`
| `simpleCharClap_auv2` (macOS) | Copy `.component` to `~/Library/Audio/Plug-Ins/Components/`; `auval -v aufx SvF1 ChCu`
| `simpleCharClap_all`


## macOS AUv3

```sh
cmake -S . -B build-auv3 -G Xcode
cmake --build build-auv3 --target simpleCharClap_auv3_standalone --config Debug
open "build-auv3/Debug/simpleCharClap AUv3.app"
```

## iPhone / iPad AUv3

```sh
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64
open build-ios/simpleCharClap.xcodeproj
```

Xcode: `simpleCharClap_ios` scheme → simulator or signed device → Run.

## AAX (local SDK)

```sh
cmake -S . -B build-aax -DCMAKE_BUILD_TYPE=Debug \
  -DSIMPLECHAR_BUILD_AAX=ON -DAAX_SDK_ROOT="$HOME/aax-sdk-2-9-0"
cmake --build build-aax --target simpleCharClap_aax --config Debug
```

macOS output: `build-aax/simpleCharClap.aaxplugin` (or `build-aax/Debug/` with Xcode).


## WCLAP

```sh
cmake -S . -B build-wclap \
  -DCMAKE_TOOLCHAIN_FILE="<wasi-sdk>/share/cmake/wasi-sdk.cmake"
cmake --build build-wclap --target simpleCharClap_wclap
```
