#!/usr/bin/env python3
"""Exercise actual CS allocation/reset with a bounded fake KMT BO allocator."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def definition(source, name):
    match = re.search(r"(?:static\s+|inline\s+)*(?:void\s*\*?|VkResult|uint32_t|uint64_t|struct tu_bo\s*\*)\s*"
                      + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"Missing function: {name}")
    start = source.index("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if not depth:
            return source[match.start():index + 1]
    raise ValueError(name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision")
    parser.add_argument("--reset-revision", help="Replace only reset with older production code")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]

    def read(name):
        path = "src/freedreno/vulkan/" + name
        return (subprocess.check_output(["git", "show", f"{args.revision}:{path}"], cwd=root, text=True)
                if args.revision else (root / path).read_text())

    header, source, sub_header, sub_source = map(read, ("tu_cs.h", "tu_cs.cc", "tu_suballoc.h", "tu_suballoc.cc"))
    structs = "\n".join(re.search(rf"{kind} {name}\s*\{{.*?\n\}};", header, re.S).group()
                        for kind, name in (("enum", "tu_cs_mode"), ("struct", "tu_cs_entry"),
                                           ("struct", "tu_bo_array"), ("struct", "tu_cs")))
    sub_structs = "\n".join(re.search(rf"struct {name}\s*\{{.*?\n\}};", sub_header, re.S).group()
                            for name in ("tu_suballocator", "tu_suballoc_bo"))
    functions = "\n".join(definition(sub_source, name) for name in
                          ("tu_bo_suballocator_init", "tu_bo_suballocator_finish", "tu_suballoc_bo_alloc", "tu_suballoc_bo_map"))
    functions += "\n" + "\n".join(definition(source, name) for name in
                                    ("tu_sanitize_ib_size", "tu_cs_init", "tu_cs_finish", "tu_cs_current_bo",
                                     "tu_cs_get_offset", "tu_cs_get_cur_iova", "tu_cs_add_bo", "tu_cs_reserve_entry",
                                     "tu_cs_add_entry", "tu_cs_set_writeable"))
    reset_source = (subprocess.check_output(["git", "show", f"{args.reset_revision}:src/freedreno/vulkan/tu_cs.cc"],
                                           cwd=root, text=True) if args.reset_revision else source)
    functions += "\n" + definition(reset_source, "tu_cs_reset")
    fixture = (here / "tu_cs_pool_test.cpp").read_text()
    fixture = fixture.replace("// PRODUCTION_SUB_STRUCTS", sub_structs)
    fixture = fixture.replace("// PRODUCTION_CS_STRUCTS", structs)
    fixture = fixture.replace("// PRODUCTION_FUNCTIONS", functions)
    with tempfile.TemporaryDirectory(prefix="tu-cs-pool-") as output:
        unit = Path(output) / "fixture.cpp"
        unit.write_text(fixture)
        executable = Path(output) / ("fixture.exe" if os.name == "nt" else "fixture")
        compiler = shutil.which("clang-cl") if os.name == "nt" else None
        if compiler:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++20",
                       "/clang:-Wno-c++11-narrowing", str(unit), f"/Fe{executable}"]
        else:
            command = ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-narrowing", str(unit), "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, cwd=output, check=True)
        return subprocess.run([str(executable)], cwd=output).returncode


if __name__ == "__main__":
    raise SystemExit(main())
