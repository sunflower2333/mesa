#!/usr/bin/env python3
"""Exercise the production primary-command-buffer batch planner and submit loop."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from tu_wddm_empty_submit_test import definition


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = (here.parent / "tu_queue.cc").read_text()
    cost = definition(source, "queue_submit_entry_cost")
    functions = cost + "\n\n" + definition(source, "queue_submit")
    fixture = (here / "tu_queue_batch_test.cpp").read_text().replace(
        "// PRODUCTION_FUNCTIONS", functions)
    with tempfile.TemporaryDirectory(prefix="turnip-queue-batch-") as output:
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
