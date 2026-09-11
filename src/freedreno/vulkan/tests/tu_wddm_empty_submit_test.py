#!/usr/bin/env python3
"""Exercise production queue signaling/CPU waits with controlled KMT retirement."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r"static\s+(?:bool|void|VkResult)\s+" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"Missing production definition: {name}")
    depth = 0
    for index in range(source.index("{", match.start()), len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[match.start():index + 1]
    raise ValueError(f"Unterminated production definition: {name}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision", help="Older source for a negative control")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    relative = "src/freedreno/vulkan/tu_knl_wddm.cc"
    source = (subprocess.check_output(["git", "show", f"{args.revision}:{relative}"],
                                     cwd=root, text=True) if args.revision else
              (root / relative).read_text())
    names = ["tu_wddm_submit_add_entries", "tu_wddm_sync_wait", "tu_wddm_sync_set_submit_fence",
             "tu_wddm_queue_submit_locked"]
    fixture = (here / "tu_wddm_empty_submit_test.cpp").read_text().replace(
        "// PRODUCTION_FUNCTIONS", "\n\n".join(definition(source, name) for name in names))
    with tempfile.TemporaryDirectory(prefix="turnip-empty-submit-") as output:
        directory = Path(output)
        unit = directory / "fixture.cpp"
        unit.write_text(fixture)
        compiler = shutil.which("clang-cl") if os.name == "nt" else None
        executable = directory / ("fixture.exe" if compiler else "fixture")
        if compiler:
            if args.sanitize:
                parser.error("--sanitize requires the Linux compiler")
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++20",
                       str(unit), f"/Fe{executable}"]
        else:
            command = ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                       str(unit), "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=directory, check=True)
        return subprocess.run([str(executable)], cwd=directory).returncode


if __name__ == "__main__":
    raise SystemExit(main())
