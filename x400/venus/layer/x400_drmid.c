/* linux-x400: Vulkan layer that tells Zink which DRM node the Venus GPU belongs to.
 * Zink 25.0 picks its VkPhysicalDevice by matching VkPhysicalDeviceDrmPropertiesEXT.renderMajor/Minor with the render node KWin's
 * GBM device sits on (/dev/dri/renderD128 = 226:128 on the SM-X400). Venus does not know that node, so the match fails and Zink
 * gives up ("failed to choose pdev"). This layer fills those fields on the way out of vkGetPhysicalDeviceProperties2.
 * Override the numbers with X400_DRM_RENDER=major:minor, X400_DRM_PRIMARY=major:minor. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetPhysicalDeviceProperties2 next_props2, next_props2_khr;
static PFN_vkGetDeviceProcAddr next_gdpa;
static VkInstance g_inst;

static void parse(const char *env, int def_a, int def_b, int *a, int *b)
{
   const char *s = getenv(env);
   *a = def_a; *b = def_b;
   if (s) sscanf(s, "%d:%d", a, b);
}

static void fix(VkPhysicalDeviceProperties2 *p)
{
   int rma, rmi, pma, pmi;
   parse("X400_DRM_RENDER", 226, 128, &rma, &rmi);
   parse("X400_DRM_PRIMARY", 226, 0, &pma, &pmi);
   for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT) {
         VkPhysicalDeviceDrmPropertiesEXT *d = (void *)s;
         d->hasRender = VK_TRUE; d->renderMajor = rma; d->renderMinor = rmi;
         d->hasPrimary = VK_TRUE; d->primaryMajor = pma; d->primaryMinor = pmi;
      }
   }
}

static VKAPI_ATTR void VKAPI_CALL x_props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{ next_props2(pd, p); fix(p); }
static VKAPI_ATTR void VKAPI_CALL x_props2_khr(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{ next_props2_khr(pd, p); fix(p); }

static VKAPI_ATTR VkResult VKAPI_CALL x_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *al, VkInstance *out)
{
   VkLayerInstanceCreateInfo *lci = (void *)ci->pNext;
   while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
      lci = (void *)lci->pNext;
   if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
   next_gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;
   PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
   VkResult r = next_create(ci, al, out);
   if (r == VK_SUCCESS) {
      g_inst = *out;
      next_props2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*out, "vkGetPhysicalDeviceProperties2");
      next_props2_khr = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
   }
   return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x_gdpa(VkDevice d, const char *n);

static VKAPI_ATTR VkResult VKAPI_CALL x_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *al, VkDevice *out)
{
   VkLayerDeviceCreateInfo *lci = (void *)ci->pNext;
   while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
      lci = (void *)lci->pNext;
   if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   next_gdpa = lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;
   PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)gipa(g_inst, "vkCreateDevice");
   return next_create(pd, ci, al, out);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x_gipa(VkInstance i, const char *n)
{
   if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)x_create_instance;
   if (!strcmp(n, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)x_gipa;
   if (i && !strcmp(n, "vkCreateDevice")) return (PFN_vkVoidFunction)x_create_device;
   if (i && !strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)x_gdpa;
   if (i && next_props2 && !strcmp(n, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)x_props2;
   if (i && next_props2_khr && !strcmp(n, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)x_props2_khr;
   return next_gipa ? next_gipa(i, n) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL x_gdpa(VkDevice d, const char *n)
{
   return next_gdpa ? next_gdpa(d, n) : NULL;
}

VKAPI_ATTR __attribute__((visibility("default"))) VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
   if (v->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
   v->loaderLayerInterfaceVersion = 2;
   v->pfnGetInstanceProcAddr = x_gipa;
   v->pfnGetDeviceProcAddr = x_gdpa;
   v->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
