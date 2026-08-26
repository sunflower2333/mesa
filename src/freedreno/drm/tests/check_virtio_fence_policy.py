#!/usr/bin/env python3

from pathlib import Path
import re
import sys


TEST_DIR = Path(__file__).resolve().parent
DRM_DIR = TEST_DIR.parent


def fail(message: str) -> None:
    raise RuntimeError(message)


def canonical(text: str) -> str:
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"\s+", "", text)


def function_body(name: str, source: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        fail(f"function not found: {name}")

    start = source.find("{", match.start())
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : index]
    fail(f"unterminated function: {name}")


def require_order(text: str, fragments: tuple[str, ...], message: str) -> None:
    offset = -1
    for fragment in fragments:
        next_offset = text.find(fragment, offset + 1)
        if next_offset < 0:
            fail(f"{message}: missing {fragment}")
        offset = next_offset


def main() -> int:
    pipe = (DRM_DIR / "freedreno_pipe.c").read_text(encoding="utf-8")
    priv = (DRM_DIR / "freedreno_priv.h").read_text(encoding="utf-8")
    virtio = (DRM_DIR / "virtio" / "virtio_pipe.c").read_text(encoding="utf-8")

    if '"util/cache_ops.h"' not in pipe or '"util/os_time.h"' not in pipe:
        fail("freedreno waits must include cache maintenance and monotonic time helpers")
    if "int64_t wait_spin_ns;" not in priv or "bool control_needs_inval;" not in priv:
        fail("fd_pipe must retain the virtio wait policy state")

    control = canonical(function_body("control_fence_reached", pipe))
    require_order(
        control,
        (
            "if(pipe->control_needs_inval)",
            "util_flush_inval_range((void*)(uintptr_t)pipe->control,sizeof(*pipe->control))",
            "return!fd_fence_after(ufence,pipe->control->fence)",
        ),
        "control-fence reads must invalidate guest-backed cache lines before comparison",
    )

    wait = canonical(function_body("fd_pipe_wait_timeout", pipe))
    require_order(
        wait,
        (
            "if(control_fence_reached(pipe,fence->ufence))",
            "if(!timeout)",
            "fd_pipe_flush(pipe,fence->ufence)",
            "if(pipe->wait_spin_ns)",
            "if(control_fence_reached(pipe,fence->ufence))",
            "returnpipe->funcs->wait(pipe,fence,timeout)",
        ),
        "freedreno waits must poll the control fence before the virtio round trip",
    )

    spin = canonical(function_body("virtio_wait_spin_ns", virtio))
    require_order(
        spin,
        (
            'debug_get_num_option("FD_POLL_SPIN_US",1200)',
            "if(spin_us<=0)return0",
            "if(spin_us>INT64_MAX/1000)returnINT64_MAX",
            "returnspin_us*1000",
        ),
        "FD_POLL_SPIN_US must be bounded and disabled for non-positive values",
    )

    new_pipe = canonical(function_body("virtio_pipe_new", virtio))
    require_order(
        new_pipe,
        (
            "pipe->wait_spin_ns=virtio_wait_spin_ns()",
            "pipe->control_needs_inval=vdrm->supports_guest_alloc",
        ),
        "virtio pipes must enable cache-aware poll-first waits only for guest allocations",
    )

    new_pipe_setup = canonical(function_body("fd_pipe_new2", pipe))
    require_order(
        new_pipe_setup,
        (
            "pipe->control->fence=0",
            "if(pipe->control_needs_inval)",
            "util_flush_range((void*)(uintptr_t)pipe->control,sizeof(*pipe->control))",
            "pipe->control_mem->bo_reuse=NO_CACHE",
        ),
        "control-fence reset must be published before cache invalidation can resume",
    )

    print("freedreno virtio guest-fence policy passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
