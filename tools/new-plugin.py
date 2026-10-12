#!/usr/bin/env python3
"""Starts a new CLAP plugin project from one of the examples.

    python3 tools/new-plugin.py --from imgui-filter --name "My Plugin" \\
        --vendor "Your Name" --dest ~/Development/my-plugin

Anything left out is asked for. The new folder gets the example's files with
its names swapped for yours, only the libraries it uses (libs/), a GitHub
Actions workflow, a README, a licence and the third-party notices. It builds
with `cmake -B build && cmake --build build --config Release`. It is not made a git repository:
do that yourself when you're ready.

Uses only Python's standard library (3.8+).
"""
import argparse
import datetime
import json
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Each example, by its folder in examples/: what it's called now, and so what gets replaced.
EXAMPLES = {
    "webview-synth": dict(name="WebView Synth", target="WebViewSynth",
                  id="com.charlieculbert.webviewsynth", subtype="WvSy", libs=["core", "webview"],
                  notices=["clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "choc"]),
    "gpu-analyzer": dict(name="GPU Analyzer", target="GpuAnalyzer",
                     id="com.charlieculbert.gpuanalyzer", subtype="GpAn", libs=["core", "gpu", "chardraw"],
                     notices=["clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "dawn", "accesskit", "stb", "clay", "nanosvg", "signalsmith", "signalsmith-basics", "inter", "emscripten"]),
    "gpu-spectrogram": dict(name="GPU Spectrogram", target="GpuSpectrogram",
                        id="com.charlieculbert.gpuspectrogram", subtype="GpSg", libs=["core", "gpu", "chardraw"],
                        notices=["clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "dawn", "accesskit", "stb", "clay", "nanosvg", "signalsmith", "cicuetea", "plex", "emscripten"]),
    "imgui-filter": dict(name="ImGui Filter", target="ImGuiFilter",
                   id="com.charlieculbert.imguifilter", subtype="ImFl", libs=["core", "gpu"],
                   notices=["clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "dawn", "accesskit", "imgui", "cmajor", "inter", "emscripten"]),
    "headless-gain": dict(name="Headless Gain", target="HeadlessGain",
                 id="com.charlieculbert.headlessgain", subtype="HdGn", libs=["core"],
                 notices=["clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem"]),
}
OLD_VENDOR, OLD_VENDOR_CODE, OLD_URL = "Charlie Culbert", "ChCu", "https://github.com/charCulbert"

NOTICES = {
    "clap": ("CLAP SDK", "https://github.com/free-audio/clap", "MIT"),
    "clap-wrapper": ("clap-wrapper, and the VST3 SDK it downloads",
                     "https://github.com/free-audio/clap-wrapper", "MIT (see the SDK's own licence)"),
    "audiounitsdk": ("AudioUnit SDK (clap-wrapper downloads it for the AU, on macOS)",
                     "https://github.com/apple/AudioUnitSDK", "Apache-2.0"),
    "rtaudio": ("RtAudio (clap-wrapper downloads it for the standalone app's audio)",
                "https://github.com/thestk/rtaudio", "MIT-style (RtAudio licence)"),
    "rtmidi": ("RtMidi (clap-wrapper downloads it for the standalone app's MIDI)",
               "https://github.com/thestk/rtmidi", "MIT-style (RtMidi licence)"),
    "ghc-filesystem": ("ghc::filesystem (clap-wrapper downloads it on macOS, unless CMAKE_OSX_DEPLOYMENT_TARGET is 10.15 or later)",
                       "https://github.com/gulrak/filesystem", "MIT"),
    "choc": ("CHOC", "https://github.com/Tracktion/choc", "ISC"),
    "dawn": ("Dawn", "https://dawn.googlesource.com/dawn", "BSD 3-Clause"),
    "accesskit": ("AccessKit (its prebuilt libraries; also contain Rust crates under their own licences)",
                  "https://github.com/AccessKit/accesskit", "MIT or Apache-2.0, with a BSD 3-Clause notice from Chromium"),
    "stb": ("stb_truetype, stb_image and stb_textedit", "https://github.com/nothings/stb", "MIT or public domain"),
    "nanosvg": ("nanosvg", "https://github.com/memononen/nanosvg", "zlib"),
    "emscripten": ("Emscripten's runtime and emdawnwebgpu, in the WCLAP's web page only",
                   "https://emscripten.org", "MIT or NCSA (Emscripten), BSD 3-Clause (emdawnwebgpu), plus musl and libc++"),
    "clay": ("Clay", "https://github.com/nicbarker/clay", "zlib"),
    "imgui": ("Dear ImGui", "https://github.com/ocornut/imgui", "MIT"),
    "signalsmith": ("Signalsmith Audio DSP (its FFT)", "https://github.com/Signalsmith-Audio/dsp", "MIT"),
    "signalsmith-basics": ("Signalsmith Basics (AnalyzerSpectrum.h follows its analyser; licence in licenses/SignalsmithBasics.txt)",
                           "https://github.com/Signalsmith-Audio/basics", "MIT"),
    "cicuetea": ("CiCueTea (chardsp_ConstantQ.h follows its constant-Q transform; licence in licenses/CiCueTea.txt)",
                 "https://github.com/jdsierral/CiCueTea", "MIT"),
    "cmajor": ("Cmajor standard library (chardsp_StateVariableFilter.h follows its state-variable filter; licence in licenses/Cmajor.txt)",
               "https://github.com/cmajor-lang/cmajor", "ISC"),
    "inter": ("Inter (font, in resources/fonts with its licence)", "https://rsms.me/inter", "SIL Open Font License 1.1"),
    "plex": ("IBM Plex Mono (font, in resources/fonts with its licence)", "https://github.com/IBM/plex", "SIL Open Font License 1.1"),
}

MIT = """MIT License

Copyright (c) {year} {vendor}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

TEXT_SUFFIXES = {".cpp", ".h", ".mm", ".txt", ".cmake", ".md", ".html", ".js", ".css", ".yml", ".json"}


def words(text):
    return re.findall(r"[A-Za-z0-9]+", text)


def four_char_code(text):
    """An AudioUnit code from a name: two letters of each of the first two
    words ("My Plugin" -> "MyPl"), or four of one word."""
    parts = [w for w in words(text) if w[0].isalpha()] or ["Plug"]
    code = parts[0][:2].capitalize() + parts[1][:2].capitalize() if len(parts) > 1 else parts[0][:4].capitalize()
    return (code + "xxxx")[:4]


def ask(label, default=None):
    if not sys.stdin.isatty():
        if default is None:
            sys.exit(f"--{label.lower().replace(' ', '-')} is needed")
        return default
    answer = input(f"{label}" + (f" [{default}]" if default else "") + ": ").strip()
    return answer or default or ask(label, default)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="example",
                        help="an example's folder in examples/: headless-gain (no UI), webview-synth (a web page), "
                             "gpu-analyzer (chardraw's Spectrum view and readouts), gpu-spectrogram (chardraw's "
                             "views and controls), imgui-filter (Dear ImGui)")
    parser.add_argument("--name", help='the plugin\'s name, e.g. "My Plugin"')
    parser.add_argument("--vendor", help="you or your company")
    parser.add_argument("--dest", help="the new project's folder (must not exist yet, or be empty)")
    parser.add_argument("--id", help="reverse-DNS plugin ID (default: com.<vendor>.<name>)")
    parser.add_argument("--url", default="", help="your website, for the plugin's descriptor")
    parser.add_argument("--au-subtype", help="4-character AudioUnit subtype (default: from the name)")
    parser.add_argument("--au-manufacturer", help="4-character AudioUnit manufacturer code (default: from the vendor)")
    parser.add_argument("--license", default="MIT", help='"MIT" (default), or "none" to add no LICENSE file')
    args = parser.parse_args()

    example = Path(args.example or ask(f"Example ({', '.join(EXAMPLES)})", "imgui-filter")).name
    if example not in EXAMPLES:
        sys.exit(f"unknown example: {example} (one of {', '.join(EXAMPLES)})")
    old = EXAMPLES[example]
    name = args.name or ask("Name")
    vendor = args.vendor or ask("Vendor")
    dest = Path(args.dest or ask("Folder", str(Path.cwd() / "-".join(words(name)).lower()))).expanduser().resolve()

    target = "".join(w[:1].upper() + w[1:] for w in words(name))
    if not target or not target[0].isalpha():
        sys.exit("the name must start with a letter")
    plugin_id = args.id or f"com.{''.join(words(vendor)).lower()}.{''.join(words(name)).lower()}"
    subtype = args.au_subtype or four_char_code(name)
    vendor_code = args.au_manufacturer or four_char_code(vendor)
    for label, code in (("AU subtype", subtype), ("AU manufacturer", vendor_code)):
        if len(code) != 4 or not code.isascii():
            sys.exit(f"{label} must be 4 ASCII characters: {code!r}")
    if dest.exists() and any(dest.iterdir()):
        sys.exit(f"{dest} already exists and isn't empty")
    try:  # inside this repository only under a build*/ folder, which git ignores (check-templates uses one)
        if not dest.relative_to(REPO).parts[0].startswith("build"):
            sys.exit("choose a folder outside this repository")
    except ValueError:
        pass

    # Longest first, so a name isn't half-replaced by a shorter one inside it.
    replacements = sorted({
        old["name"]: name, old["target"]: target, old["id"]: plugin_id,
        OLD_VENDOR: vendor, OLD_URL: args.url,
    }.items(), key=lambda item: -len(item[0]))
    # AudioUnit codes are short, so only where they are codes: quoted, or auval's arguments.
    code_replacements = [(f'"{old["subtype"]}"', f'"{subtype}"'), (f'"{OLD_VENDOR_CODE}"', f'"{vendor_code}"'),
                         (f' {old["subtype"]} {OLD_VENDOR_CODE}"', f' {subtype} {vendor_code}"'),
                         (f' {old["subtype"]} {OLD_VENDOR_CODE} ', f' {subtype} {vendor_code} ')]  # auval in the README

    def copy(source, destination, rewrite=None):
        destination.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix not in TEXT_SUFFIXES:
            shutil.copy2(source, destination)
            return
        text = source.read_text(encoding="utf-8")
        for a, b in replacements + code_replacements:
            text = text.replace(a, b)
        if rewrite:
            text = rewrite(text)
        destination.write_text(text, encoding="utf-8")

    def skip(path):
        return any(part.startswith("build") or part == ".DS_Store" for part in path.parts)

    # The example, at the project's root; libs/ beside it.
    source = REPO / "examples" / example
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        if path.is_file() and not skip(relative):
            fix_libs = (lambda text: text.replace('"${CMAKE_CURRENT_SOURCE_DIR}/../../libs"',
                                                  '"${CMAKE_CURRENT_SOURCE_DIR}/libs"')) \
                if relative.name == "CMakeLists.txt" else None
            copy(path, dest / relative, fix_libs)
    for lib in old["libs"]:
        for path in sorted((REPO / "libs" / lib).rglob("*")):
            if path.is_file() and not skip(path.relative_to(REPO)):
                (dest / path.relative_to(REPO)).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, dest / path.relative_to(REPO))  # libraries stay as they are

    # The workflow: the project is the repository's root, and every push runs it.
    def fix_workflow(text):
        text = re.sub(r"on:\n  push:\n    branches: .*\n    paths:\n(?:      - .*\n)+  pull_request:\n    paths:\n(?:      - .*\n)+",
                      "on:\n  push:\n    branches: [main] # other branches run through their pull request\n  pull_request:\n", text)
        text = text.replace(f"PROJECT: examples/{example}", "PROJECT: .")
        return text.replace(" Runs when the plugin or a library\n# it uses changes.", "")
    copy(REPO / ".github" / "workflows" / f"{example}.yml",
         dest / ".github" / "workflows" / "build.yml", fix_workflow)

    # A VS Code task that builds and opens the plugin in clap-host, if it's installed. The task's
    # terminal stays a prompt for clap-host commands; the audio stays off until you press Power.
    if shutil.which("clap-host"):
        (dest / ".vscode").mkdir()
        (dest / ".vscode" / "clap-host.txt").write_text(
            "panel\n" + ("" if example == "headless-gain" else "gui.open\n"), encoding="utf-8")
        (dest / ".vscode" / "tasks.json").write_text(json.dumps({"version": "2.0.0", "tasks": [
            {"label": "Build", "type": "shell", "command": "cmake -B build && cmake --build build --config Release --parallel",
             "group": {"kind": "build", "isDefault": True}, "problemMatcher": ["$gcc"]},
            {"label": "Run in clap-host", "type": "shell", "command": "clap-host",
             "args": [f"build/{name}.clap", "--script", ".vscode/clap-host.txt"],
             "windows": {"args": [f"build/CLAP/Release/{name}.clap", "--script", ".vscode/clap-host.txt"]},
             "dependsOn": "Build", "problemMatcher": []},
        ]}, indent=2) + "\n", encoding="utf-8")

    (dest / ".gitignore").write_text("build*/\n.cache/\n.idea/\n.DS_Store\n*.swp\n", encoding="utf-8")
    if args.license.lower() == "mit":
        (dest / "LICENSE").write_text(MIT.format(year=datetime.date.today().year, vendor=vendor), encoding="utf-8")
    notices = "\n".join(f"- {title}: {license}. {url}" for title, url, license in (NOTICES[n] for n in old["notices"]))
    (dest / "THIRD_PARTY_NOTICES.md").write_text(
        "# Third-party software\n\nThis plugin is built with, and its binaries include, the following. "
        "CMake downloads them (pinned in libs/*/*.cmake, and in CMakeLists.txt or signalsmith-dsp.cmake beside it); keep their notices with "
        f"anything you distribute. Every build ships their full licence texts in THIRD_PARTY_LICENSES/.\n\n{notices}\n", encoding="utf-8")

    print(f"""Made {name} in {dest}
  from the {old['name']} example; ID {plugin_id}; AU codes {subtype} (subtype), {vendor_code} (manufacturer)

Next:
  cd "{dest}"
  cmake -B build && cmake --build build --config Release
  git init && git add -A && git commit -m "Start {name}"    # when you're ready
""")


if __name__ == "__main__":
    main()
