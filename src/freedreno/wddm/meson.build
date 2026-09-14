# SPDX-License-Identifier: MIT
# Shared KMT backend: intentionally independent of Turnip and Gallium.
libfreedreno_wddm = static_library(
  'freedreno_wddm',
  ['freedreno_wddm.cc', 'tu_wddm_dispatch.cc', 'freedreno_wddm_submit.c'],
  include_directories : include_directories('.'),
  cpp_args : ['-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A00000C'],
  gnu_symbol_visibility : 'hidden',
  build_by_default : false,
)
