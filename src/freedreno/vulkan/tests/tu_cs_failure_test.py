#!/usr/bin/env python3
"""Run production command-stream failure paths with a controlled BO allocator."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r"(?:static inline\s+)?(?:void|VkResult|struct tu_draw_state|struct tu_cs_entry)\s+"
                      + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"Definition missing: {name}")
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[match.start():index + 1]
    raise ValueError(f"Unterminated: {name}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision", help="Use older production source as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]

    def read(name):
        relative = "src/freedreno/vulkan/" + name
        if args.revision:
            return subprocess.check_output(["git", "show", f"{args.revision}:{relative}"],
                                           cwd=root, text=True)
        return (root / relative).read_text()

    header, source = read("tu_cs.h"), read("tu_cs.cc")
    declarations = []
    for kind, name in [("enum", "tu_cs_mode"), ("struct", "tu_cs_entry"),
                       ("struct", "tu_cs_memory"), ("struct", "tu_draw_state"),
                       ("struct", "tu_bo_array"), ("struct", "tu_cs")]:
        declarations.append(re.search(rf"{kind} {name}\s*\{{.*?\n\}};", header, re.S).group())
    sink_size = re.search(r"#define TU_CS_FAIL_SINK_SIZE .*", header).group()
    functions = [definition(source, "tu_cs_init_external"), definition(header, "tu_cs_fail")]
    if "tu_cs_init_failed_external" in header:
        functions.append(definition(header, "tu_cs_init_failed_external"))
    functions += [definition(source, "tu_cs_begin_sub_stream_aligned"),
                  definition(header, "tu_cs_draw_state"),
                  definition(source, "tu_cs_end_sub_stream"),
                  definition(source, "tu_cs_reset")]
    fixture = (here / "tu_cs_failure_test.cpp").read_text()
    fixture = fixture.replace("// PRODUCTION_CS_STRUCTS", "\n".join(declarations))
    fixture = fixture.replace("// PRODUCTION_SINK_SIZE", sink_size)
    fixture = fixture.replace("// PRODUCTION_FUNCTIONS", "\n\n".join(functions))
    with tempfile.TemporaryDirectory(prefix="turnip-cs-failure-") as output:
        directory = Path(output)
        unit = directory / "fixture.cpp"
        unit.write_text(fixture)
        # Mesa uses GNU compound literals in C++ and builds with clang-cl.
        compiler = shutil.which("clang-cl") if os.name == "nt" else None
        if compiler:
            if args.sanitize:
                parser.error("--sanitize requires clang++ or g++")
            executable = directory / "fixture.exe"
            # Production CS entries multiply uint32 sizes by sizeof(uint32_t).
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX",
                       "/clang:-Wno-c++11-narrowing", "/std:c++20", str(unit),
                       f"/Fe{executable}"]
        else:
            executable = directory / "fixture"
            command = ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-narrowing", str(unit),
                       "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=directory, check=True)
        return subprocess.run([str(executable)], cwd=directory).returncode


if __name__ == "__main__":
    raise SystemExit(main())
