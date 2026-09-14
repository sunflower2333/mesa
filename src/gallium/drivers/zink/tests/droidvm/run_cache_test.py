#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the production last-pipeline predicate with explicit fake key types."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

FIXTURE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
enum { MESA_SHADER_FRAGMENT = 4 };
struct zink_gfx_pipeline_state { uint32_t baked, module, dynamic; };
struct zink_gfx_pipeline_cache_entry { zink_gfx_pipeline_state state; };
struct table { bool (*key_equals_function)(const void *, const void *); };
struct shader { struct { unsigned legacy_shadow_mask; } fs; };
struct zink_gfx_program {
   uint32_t last_finalized_hash[4];
   bool inline_variants;
   zink_gfx_pipeline_cache_entry *last_pipeline[4];
   shader *shaders[5];
   table pipelines[11];
};
static unsigned comparisons;
static unsigned checks;
// Fake semantic key comparator deliberately ignores dynamically supplied state.
static bool equal_key(const void *left, const void *right)
{
   ++comparisons;
   const auto *a = static_cast<const zink_gfx_pipeline_state *>(left);
   const auto *b = static_cast<const zink_gfx_pipeline_state *>(right);
   return a->baked == b->baked && a->module == b->module;
}
// Fail with a stable marker; never depend on assert/NDEBUG for test verdicts.
static void check(bool value, const char *message)
{
   ++checks;
   if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
// INSERT_PRODUCTION
// Exercise collisions, fast rejects, semantic dynamic state, and all cache slots.
int main()
{
   shader fs = {};
   zink_gfx_program prog = {};
   prog.shaders[MESA_SHADER_FRAGMENT] = &fs;
   zink_gfx_pipeline_cache_entry entries[4] = {};
   for (unsigned slot = 0; slot < 4; ++slot) {
      prog.pipelines[slot].key_equals_function = equal_key;
      prog.last_finalized_hash[slot] = 0xabc;
      entries[slot].state = {slot + 1, 7, 100};
      auto request = entries[slot].state;
      comparisons = 0;
      check(!zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "empty cache accepted");
      check(comparisons == 0, "empty cache called equality");
      prog.last_pipeline[slot] = &entries[slot];
      check(zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "equal key missed");
      ++request.baked;
      check(!zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "hash collision accepted");
      request = entries[slot].state;
      ++request.module;
      check(!zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "module collision accepted");
      request = entries[slot].state;
      ++request.dynamic;
      check(zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "dynamic-only state rejected");
      comparisons = 0;
      check(!zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabd), "hash mismatch accepted");
      prog.inline_variants = true;
      check(!zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "inline variant accepted");
      prog.inline_variants = false;
      fs.fs.legacy_shadow_mask = 1;
      check(!zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc), "shadow variant accepted");
      fs.fs.legacy_shadow_mask = 0;
      check(comparisons == 0, "fast reject called equality");
   }
   uint32_t rng = 17;
   for (unsigned i = 0; i < 100000; ++i) {
      rng = rng * 1664525u + 1013904223u;
      unsigned slot = (rng >> 8) & 3;
      auto request = entries[slot].state;
      bool same = (rng & 1) != 0;
      request.baked += same ? 0 : 1;
      request.dynamic = rng;
      comparisons = 0;
      check(zink_can_reuse_cached_pipeline(&prog, &request, slot, 0xabc) == same,
            "randomized same-hash selection mismatch");
      check(comparisons == 1, "candidate did not use one semantic comparison");
   }
   std::printf("PASS: %u predicate checks; fake key types, no GPU execution\n", checks);
}
'''


# Extract a uniquely named production function, including its balanced body.
def production_function(source: str) -> str:
    start = source.index('static bool\nzink_can_reuse_cached_pipeline(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end]


# Run the tracked predicate, requiring an intentional hash-only mutation to fail.
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--negative-control', action='store_true')
    args = parser.parse_args()
    source_path = Path(__file__).resolve().parents[2] / 'zink_program_state.hpp'
    source = source_path.read_text(encoding='utf-8')
    for field in ('final_hash', 'mesh_final_hash'):
        expected = f'zink_can_reuse_cached_pipeline(prog, state, idx, state->{field})'
        if source.count(expected) != 1:
            raise RuntimeError(f'production {field} fast path is not connected')
    body = production_function(source)
    if args.negative_control:
        old = 'return prog->pipelines[idx].key_equals_function(\n      &prog->last_pipeline[idx]->state, state);'
        if body.count(old) != 1:
            raise RuntimeError('negative-control anchor changed')
        body = body.replace(old, '(void)state;\n   return true;')
    compiler = os.environ.get('CXX') or ('cl' if os.name == 'nt' else 'g++')
    if not shutil.which(compiler):
        raise RuntimeError(f'compiler not found: {compiler}')
    with tempfile.TemporaryDirectory(prefix='zink-cache-') as directory:
        root = Path(directory)
        src = root / 'test.cpp'
        exe = root / ('test.exe' if os.name == 'nt' else 'test')
        src.write_text(FIXTURE.replace('// INSERT_PRODUCTION', body), encoding='utf-8')
        if Path(compiler).stem.lower() == 'cl':
            command = [compiler, '/nologo', '/std:c++17', '/W4', '/WX', '/EHsc',
                       str(src), f'/Fe:{exe}']
        else:
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-fno-pie', '-no-pie', str(src), '-o', str(exe)]
        subprocess.run(command, check=True, cwd=root, timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if args.negative_control:
            if result.returncode != 1 or result.stderr.strip() != 'FAIL: hash collision accepted':
                raise RuntimeError(f'wrong negative-control failure: {result}')
            print('PASS: hash-only negative control rejected at intended assertion')
        else:
            print(result.stdout, end='')
            if result.returncode:
                raise RuntimeError(result.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
