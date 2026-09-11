/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include <cstdio>
#include <initializer_list>

enum VkResult { VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_PIPELINE_COMPILE_REQUIRED };
constexpr uint64_t VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR = 1;
constexpr int MESA_SHADER_COMPUTE = 5;
struct nir_shader {};
struct tu_shader {};
struct ir3_shader_key {};
struct tu_shader_key {};
struct tu_shader_info {};
struct fake_pipeline { struct { void *executables_mem_ctx; } base; };

static nir_shader valid_nir;
static tu_shader valid_shader;
static bool fail_conversion, fail_shader, saw_null;
static unsigned converts, disassembles, lowers, creates, inserts;

static nir_shader *tu_spirv_to_nir(void *, void *, uint64_t, void *,
                                  tu_shader_key *, int)
{
   converts++;
   return fail_conversion ? nullptr : &valid_nir;
}
static char *nir_shader_as_str(nir_shader *nir, void *)
{
   disassembles++;
   saw_null |= nir == nullptr;
   return nullptr;
}
static void tu_lower_nir(void *, nir_shader *nir, tu_shader_key *,
                         ir3_shader_key *, tu_shader_info *)
{
   lowers++;
   saw_null |= nir == nullptr;
}
static VkResult tu_shader_create(void *, tu_shader **shader, nir_shader *nir,
                                 tu_shader_key *, tu_shader_info *, ir3_shader_key *,
                                 const void *, size_t, void *, bool)
{
   creates++;
   saw_null |= nir == nullptr;
   *shader = fail_shader ? nullptr : &valid_shader;
   return fail_shader ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
}
static tu_shader *tu_pipeline_cache_insert(void *, tu_shader *shader)
{
   inserts++;
   return shader;
}

/* Only dependencies are simulated. Execute the actual production cache-miss
 * branch, including its early-return ordering and jump to failure cleanup. */
static VkResult run(uint64_t flags, bool executable_info, bool cache_hit,
                    bool *failed)
{
   void *dev = nullptr, *pipeline_mem_ctx = nullptr, *stage_info = nullptr;
   void *layout = nullptr, *cache = nullptr;
   fake_pipeline value = {}, *pipeline = &value;
   tu_shader_key key = {};
   unsigned char pipeline_blake3[32] = {};
   tu_shader *shader = cache_hit ? &valid_shader : nullptr;
   char *nir_initial_disasm = nullptr;
   VkResult result = VK_SUCCESS;
   // PRODUCTION_CACHE_MISS
   (void) nir_initial_disasm;
   *failed = false;
   return VK_SUCCESS;
fail:
   *failed = true;
   return result;
}

int main()
{
   unsigned checks = 0;
   for (bool capture : {false, true}) {
      for (unsigned scenario = 0; scenario < 5; scenario++) {
         fail_conversion = scenario == 0;
         fail_shader = scenario == 1;
         const bool cache_hit = scenario == 2;
         const bool compile_required = scenario == 3;
         converts = disassembles = lowers = creates = inserts = 0;
         saw_null = false;
         bool failed = false;
         const VkResult result = run(compile_required ? 1 : 0, capture, cache_hit, &failed);
         const VkResult expected = compile_required ? VK_PIPELINE_COMPILE_REQUIRED :
            fail_conversion || fail_shader ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
         const unsigned converted = cache_hit || compile_required ? 0 : 1;
         const unsigned lowered = converted && !fail_conversion ? 1 : 0;
         const unsigned inserted = lowered && !fail_shader ? 1 : 0;
         checks++;
         if (result != expected || failed != (expected != VK_SUCCESS) || saw_null ||
             converts != converted || lowers != lowered || creates != lowered ||
             disassembles != (capture ? lowered : 0) || inserts != inserted) {
            fprintf(stderr, "FAIL scenario=%u capture=%d result=%d expected=%d null=%d\n",
                    scenario, capture, result, expected, saw_null);
            return 1;
         }
      }
   }
   printf("Compute pipeline production failure ordering: %u checks PASS\n", checks);
   return 0;
}
