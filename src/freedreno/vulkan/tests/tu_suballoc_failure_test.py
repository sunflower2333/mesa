#!/usr/bin/env python3
"""Exercise production BO suballocator rollback and retry after a failed map."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from tu_cs_failure_test import definition


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    relative = "src/freedreno/vulkan/tu_suballoc.cc"
    source = (subprocess.check_output(["git", "show", f"{args.revision}:{relative}"],
                                     cwd=root, text=True) if args.revision else
              (root / relative).read_text())
    header = (here.parent / "tu_suballoc.h").read_text()
    structs = "\n".join(re.search(rf"struct {name}\s*\{{.*?\n\}};", header, re.S).group()
                        for name in ("tu_suballocator", "tu_suballoc_bo"))
    functions = "\n".join(definition(source, name) for name in
                          ("tu_bo_suballocator_init", "tu_bo_suballocator_finish", "tu_suballoc_bo_alloc"))
    fixture = (here / "tu_suballoc_failure_test.cpp").read_text()
    fixture = fixture.replace("// PRODUCTION_STRUCTS", structs).replace("// PRODUCTION_FUNCTIONS", functions)
    with tempfile.TemporaryDirectory(prefix="tu-suballoc-failure-") as directory:
        unit = Path(directory) / "fixture.cpp"
        unit.write_text(fixture)
        executable = Path(directory) / ("fixture.exe" if os.name == "nt" else "fixture")
        compiler = shutil.which("clang-cl") if os.name == "nt" else None
        if compiler:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++20",
                       str(unit), f"/Fe{executable}"]
        else:
            command = ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=directory, check=True)
        return subprocess.run([str(executable)], cwd=directory).returncode


if __name__ == "__main__":
    raise SystemExit(main())
