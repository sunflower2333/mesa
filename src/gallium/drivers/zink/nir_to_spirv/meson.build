# Copyright © 2026 Valve Corporation
# SPDX-License-Identifier: MIT

inc_ntv = include_directories('.')

files_ntv = files(
  'nir_to_spirv.c',
  'spirv_builder.c',
)

idep_ntv = declare_dependency(
  sources : [files_ntv],
  include_directories : [inc_util, inc_src, inc_gallium, inc_gallium_aux],
  dependencies : [idep_nir_headers, idep_mesautil],
)

if with_tests
  test(
    'zink_spirv_builder_test',
    executable(
      'zink_spirv_builder_test',
      ['spirv_builder_test.c', 'spirv_builder.c'],
      include_directories : [inc_src, inc_include, inc_gallium, inc_gallium_aux],
      dependencies : [idep_mesautil],
    ),
    suite : ['zink'],
  )
endif
