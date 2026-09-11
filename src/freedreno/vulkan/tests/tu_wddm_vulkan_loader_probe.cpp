/* SPDX-License-Identifier: MIT
 * Validate a staged ICD's actual loader exports and calling convention.
 * No GPU, Vulkan registry registration, or device creation is required.
 */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <vulkan/vk_icd.h>
#include <windows.h>

int
main()
{
   HMODULE icd = LoadLibraryW(L"vulkan_freedreno.dll");
   if (!icd) {
      fprintf(stderr, "ICD LoadLibrary failed: %lu\n", GetLastError());
      return 1;
   }

   const char *names[] = {
      "vk_icdNegotiateLoaderICDInterfaceVersion",
      "vk_icdGetInstanceProcAddr",
      "vk_icdGetPhysicalDeviceProcAddr",
   };
   for (const char *name : names) {
      if (!GetProcAddress(icd, name)) {
         fprintf(stderr, "ICD exact loader export missing: %s\n", name);
         FreeLibrary(icd);
         return 1;
      }
   }

   auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderICDInterfaceVersion>(
      GetProcAddress(icd, names[0]));
   uint32_t version = CURRENT_LOADER_ICD_INTERFACE_VERSION;
   if (negotiate(&version) != VK_SUCCESS || version > CURRENT_LOADER_ICD_INTERFACE_VERSION) {
      fprintf(stderr, "ICD interface negotiation failed: version %u\n", version);
      FreeLibrary(icd);
      return 1;
   }

   auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, names[1]));
   auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
   uint32_t api_version = 0;
   if (!enumerate_version || enumerate_version(&api_version) != VK_SUCCESS ||
       api_version < VK_API_VERSION_1_1) {
      fprintf(stderr, "ICD instance API version query failed\n");
      FreeLibrary(icd);
      return 1;
   }

   printf("ICD loader PASS: interface %u, Vulkan %u.%u.%u (no device tested)\n",
          version, VK_API_VERSION_MAJOR(api_version), VK_API_VERSION_MINOR(api_version),
          VK_API_VERSION_PATCH(api_version));
   FreeLibrary(icd);
   return 0;
}
