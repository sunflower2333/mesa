/* SPDX-License-Identifier: MIT */
#include "spirv_builder.h"
#include "util/ralloc.h"
#include <stdio.h>

/* Decode the SPIR-V wire operands by their specified mask order. In
 * particular, a multisample fetch with an offset must not exchange the
 * scalar sample ID and the vector offset ID.
 */
static bool
check_fetch(bool constant_offset, bool with_offset, bool with_sample, bool sparse)
{
   struct spirv_builder b = {0};
   b.mem_ctx = ralloc_context(NULL);
   SpvId result_type = spirv_builder_type_vector(&b,
      spirv_builder_type_float(&b, 32), 4);
   struct spriv_tex_src src = {
      .coord = 100,
      .sample = with_sample ? 101 : 0,
      .const_offset = with_offset && constant_offset ? 102 : 0,
      .offset = with_offset && !constant_offset ? 103 : 0,
      .sparse = sparse,
   };
   spirv_builder_emit_image_fetch(&b, result_type, 104, &src);
   const uint32_t *words = b.instructions.words;
   unsigned count = words[0] >> 16;
   bool pass = (words[0] & 0xffff) ==
      (sparse ? SpvOpImageSparseFetch : SpvOpImageFetch);
   SpvImageOperandsMask expected_mask =
      (with_offset ? (constant_offset ? SpvImageOperandsConstOffsetMask :
                                      SpvImageOperandsOffsetMask) : 0) |
      (with_sample ? SpvImageOperandsSampleMask : 0);
   pass &= words[5] == expected_mask;
   unsigned index = 6;
   if (with_offset)
      pass &= words[index++] == (constant_offset ? 102 : 103);
   if (with_sample)
      pass &= words[index++] == 101;
   pass &= count == index && b.instructions.num_words == count;
   if (!pass)
      fprintf(stderr, "ImageFetch operands failed: const=%d offset=%d sample=%d sparse=%d\n",
              constant_offset, with_offset, with_sample, sparse);
   ralloc_free(b.mem_ctx);
   return pass;
}

int
main(void)
{
   bool pass = true;
   for (unsigned sparse = 0; sparse < 2; sparse++) {
      pass &= check_fetch(true, true, true, sparse);
      pass &= check_fetch(false, true, true, sparse);
      pass &= check_fetch(false, false, true, sparse);
      pass &= check_fetch(true, true, false, sparse);
   }
   return pass ? 0 : 1;
}
