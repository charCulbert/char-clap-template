#!/usr/bin/env python3
"""Checks tools/new-plugin.py: makes a project from every example in
build-templates/, builds each with `cmake -B build && cmake --build build --config Release`
against this working tree (FETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE) rather than
the release it pins, and, if clap-host
(https://github.com/charCulbert/char-clap-host) is installed, validates its CLAP. Run it after changing an example, a library or the
generator. (CI doesn't: it's slow and local.)

    python3 tools/check-templates.py [example ...]
"""
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
# The examples new-plugin.py knows: one list, there.
_spec = importlib.util.spec_from_file_location("new_plugin", REPO / "tools" / "new-plugin.py")
new_plugin = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(new_plugin)
EXAMPLES = list(new_plugin.EXAMPLES)


def run(command, cwd):
    print("  $", " ".join(command))
    # No stdin: given a terminal, clap-host keeps reading commands from it.
    result = subprocess.run(command, cwd=cwd, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-4000:], result.stderr[-4000:], sep="\n")
    return result.returncode == 0


def main():
    examples = sys.argv[1:] or EXAMPLES
    root = REPO / "build-templates"
    failed = []
    for example in examples:
        print(f"{example}:")
        dest = root / f"check-{example}"
        shutil.rmtree(dest, ignore_errors=True)
        ok = run([sys.executable, str(REPO / "tools" / "new-plugin.py"), "--from", example,
                  "--name", f"Check {example.capitalize()}", "--vendor", "Check Vendor", "--template-version", "0.1.0", "--dest", str(dest)], REPO)
        ok = ok and run(["cmake", "-B", "build", f"-DFETCHCONTENT_SOURCE_DIR_CLAP_TEMPLATE={REPO}"], dest) and run(["cmake", "--build", "build", "--config", "Release", "--parallel"], dest)
        clap = next(dest.glob("build/**/*.clap"), None) if ok else None
        if ok and not clap:
            print("  no .clap was built")
            ok = False
        if ok and shutil.which("clap-host"):
            ok = run(["clap-host", "--strict", "--json", "--quiet", str(clap), "--", "validate.run", "--isolate"], dest)
        print("  ok" if ok else "  FAILED")
        if not ok:
            failed.append(example)
    if failed:
        sys.exit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
