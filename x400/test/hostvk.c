// linux-x400: lists what Samsung's own Vulkan driver (the host side of Venus) offers: extensions and the features Zink asks for.
// Built with the NDK, run on the tablet with no session. Usage: hostvk [libvulkan.so]
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
int main(int argc, char **argv)
{
  void *h = dlopen(argc > 1 ? argv[1] : "/system/lib64/libvulkan.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
  VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
  VkInstance inst; if (ci(&ici, NULL, &inst)) { fprintf(stderr, "vkCreateInstance failed\n"); return 1; }
  PFN_vkEnumeratePhysicalDevices ep = (void *)gipa(inst, "vkEnumeratePhysicalDevices");
  PFN_vkEnumerateDeviceExtensionProperties ee = (void *)gipa(inst, "vkEnumerateDeviceExtensionProperties");
  PFN_vkGetPhysicalDeviceProperties gp = (void *)gipa(inst, "vkGetPhysicalDeviceProperties");
  PFN_vkGetPhysicalDeviceFeatures2 gf2 = (void *)gipa(inst, "vkGetPhysicalDeviceFeatures2");
  uint32_t n = 1; VkPhysicalDevice pd; ep(inst, &n, &pd);
  VkPhysicalDeviceProperties p; gp(pd, &p);
  printf("device %s apiVersion %u.%u.%u driver 0x%x\n", p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
  uint32_t ne = 0; ee(pd, NULL, &ne, NULL); VkExtensionProperties *e = calloc(ne, sizeof *e); ee(pd, NULL, &ne, e);
  printf("%u device extensions\n", ne);
  const char *want[] = { "VK_KHR_maintenance5", "VK_EXT_queue_family_foreign", "VK_EXT_external_memory_dma_buf", "VK_EXT_robustness2", "VK_KHR_robustness2", "VK_EXT_image_robustness", "VK_EXT_line_rasterization",
    "VK_EXT_custom_border_color", "VK_EXT_provoking_vertex", "VK_EXT_transform_feedback", "VK_EXT_conditional_rendering", "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf","VK_EXT_queue_family_foreign","VK_KHR_external_memory_fd", "VK_EXT_image_drm_format_modifier", "VK_EXT_physical_device_drm", "VK_KHR_timeline_semaphore",
    "VK_KHR_maintenance4", "VK_KHR_dynamic_rendering", "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2", "VK_EXT_extended_dynamic_state3",
    "VK_EXT_scalar_block_layout", "VK_KHR_draw_indirect_count", "VK_EXT_index_type_uint8", "VK_KHR_swapchain", "VK_EXT_descriptor_indexing", NULL };
  for (int i = 0; want[i]; i++) { int f = 0; for (uint32_t j = 0; j < ne; j++) if (!strcmp(e[j].extensionName, want[i])) f = 1; printf("  %-38s %s\n", want[i], f ? "YES" : "no"); }
  // robustness2 features, queried through the chain only when the extension is there
  VkPhysicalDeviceRobustness2FeaturesEXT rb = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
  VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rb };
  int has = 0; for (uint32_t j = 0; j < ne; j++) if (!strcmp(e[j].extensionName, "VK_EXT_robustness2")) has = 1;
  if (has) { gf2(pd, &f2); printf("robustness2: robustBufferAccess2 %u robustImageAccess2 %u nullDescriptor %u\n", rb.robustBufferAccess2, rb.robustImageAccess2, rb.nullDescriptor); }
  else printf("robustness2: the driver does not offer the extension, so nullDescriptor cannot come from it\n");
  return 0;
}
