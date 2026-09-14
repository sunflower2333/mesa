#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Enforce the dependency direction and the native submit's production connection."""
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
SHARED = HERE.parent
FREEDRENO = SHARED.parent


# Ignore comments, but not includes or executable tokens, when checking layering.
def code(text: str) -> str:
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)


# Fail rather than allow a native-backend target to silently depend on Vulkan.
def main() -> None:
    paths = list(SHARED.glob('*.h')) + list(SHARED.glob('*.c')) + list(SHARED.glob('*.cc'))
    for path in paths:
        text = code(path.read_text(encoding='utf-8'))
        if re.search(r'\b(?:Vk[A-Za-z_0-9]+|vk_[A-Za-z_0-9]+|TU_HAS_WDDM)\b', text):
            raise RuntimeError(f'Vulkan integration leaked into {path.name}')
        for include in re.findall(r'^\s*#\s*include\s*([<"][^>"\n]+[>"])', text, re.M):
            if re.search(r'vulkan|zink|xf86drm|drm\.h|freedreno_priv', include):
                raise RuntimeError(f'Forbidden backend dependency: {include}')
    meson = (SHARED / 'meson.build').read_text(encoding='utf-8')
    if any(name in meson for name in ('idep_vulkan', 'libvulkan', 'libtu', 'libdrm', 'libzink')):
        raise RuntimeError('Shared target must not link a graphics API implementation')
    if "tu_link_with += libfreedreno_wddm" not in (FREEDRENO / 'vulkan/meson.build').read_text():
        raise RuntimeError('Turnip is not linked to the shared transport')
    integration = (FREEDRENO / 'vulkan/tu_knl_wddm.cc').read_text()
    if integration.count('fd_wddm_build_submit_packet(') != 1:
        raise RuntimeError('Production submission does not call the shared encoder exactly once')
    if 'bool\ntu_wddm_context_render(' in integration:
        raise RuntimeError('Duplicate low-level Render implementation remains in Vulkan')
    if 'struct tu_wddm_msm_submit_request {' in integration:
        raise RuntimeError('Duplicate wire layout remains in Vulkan')
    print(f'PASS native split: {len(paths)} implementation/header files; no Vulkan/Zink/DRM dependency')


if __name__ == '__main__':
    main()
