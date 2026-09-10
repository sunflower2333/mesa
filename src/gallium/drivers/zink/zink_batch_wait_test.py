#!/usr/bin/env python3
"""Execute the production unflushed wait and reset query with scheduled events."""
import argparse
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r"(?:static\s+)?(?:bool|enum pipe_reset_status)\s+" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    depth = 0
    for i in range(source.index("{", match.start()), len(source)):
        depth += (source[i] == "{") - (source[i] == "}")
        if not depth:
            return source[match.start():i+1]
    raise ValueError("Unterminated " + name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    def read(name):
        if args.revision:
            return subprocess.check_output(["git", "show", args.revision + ":src/gallium/drivers/zink/" + name], cwd=here, text=True)
        return (here / name).read_text()
    source = definition(read("zink_batch.c"), "zink_batch_usage_unflushed_wait")
    source += "\n" + definition(read("zink_context.c"), "zink_get_device_reset_status")
    fixture = (here / "zink_batch_wait_test.cpp").read_text().replace("// PRODUCTION_FUNCTIONS", source)
    with tempfile.TemporaryDirectory(prefix="zink-batch-wait-") as temporary:
        out = Path(temporary)
        unit = out / "fixture.cpp"
        unit.write_text(fixture)
        if os.name == "nt":
            executable = out / "fixture.exe"
            compiler = shutil.which("clang-cl")
            command = [compiler, "/nologo", "/EHsc", "/std:c++20", "/W4", "/WX", str(unit), f"/Fe{executable}"]
        else:
            executable = out / "fixture"
            command = ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=out, check=True)
        return subprocess.run([str(executable)], cwd=out).returncode


if __name__ == "__main__":
    raise SystemExit(main())
