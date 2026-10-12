#!/usr/bin/env python3
"""Starts a new CLAP plugin project from one of the examples.

    python3 tools/new-plugin.py --from imgui-filter --name "My Plugin" \\
        --vendor "Your Name" --dest ../my-plugin

Anything left out is asked for. The new folder gets the example's files with
its names swapped for yours, a GitHub Actions workflow, a README, a licence and
the third-party notices. Its CMakeLists.txt fetches this template's libraries
(libs/) at a release tag, --template-version (the newest tag here). It
builds with `cmake -B build && cmake --build build --config Release`. It is not
made a git repository: do that yourself when you're ready.

Uses only Python's standard library (3.8+).
"""
import argparse
import datetime
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Each example, by its folder in examples/: what it's called now, and so what gets replaced.
EXAMPLES = {
    "webview-synth": dict(name="WebView Synth", target="WebViewSynth",
                  id="com.charlieculbert.webviewsynth", subtype="WvSy",
                  notices=["clap_template", "clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "choc", "wasi-sdk"]),
    "gpu-analyzer": dict(name="GPU Analyzer", target="GpuAnalyzer",
                     id="com.charlieculbert.gpuanalyzer", subtype="GpAn",
                     notices=["clap_template", "clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "dawn", "accesskit", "stb", "clay", "nanosvg", "signalsmith", "signalsmith-basics", "inter", "cividis", "turbo", "emscripten", "wasi-sdk"]),
    "gpu-spectrogram": dict(name="GPU Spectrogram", target="GpuSpectrogram",
                        id="com.charlieculbert.gpuspectrogram", subtype="GpSg",
                        notices=["clap_template", "clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "dawn", "accesskit", "stb", "clay", "nanosvg", "signalsmith", "cicuetea", "plex", "cividis", "turbo", "emscripten", "wasi-sdk"]),
    "imgui-filter": dict(name="ImGui Filter", target="ImGuiFilter",
                   id="com.charlieculbert.imguifilter", subtype="ImFl",
                   notices=["clap_template", "clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "dawn", "accesskit", "imgui", "cmajor", "inter", "emscripten", "wasi-sdk"]),
    "headless-gain": dict(name="Headless Gain", target="HeadlessGain",
                 id="com.charlieculbert.headlessgain", subtype="HdGn",
                 notices=["clap_template", "clap", "clap-wrapper", "audiounitsdk", "rtaudio", "rtmidi", "ghc-filesystem", "wasi-sdk"]),
}
OLD_VENDOR, OLD_VENDOR_CODE, OLD_URL = "Charlie Culbert", "ChCu", "https://github.com/charCulbert"

NOTICES = {
    "clap_template": ("char-clap-template (its libs/)", "https://github.com/charCulbert/char-clap-template", "MIT"),
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
    "wasi-sdk": ("The WASI SDK's runtime (wasi-libc, libc++, libc++abi, the LLVM runtime and dlmalloc), in the WCLAP only",
                 "https://github.com/WebAssembly/wasi-sdk", "Apache-2.0 with LLVM Exceptions, MIT, or CC0 (dlmalloc)"),
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
    "cividis": ("Cividis (a colour scheme in chardraw's views.h; licence in the template's libs/chardraw/licenses/cividis.txt)",
                "https://github.com/pnnl/cmaputil", "BSD-style (Battelle Memorial Institute)"),
    "turbo": ("Turbo (a colour scheme in chardraw's views.h; licence in the template's libs/chardraw/licenses/turbo.txt)",
              "https://gist.github.com/mikhailov-work/6a308c20e494d9e0ccc29036b28faa7a", "Apache-2.0"),
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

# Where a project gets the template's libraries: in place of the example's
# set(LIBS ".../../libs"), so before the GPU_PAGE branch that uses them too.
FETCH = """# This template's libraries (libs/), at a release: read its release notes before
# changing GIT_TAG. To build against a local checkout of it instead:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE=<checkout>
include(FetchContent)
FetchContent_Declare(clap_template
    GIT_REPOSITORY https://github.com/charCulbert/char-clap-template.git
    GIT_TAG v{version}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none) # only libs/: the template's own CMakeLists isn't for plugins
FetchContent_MakeAvailable(clap_template)
set(LIBS "${{clap_template_SOURCE_DIR}}/libs")"""
EXAMPLE_LIBS = 'set(LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../libs")'


def in_own_checkout():
    """True when REPO is itself a git checkout's top (not an archive unpacked inside someone else's repo)."""
    try:
        top = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return False
    return Path(top).resolve() == REPO.resolve()


def newest_version():
    """The newest release: this checkout's highest vX.Y.Z tag, else the version in the
    folder's name (a GitHub archive: char-clap-template-0.1.0), else None."""
    if in_own_checkout():
        try:
            tags = subprocess.run(["git", "-C", str(REPO), "tag", "--list", "v*", "--sort=-v:refname"],
                                  capture_output=True, text=True, check=True).stdout.split()
        except (OSError, subprocess.CalledProcessError):
            tags = []
        found = next((t[1:] for t in tags if re.fullmatch(r"v\d+\.\d+\.\d+", t)), None)
        if found:
            return found
    named = re.fullmatch(r"char-clap-template-v?(\d+\.\d+\.\d+)", REPO.name)
    return named.group(1) if named else None


def warn_if_not_at(version):
    """The examples copied are this checkout's; the libraries fetched are the tag's."""
    if not in_own_checkout():
        return
    try:
        tags = subprocess.run(["git", "-C", str(REPO), "tag", "--points-at", "HEAD"],
                              capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return  # not a git checkout (a downloaded archive): nothing to compare
    if f"v{version}" not in tags:
        print(f"warning: this checkout isn't at v{version}, so the example copied may not match "
              f"the libraries the project fetches (check out v{version}, or pass --template-version)",
              file=sys.stderr)


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
    parser.add_argument("--template-version", default=newest_version(),
                        help="the template release the project fetches its libraries at (default: the newest "
                             "tag in this checkout, else the version in the folder's name; default %(default)s)")
    args = parser.parse_args()

    example = Path(args.example or ask(f"Example ({', '.join(EXAMPLES)})", "imgui-filter")).name
    if example not in EXAMPLES:
        sys.exit(f"unknown example: {example} (one of {', '.join(EXAMPLES)})")
    old = EXAMPLES[example]
    version = re.sub(r"^v", "", args.template_version or "")  # v0.1.0 or 0.1.0
    if not version:
        sys.exit("no vX.Y.Z tag here (downloaded without git?): pass --template-version")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit(f"--template-version must be a release, like 0.1.0: {version!r}")
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
    warn_if_not_at(version)
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

    # The example, at the project's root, fetching libs/ from the template.
    def fetch_libs(text):
        if EXAMPLE_LIBS not in text:
            sys.exit(f"examples/{example}/CMakeLists.txt has no {EXAMPLE_LIBS} to replace")
        return text.replace(EXAMPLE_LIBS, FETCH.format(version=version))

    def fix_readme(text):
        return re.sub(r"`libs/` holds the reusable parts; see the comment at the top of each `\.cmake`\s+file there\.",
                      "The reusable parts are the template's `libs/`, which CMakeLists.txt fetches\n"
                      "(https://github.com/charCulbert/char-clap-template); see the comment at the\n"
                      "top of each `.cmake` file there.", text)

    source = REPO / "examples" / example
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        if path.is_file() and not skip(relative):
            rewrite = {Path("CMakeLists.txt"): fetch_libs, Path("README.md"): fix_readme}.get(relative)
            copy(path, dest / relative, rewrite)

    # The workflow: the project is the repository's root, and every push runs it.
    def fix_workflow(text):
        replaced = re.sub(r"on:\n  push:\n(?:    branches: .*\n)?    paths:\n(?:      - .*\n)+  pull_request:\n    paths:\n(?:      - .*\n)+",
                          "on:\n  push:\n    branches: [main] # other branches run through their pull request\n  pull_request:\n", text)
        if replaced == text:
            sys.exit(f".github/workflows/{example}.yml has no paths filter to replace: the project's CI would never run")
        text = replaced.replace(f"PROJECT: examples/{example}", "PROJECT: .")
        text = text.replace(" -DWARNINGS_AS_ERRORS=ON", "")  # a newer compiler's warning in pinned third-party code shouldn't turn your CI red
        return text.replace(" Runs when the plugin or a library\n# it uses changes.", "")
    copy(REPO / ".github" / "workflows" / f"{example}.yml",
         dest / ".github" / "workflows" / "build.yml", fix_workflow)

    # A VS Code task that builds and opens the plugin in clap-host
    # (https://github.com/charCulbert/char-clap-host), if it's installed. The task's
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
        f"CMake downloads them, at versions pinned by the template version (v{version}, GIT_TAG in CMakeLists.txt) "
        "and by CMakeLists.txt or signalsmith-dsp.cmake here; keep their notices with "
        f"anything you distribute. Every build ships their full licence texts in THIRD_PARTY_LICENSES/.\n\n{notices}\n", encoding="utf-8")

    print(f"""Made {name} in {dest}
  from the {old['name']} example, fetching the template's libs/ at v{version}
  ID {plugin_id}; AU codes {subtype} (subtype), {vendor_code} (manufacturer)

Next:
  cd "{dest}"
  cmake -B build && cmake --build build --config Release
  git init && git add -A && git commit -m "Start {name}"    # when you're ready
""")


if __name__ == "__main__":
    main()
