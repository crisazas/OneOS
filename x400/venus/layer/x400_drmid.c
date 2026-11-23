/* linux-x400: Vulkan layer that tells Zink which DRM node the Venus GPU belongs to.
 * Zink 25.0 picks its VkPhysicalDevice by matching VkPhysicalDeviceDrmPropertiesEXT.renderMajor/Minor with the render node KWin's
 * GBM device sits on (/dev/dri/renderD128 = 226:128 on the SM-X400). Venus does not know that node, so the match fails and Zink
 * gives up ("failed to choose pdev"). This layer fills those fields on the way out of vkGetPhysicalDeviceProperties2.
 * Override the numbers with X400_DRM_RENDER=major:minor, X400_DRM_PRIMARY=major:minor.
 *
 * It also traces what Zink (or any app) asks of the device. Every call listed in RI_LIST/V_LIST goes through a wrapper that counts it;
 * a VkResult other than VK_SUCCESS is printed to stderr ("x400-vk: <call> returned <n>", first 20 per call), and with X400_VKTRACE=1
 * a line with all the counters is printed on the first 16 vkQueueSubmit and then at each power of two. The wrappers forward eight
 * integer registers to the real function: all of these calls take at most eight integer/pointer arguments, so on aarch64 (AAPCS64) that
 * is the same call; they would be wrong for a call taking floats or more than eight arguments, none of which are in the lists. */
#include <stdlib.h>
#include <pthread.h>
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

typedef uint64_t u64;
typedef int32_t (*gen_ri)(u64, u64, u64, u64, u64, u64, u64, u64);
typedef void (*gen_v)(u64, u64, u64, u64, u64, u64, u64, u64);
struct fn_info { void *real; u64 calls, fails; };
static int trace_on;

#define RI_LIST(X) \
   X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkCreateShaderModule) X(vkQueueSubmit) X(vkQueueSubmit2) \
   X(vkEndCommandBuffer) X(vkBeginCommandBuffer) X(vkAllocateMemory) X(vkBindImageMemory) X(vkBindImageMemory2) \
   X(vkBindBufferMemory) X(vkBindBufferMemory2) X(vkCreateImage) X(vkCreateBuffer) X(vkCreateImageView) X(vkCreateBufferView) \
   X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool) \
   X(vkAllocateDescriptorSets) X(vkCreateFence) X(vkCreateSemaphore) X(vkWaitSemaphores) X(vkWaitForFences) \
   X(vkGetSemaphoreFdKHR) X(vkImportSemaphoreFdKHR) X(vkGetMemoryFdKHR) X(vkMapMemory) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
   X(vkCreateRenderPass) X(vkCreateRenderPass2) X(vkCreateFramebuffer) X(vkCreateQueryPool) X(vkGetQueryPoolResults) \
   X(vkCreateCommandPool) X(vkAllocateCommandBuffers)
#define V_LIST(X) \
   X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdBeginRenderPass) \
   X(vkCmdBeginRenderPass2) X(vkCmdBeginRendering) X(vkCmdBindPipeline) X(vkCmdDispatch) \
   X(vkCmdBindVertexBuffers) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdEndRendering) X(vkCmdSetViewport) \
   X(vkCmdSetScissor) X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) X(vkCmdPushConstants) X(vkCmdBindDescriptorSets) \
   X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) \
   X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
   X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetDepthBiasEnable) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) \
   X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdClearColorImage) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdFillBuffer) X(vkCmdPipelineBarrier2) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilCompareMask) X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) X(vkCmdSetDepthBounds)

static void note_fail(const char *n, int32_t r, u64 nth)
{
   if (nth <= 20)
      fprintf(stderr, "x400-vk: %s returned %d (#%llu)\n", n, r, (unsigned long long)nth);
}

#define DECL_RI(n) \
   static struct fn_info info_##n; \
   static VKAPI_ATTR int32_t w_##n(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) \
   { \
      int32_t r = ((gen_ri)info_##n.real)(a, b, c, d, e, f, g, h); \
      __atomic_add_fetch(&info_##n.calls, 1, __ATOMIC_RELAXED); \
      if (r) note_fail(#n, r, __atomic_add_fetch(&info_##n.fails, 1, __ATOMIC_RELAXED)); \
      maybe_summary(#n); \
      return r; \
   }
#define DECL_V(n) \
   static struct fn_info info_##n; \
   static VKAPI_ATTR void w_##n(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) \
   { \
      __atomic_add_fetch(&info_##n.calls, 1, __ATOMIC_RELAXED); \
      ((gen_v)info_##n.real)(a, b, c, d, e, f, g, h); \
   }

static void maybe_summary(const char *n);
RI_LIST(DECL_RI)
V_LIST(DECL_V)

/* Wrappers that print their arguments (first 40 of each), after counting: an empty viewport or scissor, or a draw of zero vertices, would
 * explain a draw that writes nothing. They replace the generic counters above for these calls (see trace_lookup). */
static u64 cmd_seq;
static void order(const char *what, const void *cb)
{
   if (!trace_on) return;
   u64 n = __atomic_add_fetch(&cmd_seq, 1, __ATOMIC_RELAXED);
   if (n <= 300) fprintf(stderr, "x400-vk: #%llu cb=%p %s\n", (unsigned long long)n, cb, what);
}
static u64 arg_prints_draw, arg_prints_vp, arg_prints_sc, arg_prints_rend;
static VKAPI_ATTR void w_arg_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
   __atomic_add_fetch(&info_vkCmdDraw.calls, 1, __ATOMIC_RELAXED);
   order("vkCmdDraw", cb);
   if (trace_on && __atomic_add_fetch(&arg_prints_draw, 1, __ATOMIC_RELAXED) <= 40)
      fprintf(stderr, "x400-vk: vkCmdDraw vertexCount=%u instanceCount=%u firstVertex=%u firstInstance=%u\n", vc, ic, fv, fi);
   ((void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t))info_vkCmdDraw.real)(cb, vc, ic, fv, fi);
}
static void print_viewports(const char *who, uint32_t first, uint32_t n, const VkViewport *v)
{
   for (uint32_t i = 0; i < n && i < 4; i++)
      fprintf(stderr, "x400-vk: %s [%u] x=%.1f y=%.1f w=%.1f h=%.1f z=%.2f..%.2f\n", who, first + i, v[i].x, v[i].y, v[i].width, v[i].height, v[i].minDepth, v[i].maxDepth);
}
static void print_scissors(const char *who, uint32_t first, uint32_t n, const VkRect2D *r)
{
   for (uint32_t i = 0; i < n && i < 4; i++)
      fprintf(stderr, "x400-vk: %s [%u] off=(%d,%d) ext=%ux%u\n", who, first + i, r[i].offset.x, r[i].offset.y, r[i].extent.width, r[i].extent.height);
}
static VKAPI_ATTR void w_arg_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
   __atomic_add_fetch(&info_vkCmdSetViewport.calls, 1, __ATOMIC_RELAXED);
   if (trace_on && __atomic_add_fetch(&arg_prints_vp, 1, __ATOMIC_RELAXED) <= 40) print_viewports("vkCmdSetViewport", first, n, v);
   ((void (*)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport *))info_vkCmdSetViewport.real)(cb, first, n, v);
}
static VKAPI_ATTR void w_arg_vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v)
{
   __atomic_add_fetch(&info_vkCmdSetViewportWithCount.calls, 1, __ATOMIC_RELAXED);
   if (trace_on && __atomic_add_fetch(&arg_prints_vp, 1, __ATOMIC_RELAXED) <= 40) print_viewports("vkCmdSetViewportWithCount", 0, n, v);
   ((void (*)(VkCommandBuffer, uint32_t, const VkViewport *))info_vkCmdSetViewportWithCount.real)(cb, n, v);
}
static VKAPI_ATTR void w_arg_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r)
{
   __atomic_add_fetch(&info_vkCmdSetScissor.calls, 1, __ATOMIC_RELAXED);
   if (trace_on && __atomic_add_fetch(&arg_prints_sc, 1, __ATOMIC_RELAXED) <= 40) print_scissors("vkCmdSetScissor", first, n, r);
   ((void (*)(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *))info_vkCmdSetScissor.real)(cb, first, n, r);
}
static VKAPI_ATTR void w_arg_vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D *r)
{
   __atomic_add_fetch(&info_vkCmdSetScissorWithCount.calls, 1, __ATOMIC_RELAXED);
   if (trace_on && __atomic_add_fetch(&arg_prints_sc, 1, __ATOMIC_RELAXED) <= 40) print_scissors("vkCmdSetScissorWithCount", 0, n, r);
   ((void (*)(VkCommandBuffer, uint32_t, const VkRect2D *))info_vkCmdSetScissorWithCount.real)(cb, n, r);
}
static VKAPI_ATTR void w_arg_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
   __atomic_add_fetch(&info_vkCmdBeginRendering.calls, 1, __ATOMIC_RELAXED);
   order("vkCmdBeginRendering", cb);
   if (trace_on && ri && __atomic_add_fetch(&arg_prints_rend, 1, __ATOMIC_RELAXED) <= 40) {
      const VkRenderingAttachmentInfo *c = ri->pColorAttachments;
      fprintf(stderr, "x400-vk: vkCmdBeginRendering area=(%d,%d %ux%u) layers=%u views=0x%x colors=%u color0{load=%d store=%d layout=%d} depth=%s\n",
              ri->renderArea.offset.x, ri->renderArea.offset.y, ri->renderArea.extent.width, ri->renderArea.extent.height, ri->layerCount,
              ri->viewMask, ri->colorAttachmentCount, c ? (int)c->loadOp : -1, c ? (int)c->storeOp : -1, c ? (int)c->imageLayout : -1,
              ri->pDepthAttachment ? "yes" : "no");
   }
   ((void (*)(VkCommandBuffer, const VkRenderingInfo *))info_vkCmdBeginRendering.real)(cb, ri);
}

static u64 arg_prints_pipe, arg_prints_state;
static void dump_gpi(const VkGraphicsPipelineCreateInfo *g)
{
   fprintf(stderr, "x400-vk: pipeline flags=0x%x layout=%p renderPass=%p subpass=%u stages=%u:", g->flags, (void *)g->layout, (void *)g->renderPass, g->subpass, g->stageCount);
   for (uint32_t i = 0; i < g->stageCount; i++)
      fprintf(stderr, " {stage=0x%x entry=%s}", g->pStages[i].stage, g->pStages[i].pName);
   fprintf(stderr, "\n");
   const VkPipelineVertexInputStateCreateInfo *vi = g->pVertexInputState;
   if (vi) fprintf(stderr, "x400-vk:   vertexInput bindings=%u attribs=%u\n", vi->vertexBindingDescriptionCount, vi->vertexAttributeDescriptionCount);
   else fprintf(stderr, "x400-vk:   vertexInput NULL\n");
   const VkPipelineInputAssemblyStateCreateInfo *ia = g->pInputAssemblyState;
   if (ia) fprintf(stderr, "x400-vk:   inputAssembly topology=%d primitiveRestart=%u\n", ia->topology, ia->primitiveRestartEnable);
   const VkPipelineViewportStateCreateInfo *vp = g->pViewportState;
   if (vp) fprintf(stderr, "x400-vk:   viewportState viewports=%u scissors=%u pViewports=%p pScissors=%p\n", vp->viewportCount, vp->scissorCount, (const void *)vp->pViewports, (const void *)vp->pScissors);
   else fprintf(stderr, "x400-vk:   viewportState NULL\n");
   const VkPipelineRasterizationStateCreateInfo *rs = g->pRasterizationState;
   if (rs) fprintf(stderr, "x400-vk:   raster depthClamp=%u DISCARD=%u polygonMode=%d cullMode=%u frontFace=%d depthBias=%u lineWidth=%.1f\n", rs->depthClampEnable, rs->rasterizerDiscardEnable, rs->polygonMode, rs->cullMode, rs->frontFace, rs->depthBiasEnable, rs->lineWidth);
   else fprintf(stderr, "x400-vk:   raster NULL\n");
   const VkPipelineMultisampleStateCreateInfo *ms = g->pMultisampleState;
   if (ms) fprintf(stderr, "x400-vk:   multisample samples=%u sampleShading=%u mask=%p alphaToCoverage=%u alphaToOne=%u\n", ms->rasterizationSamples, ms->sampleShadingEnable, (const void *)ms->pSampleMask, ms->alphaToCoverageEnable, ms->alphaToOneEnable);
   const VkPipelineDepthStencilStateCreateInfo *ds = g->pDepthStencilState;
   if (ds) fprintf(stderr, "x400-vk:   depthStencil depthTest=%u depthWrite=%u compare=%d stencilTest=%u\n", ds->depthTestEnable, ds->depthWriteEnable, ds->depthCompareOp, ds->stencilTestEnable);
   const VkPipelineColorBlendStateCreateInfo *cb = g->pColorBlendState;
   if (cb) {
      fprintf(stderr, "x400-vk:   blend logicOp=%u attachments=%u:", cb->logicOpEnable, cb->attachmentCount);
      for (uint32_t i = 0; i < cb->attachmentCount && i < 8; i++)
         fprintf(stderr, " {blend=%u writeMask=0x%x}", cb->pAttachments[i].blendEnable, cb->pAttachments[i].colorWriteMask);
      fprintf(stderr, "\n");
   } else fprintf(stderr, "x400-vk:   blend NULL\n");
   const VkPipelineDynamicStateCreateInfo *dy = g->pDynamicState;
   if (dy) {
      fprintf(stderr, "x400-vk:   dynamic(%u):", dy->dynamicStateCount);
      for (uint32_t i = 0; i < dy->dynamicStateCount; i++) fprintf(stderr, " %d", dy->pDynamicStates[i]);
      fprintf(stderr, "\n");
   }
   for (const VkBaseInStructure *n = g->pNext; n; n = n->pNext) {
      if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
         const VkPipelineRenderingCreateInfo *r = (const void *)n;
         fprintf(stderr, "x400-vk:   rendering viewMask=0x%x colors=%u depthFmt=%d stencilFmt=%d formats:", r->viewMask, r->colorAttachmentCount, r->depthAttachmentFormat, r->stencilAttachmentFormat);
         for (uint32_t i = 0; i < r->colorAttachmentCount && i < 8; i++) fprintf(stderr, " %d", r->pColorAttachmentFormats[i]);
         fprintf(stderr, "\n");
      } else
         fprintf(stderr, "x400-vk:   pNext sType=%d\n", n->sType);
   }
}
static VKAPI_ATTR int32_t w_arg_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *g, const VkAllocationCallbacks *a, VkPipeline *out)
{
   if (trace_on && __atomic_add_fetch(&arg_prints_pipe, 1, __ATOMIC_RELAXED) <= 10)
      for (uint32_t i = 0; i < n; i++) dump_gpi(&g[i]);
   /* X400_DYN_OFF=id,id,...  removes those VkDynamicState values from every graphics pipeline before it reaches the driver, so the pipeline's own
    * (already filled in) static state is used instead. X400_PIPE_OFF=bias clears depthBiasEnable. A bisection tool on the real program: Zink's
    * later vkCmdSet* calls for a state that is no longer dynamic are not valid Vulkan, but they are passed through and ignored. */
   enum { MAXP = 8 };
   VkGraphicsPipelineCreateInfo gcopy[MAXP]; VkPipelineDynamicStateCreateInfo dcopy[MAXP]; VkDynamicState dlist[MAXP][80];
   VkPipelineViewportStateCreateInfo vcopy[MAXP]; VkPipelineRasterizationStateCreateInfo rcopy[MAXP];
   VkViewport svp = { 0, 0, 256, 256, 0, 1 }; VkRect2D ssc = { { 0, 0 }, { 256, 256 } };
   const char *off = getenv("X400_DYN_OFF"), *poff = getenv("X400_PIPE_OFF");
   if (((off && *off) || (poff && *poff)) && n <= MAXP) {
      if (getenv("X400_VP_SIZE")) { svp.width = svp.height = (float)atoi(getenv("X400_VP_SIZE")); ssc.extent.width = ssc.extent.height = (uint32_t)atoi(getenv("X400_VP_SIZE")); }
      for (uint32_t i = 0; i < n; i++) {
         gcopy[i] = g[i];
         if (g[i].pDynamicState && off && *off) {
            unsigned kept = 0; int drop_vp = 0;
            for (uint32_t k = 0; k < g[i].pDynamicState->dynamicStateCount && kept < 80; k++) {
               int v = (int)g[i].pDynamicState->pDynamicStates[k], hit = 0;
               char tmp[512]; strncpy(tmp, off, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
               for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) if (atoi(t) == v) hit = 1;
               if (hit) { if (v == 1000267003 || v == 1000267004) drop_vp = 1; continue; }
               dlist[i][kept++] = (VkDynamicState)v;
            }
            dcopy[i] = *g[i].pDynamicState; dcopy[i].dynamicStateCount = kept; dcopy[i].pDynamicStates = dlist[i];
            gcopy[i].pDynamicState = &dcopy[i];
            if (drop_vp && g[i].pViewportState) { vcopy[i] = *g[i].pViewportState; vcopy[i].viewportCount = vcopy[i].scissorCount = 1; vcopy[i].pViewports = &svp; vcopy[i].pScissors = &ssc; gcopy[i].pViewportState = &vcopy[i]; }
         }
         if (poff && strstr(poff, "bias") && g[i].pRasterizationState) { rcopy[i] = *g[i].pRasterizationState; rcopy[i].depthBiasEnable = VK_FALSE; gcopy[i].pRasterizationState = &rcopy[i]; }
      }
      g = gcopy;
      if (trace_on) fprintf(stderr, "x400-vk: pipelines created with X400_DYN_OFF=%s X400_PIPE_OFF=%s\n", off ? off : "", poff ? poff : "");
   }
   int32_t r = ((int32_t (*)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *))info_vkCreateGraphicsPipelines.real)(d, c, n, g, a, out);
   __atomic_add_fetch(&info_vkCreateGraphicsPipelines.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkCreateGraphicsPipelines", r, __atomic_add_fetch(&info_vkCreateGraphicsPipelines.fails, 1, __ATOMIC_RELAXED));
   return r;
}
#define STATE1(n, T) \
   static VKAPI_ATTR void w_arg_##n(VkCommandBuffer cb, T v) \
   { \
      __atomic_add_fetch(&info_##n.calls, 1, __ATOMIC_RELAXED); \
      if (trace_on && __atomic_add_fetch(&arg_prints_state, 1, __ATOMIC_RELAXED) <= 60) fprintf(stderr, "x400-vk: %s(%u)\n", #n, (unsigned)v); \
      ((void (*)(VkCommandBuffer, T))info_##n.real)(cb, v); \
   }
STATE1(vkCmdSetRasterizerDiscardEnable, VkBool32)
STATE1(vkCmdSetCullMode, uint32_t)
STATE1(vkCmdSetFrontFace, uint32_t)
STATE1(vkCmdSetPrimitiveTopology, uint32_t)
STATE1(vkCmdSetDepthWriteEnable, VkBool32)
STATE1(vkCmdSetDepthBoundsTestEnable, VkBool32)
STATE1(vkCmdSetStencilTestEnable, VkBool32)

static VKAPI_ATTR void w_arg_vkCmdSetLineWidth(VkCommandBuffer cb, float w)
{
   __atomic_add_fetch(&info_vkCmdSetLineWidth.calls, 1, __ATOMIC_RELAXED);
   ((void (*)(VkCommandBuffer, float))info_vkCmdSetLineWidth.real)(cb, w);
}
static VKAPI_ATTR void w_arg_vkCmdSetDepthBias(VkCommandBuffer cb, float a, float b, float c)
{
   __atomic_add_fetch(&info_vkCmdSetDepthBias.calls, 1, __ATOMIC_RELAXED);
   ((void (*)(VkCommandBuffer, float, float, float))info_vkCmdSetDepthBias.real)(cb, a, b, c);
}
static VKAPI_ATTR void w_arg_vkCmdSetDepthBounds(VkCommandBuffer cb, float a, float b)
{
   __atomic_add_fetch(&info_vkCmdSetDepthBounds.calls, 1, __ATOMIC_RELAXED);
   ((void (*)(VkCommandBuffer, float, float))info_vkCmdSetDepthBounds.real)(cb, a, b);
}

static u64 arg_prints_img;
static VKAPI_ATTR int32_t w_arg_vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
   int32_t r = ((int32_t (*)(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *))info_vkCreateImage.real)(d, ci, a, out);
   __atomic_add_fetch(&info_vkCreateImage.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkCreateImage", r, __atomic_add_fetch(&info_vkCreateImage.fails, 1, __ATOMIC_RELAXED));
   if (trace_on && __atomic_add_fetch(&arg_prints_img, 1, __ATOMIC_RELAXED) <= 12) {
      fprintf(stderr, "x400-vk: vkCreateImage -> %p flags=0x%x type=%d fmt=%d %ux%ux%u mips=%u layers=%u samples=%u tiling=%d usage=0x%x sharing=%d initial=%d",
              (void *)(out ? *out : 0), ci->flags, ci->imageType, ci->format, ci->extent.width, ci->extent.height, ci->extent.depth, ci->mipLevels, ci->arrayLayers,
              ci->samples, ci->tiling, ci->usage, ci->sharingMode, ci->initialLayout);
      for (const VkBaseInStructure *n = ci->pNext; n; n = n->pNext) {
         if (n->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
            fprintf(stderr, " ext{handles=0x%x}", ((const VkExternalMemoryImageCreateInfo *)n)->handleTypes);
         else if (n->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT)
            fprintf(stderr, " drmModList{n=%u}", ((const VkImageDrmFormatModifierListCreateInfoEXT *)n)->drmFormatModifierCount);
         else if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
            fprintf(stderr, " fmtList{n=%u}", ((const VkImageFormatListCreateInfo *)n)->viewFormatCount);
         else
            fprintf(stderr, " pNext{%d}", n->sType);
      }
      fprintf(stderr, "\n");
   }
   return r;
}
static u64 arg_prints_view;
static VKAPI_ATTR int32_t w_arg_vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
   int32_t r = ((int32_t (*)(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *))info_vkCreateImageView.real)(d, ci, a, out);
   __atomic_add_fetch(&info_vkCreateImageView.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkCreateImageView", r, __atomic_add_fetch(&info_vkCreateImageView.fails, 1, __ATOMIC_RELAXED));
   if (trace_on && __atomic_add_fetch(&arg_prints_view, 1, __ATOMIC_RELAXED) <= 12)
      fprintf(stderr, "x400-vk: vkCreateImageView image=%p type=%d fmt=%d swizzle=(%d,%d,%d,%d) aspect=0x%x mips=%u..+%u layers=%u..+%u\n",
              (void *)ci->image, ci->viewType, ci->format, ci->components.r, ci->components.g, ci->components.b, ci->components.a,
              ci->subresourceRange.aspectMask, ci->subresourceRange.baseMipLevel, ci->subresourceRange.levelCount, ci->subresourceRange.baseArrayLayer, ci->subresourceRange.layerCount);
   return r;
}
static u64 arg_prints_bar;
static VKAPI_ATTR void w_arg_vkCmdPipelineBarrier(VkCommandBuffer cb, uint32_t src, uint32_t dst, uint32_t dep, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im)
{
   __atomic_add_fetch(&info_vkCmdPipelineBarrier.calls, 1, __ATOMIC_RELAXED);
   if (trace_on && ni && __atomic_add_fetch(&arg_prints_bar, 1, __ATOMIC_RELAXED) <= 30)
      for (uint32_t i = 0; i < ni && i < 4; i++)
         fprintf(stderr, "x400-vk: vkCmdPipelineBarrier stages 0x%x->0x%x img=%p %d->%d access 0x%x->0x%x queues %u->%u\n", src, dst, (void *)im[i].image,
                 im[i].oldLayout, im[i].newLayout, im[i].srcAccessMask, im[i].dstAccessMask, im[i].srcQueueFamilyIndex, im[i].dstQueueFamilyIndex);
   ((void (*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *))info_vkCmdPipelineBarrier.real)(cb, src, dst, dep, nm, m, nb, b, ni, im);
}

#define ORD0(n) \
   static VKAPI_ATTR void w_ord_##n(u64 cb, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) \
   { __atomic_add_fetch(&info_##n.calls, 1, __ATOMIC_RELAXED); order(#n + 2, (void *)cb); ((gen_v)info_##n.real)(cb, b, c, d, e, f, g, h); }
ORD0(vkCmdCopyImageToBuffer) ORD0(vkCmdCopyBufferToImage) ORD0(vkCmdCopyBuffer) ORD0(vkCmdCopyImage) ORD0(vkCmdClearColorImage)
ORD0(vkCmdBlitImage) ORD0(vkCmdResolveImage) ORD0(vkCmdFillBuffer) ORD0(vkCmdPipelineBarrier2)

#define TRK_N 512
static struct { u64 mem; void *ptr; u64 size; } trk_map[TRK_N];
static struct { u64 buf; u64 mem; u64 off; } trk_buf[TRK_N];
static unsigned trk_nmap, trk_nbuf;
static pthread_mutex_t trk_mtx = PTHREAD_MUTEX_INITIALIZER;
static void trk_add_map(u64 mem, void *ptr, u64 size)
{
   pthread_mutex_lock(&trk_mtx);
   if (trk_nmap < TRK_N) { trk_map[trk_nmap].mem = mem; trk_map[trk_nmap].ptr = ptr; trk_map[trk_nmap].size = size; trk_nmap++; }
   pthread_mutex_unlock(&trk_mtx);
}
static void trk_add_buf(u64 buf, u64 mem, u64 off)
{
   pthread_mutex_lock(&trk_mtx);
   if (trk_nbuf < TRK_N) { trk_buf[trk_nbuf].buf = buf; trk_buf[trk_nbuf].mem = mem; trk_buf[trk_nbuf].off = off; trk_nbuf++; }
   pthread_mutex_unlock(&trk_mtx);
}
/* the host-visible bytes a buffer holds at `off`, or NULL if its memory is not mapped */
static const unsigned char *trk_bytes(u64 buf, u64 off)
{
   const unsigned char *r = NULL;
   pthread_mutex_lock(&trk_mtx);
   for (unsigned i = 0; i < trk_nbuf && !r; i++) {
      if (trk_buf[i].buf != buf) continue;
      for (unsigned j = 0; j < trk_nmap; j++)
         if (trk_map[j].mem == trk_buf[i].mem) { r = (const unsigned char *)trk_map[j].ptr + trk_buf[i].off + off; break; }
   }
   pthread_mutex_unlock(&trk_mtx);
   return r;
}

static u64 arg_prints_vb;
static VKAPI_ATTR void w_arg_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *off, const VkDeviceSize *size, const VkDeviceSize *stride)
{
   __atomic_add_fetch(&info_vkCmdBindVertexBuffers2.calls, 1, __ATOMIC_RELAXED);
   order("vkCmdBindVertexBuffers2", cb);
   if (trace_on && __atomic_load_n(&arg_prints_vb, __ATOMIC_RELAXED) < 30)
      for (uint32_t i = 0; i < n && i < 2; i++) {
         const unsigned char *bytes = trk_bytes((u64)b[i], off ? off[i] : 0);
         if (bytes) {
            const float *f = (const float *)bytes;
            fprintf(stderr, "x400-vk:   vertex data as the CPU sees it at the draw: (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f)\n", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
         } else {
            fprintf(stderr, "x400-vk:   vertex buffer %p is not mapped (no CPU view)\n", (void *)b[i]);
         }
      }
   if (trace_on && __atomic_add_fetch(&arg_prints_vb, 1, __ATOMIC_RELAXED) <= 30)
      for (uint32_t i = 0; i < n && i < 4; i++)
         fprintf(stderr, "x400-vk: vkCmdBindVertexBuffers2 first=%u [%u] buffer=%p offset=%llu size=%s%llu stride=%s%llu\n", first, i, (void *)b[i],
                 (unsigned long long)(off ? off[i] : 0), size ? "" : "NULL:", (unsigned long long)(size ? size[i] : 0),
                 stride ? "" : "NULL:", (unsigned long long)(stride ? stride[i] : 0));
   ((void (*)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *, const VkDeviceSize *, const VkDeviceSize *))info_vkCmdBindVertexBuffers2.real)(cb, first, n, b, off, size, stride);
}
static u64 arg_prints_buf;
static VKAPI_ATTR int32_t w_arg_vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{
   int32_t r = ((int32_t (*)(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *))info_vkCreateBuffer.real)(d, ci, a, out);
   __atomic_add_fetch(&info_vkCreateBuffer.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkCreateBuffer", r, __atomic_add_fetch(&info_vkCreateBuffer.fails, 1, __ATOMIC_RELAXED));
   if (trace_on && __atomic_add_fetch(&arg_prints_buf, 1, __ATOMIC_RELAXED) <= 30)
      fprintf(stderr, "x400-vk: vkCreateBuffer -> %p size=%llu usage=0x%x flags=0x%x sharing=%d\n", (void *)(out ? *out : 0), (unsigned long long)ci->size, ci->usage, ci->flags, ci->sharingMode);
   return r;
}
static VKAPI_ATTR int32_t w_arg_vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
   int32_t r = ((int32_t (*)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize))info_vkBindBufferMemory.real)(d, b, m, off);
   __atomic_add_fetch(&info_vkBindBufferMemory.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkBindBufferMemory", r, __atomic_add_fetch(&info_vkBindBufferMemory.fails, 1, __ATOMIC_RELAXED));
   if (!r) trk_add_buf((u64)b, (u64)m, off);
   if (trace_on) fprintf(stderr, "x400-vk: vkBindBufferMemory buffer=%p memory=%p offset=%llu\n", (void *)b, (void *)m, (unsigned long long)off);
   return r;
}
static VKAPI_ATTR int32_t w_arg_vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, uint32_t flags, void **pp)
{
   int32_t r = ((int32_t (*)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void **))info_vkMapMemory.real)(d, m, off, size, flags, pp);
   __atomic_add_fetch(&info_vkMapMemory.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkMapMemory", r, __atomic_add_fetch(&info_vkMapMemory.fails, 1, __ATOMIC_RELAXED));
   if (!r && pp) trk_add_map((u64)m, (char *)*pp - off, size);
   if (trace_on) fprintf(stderr, "x400-vk: vkMapMemory memory=%p offset=%llu size=%llu -> %p\n", (void *)m, (unsigned long long)off, (unsigned long long)size, pp ? *pp : NULL);
   return r;
}
static u64 arg_prints_alloc;
static VKAPI_ATTR int32_t w_arg_vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
   int32_t r = ((int32_t (*)(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *))info_vkAllocateMemory.real)(d, ai, a, out);
   __atomic_add_fetch(&info_vkAllocateMemory.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkAllocateMemory", r, __atomic_add_fetch(&info_vkAllocateMemory.fails, 1, __ATOMIC_RELAXED));
   if (trace_on && __atomic_add_fetch(&arg_prints_alloc, 1, __ATOMIC_RELAXED) <= 30) {
      fprintf(stderr, "x400-vk: vkAllocateMemory -> %p size=%llu type=%u", (void *)(out ? *out : 0), (unsigned long long)ai->allocationSize, ai->memoryTypeIndex);
      for (const VkBaseInStructure *n = ai->pNext; n; n = n->pNext) {
         if (n->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) fprintf(stderr, " export{0x%x}", ((const VkExportMemoryAllocateInfo *)n)->handleTypes);
         else if (n->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) fprintf(stderr, " dedicated");
         else if (n->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) fprintf(stderr, " importFd");
         else fprintf(stderr, " pNext{%d}", n->sType);
      }
      fprintf(stderr, "\n");
   }
   return r;
}

static VKAPI_ATTR int32_t w_arg_vkBindImageMemory(VkDevice d, VkImage im, VkDeviceMemory m, VkDeviceSize off)
{
   int32_t r = ((int32_t (*)(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize))info_vkBindImageMemory.real)(d, im, m, off);
   __atomic_add_fetch(&info_vkBindImageMemory.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkBindImageMemory", r, __atomic_add_fetch(&info_vkBindImageMemory.fails, 1, __ATOMIC_RELAXED));
   if (trace_on) fprintf(stderr, "x400-vk: vkBindImageMemory image=%p memory=%p offset=%llu\n", (void *)im, (void *)m, (unsigned long long)off);
   return r;
}
static VKAPI_ATTR int32_t w_arg_vkBindImageMemory2(VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi)
{
   int32_t r = ((int32_t (*)(VkDevice, uint32_t, const VkBindImageMemoryInfo *))info_vkBindImageMemory2.real)(d, n, bi);
   __atomic_add_fetch(&info_vkBindImageMemory2.calls, 1, __ATOMIC_RELAXED);
   if (r) note_fail("vkBindImageMemory2", r, __atomic_add_fetch(&info_vkBindImageMemory2.fails, 1, __ATOMIC_RELAXED));
   if (trace_on) for (uint32_t i = 0; i < n && i < 8; i++)
      fprintf(stderr, "x400-vk: vkBindImageMemory2 image=%p memory=%p offset=%llu pNext=%s\n", (void *)bi[i].image, (void *)bi[i].memory, (unsigned long long)bi[i].memoryOffset, bi[i].pNext ? "yes" : "no");
   return r;
}

static u64 arg_prints_bar2;
static VKAPI_ATTR void w_arg_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di)
{
   __atomic_add_fetch(&info_vkCmdPipelineBarrier2.calls, 1, __ATOMIC_RELAXED);
   order("vkCmdPipelineBarrier2", cb);
   if (trace_on && di && __atomic_add_fetch(&arg_prints_bar2, 1, __ATOMIC_RELAXED) <= 40) {
      fprintf(stderr, "x400-vk: Barrier2 flags=0x%x mem=%u buf=%u img=%u\n", di->dependencyFlags, di->memoryBarrierCount, di->bufferMemoryBarrierCount, di->imageMemoryBarrierCount);
      for (uint32_t i = 0; i < di->memoryBarrierCount && i < 2; i++) {
         const VkMemoryBarrier2 *m = &di->pMemoryBarriers[i];
         fprintf(stderr, "x400-vk:   mem  stage 0x%llx->0x%llx access 0x%llx->0x%llx\n", (unsigned long long)m->srcStageMask, (unsigned long long)m->dstStageMask, (unsigned long long)m->srcAccessMask, (unsigned long long)m->dstAccessMask);
      }
      for (uint32_t i = 0; i < di->imageMemoryBarrierCount && i < 3; i++) {
         const VkImageMemoryBarrier2 *b = &di->pImageMemoryBarriers[i];
         fprintf(stderr, "x400-vk:   img=%p layout %d->%d stage 0x%llx->0x%llx access 0x%llx->0x%llx queue %u->%u aspect=0x%x mips %u+%u layers %u+%u\n", (void *)b->image, b->oldLayout, b->newLayout,
                 (unsigned long long)b->srcStageMask, (unsigned long long)b->dstStageMask, (unsigned long long)b->srcAccessMask, (unsigned long long)b->dstAccessMask,
                 b->srcQueueFamilyIndex, b->dstQueueFamilyIndex, b->subresourceRange.aspectMask, b->subresourceRange.baseMipLevel, b->subresourceRange.levelCount, b->subresourceRange.baseArrayLayer, b->subresourceRange.layerCount);
      }
   }
   ((void (*)(VkCommandBuffer, const VkDependencyInfo *))info_vkCmdPipelineBarrier2.real)(cb, di);
}

/* Samsung's Vulkan driver (Mali-G68) applies vkCmdSetDepthCompareOp even when the depth test is disabled, so NEVER/LESS make every fragment fail
 * (measured with x400/test/vk-dynrender.c DEPTHOP=n: 0 and 1 draw nothing, 2, 3 and 7 draw). The spec says the compare op has no effect with the
 * test disabled, and Zink sets compareOp=NEVER with testEnable=FALSE on every draw. This keeps, per command buffer, the last depth-test-enable
 * and compare op the application set, and tells the driver ALWAYS while the test is off and the real op while it is on. X400_NO_DEPTHFIX=1 turns
 * it off. */
#define CB_N 256
static struct { u64 cb; uint32_t test, op; int have_test, have_op; } cbst[CB_N];
static pthread_mutex_t cb_mtx = PTHREAD_MUTEX_INITIALIZER;
static unsigned cb_n;
static int depthfix_on(void) { static int v = -1; if (v < 0) v = getenv("X400_NO_DEPTHFIX") == NULL; return v; }
static unsigned cb_slot(u64 cb)
{
   for (unsigned i = 0; i < cb_n; i++) if (cbst[i].cb == cb) return i;
   if (cb_n < CB_N) { cbst[cb_n].cb = cb; cbst[cb_n].test = 0; cbst[cb_n].op = 7; cbst[cb_n].have_test = cbst[cb_n].have_op = 0; return cb_n++; }
   return CB_N - 1;
}
static uint32_t eff_op(unsigned i) { return (cbst[i].have_test && !cbst[i].test) ? 7u : cbst[i].op; }
static u64 depthfix_prints;
static VKAPI_ATTR void w_arg_vkCmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 v)
{
   __atomic_add_fetch(&info_vkCmdSetDepthTestEnable.calls, 1, __ATOMIC_RELAXED);
   ((void (*)(VkCommandBuffer, VkBool32))info_vkCmdSetDepthTestEnable.real)(cb, v);
   if (!depthfix_on()) return;
   pthread_mutex_lock(&cb_mtx);
   unsigned i = cb_slot((u64)cb); cbst[i].test = v; cbst[i].have_test = 1;
   int resend = cbst[i].have_op; uint32_t op = eff_op(i);
   pthread_mutex_unlock(&cb_mtx);
   /* if the op was set before the test enable, the driver already got the raw op: send the effective one now */
   if (resend && info_vkCmdSetDepthCompareOp.real) ((void (*)(VkCommandBuffer, uint32_t))info_vkCmdSetDepthCompareOp.real)(cb, op);
}
static VKAPI_ATTR void w_arg_vkCmdSetDepthCompareOp(VkCommandBuffer cb, uint32_t op)
{
   __atomic_add_fetch(&info_vkCmdSetDepthCompareOp.calls, 1, __ATOMIC_RELAXED);
   uint32_t send = op;
   if (depthfix_on()) {
      pthread_mutex_lock(&cb_mtx);
      unsigned i = cb_slot((u64)cb); cbst[i].op = op; cbst[i].have_op = 1;
      send = eff_op(i);
      pthread_mutex_unlock(&cb_mtx);
      if (send != op && trace_on && __atomic_add_fetch(&depthfix_prints, 1, __ATOMIC_RELAXED) <= 8)
         fprintf(stderr, "x400-vk: vkCmdSetDepthCompareOp(%u) with the depth test off -> %u\n", op, send);
   }
   ((void (*)(VkCommandBuffer, uint32_t))info_vkCmdSetDepthCompareOp.real)(cb, send);
}

static const struct { const char *name; struct fn_info *info; PFN_vkVoidFunction w; } trace_tbl[] = {
#define ENTRY(n) { #n, &info_##n, (PFN_vkVoidFunction)w_##n },
#define ENTRY_ARG(n) { #n, &info_##n, (PFN_vkVoidFunction)w_arg_##n },
   RI_LIST(ENTRY) V_LIST(ENTRY)
#undef ENTRY
};
#define TRACE_N (sizeof trace_tbl / sizeof trace_tbl[0])
static PFN_vkVoidFunction wrapper_for(const char *n, PFN_vkVoidFunction dflt)
{
   if (!strcmp(n, "vkCmdDraw")) return (PFN_vkVoidFunction)w_arg_vkCmdDraw;
   if (!strcmp(n, "vkCmdSetViewport")) return (PFN_vkVoidFunction)w_arg_vkCmdSetViewport;
   if (!strcmp(n, "vkCmdSetViewportWithCount")) return (PFN_vkVoidFunction)w_arg_vkCmdSetViewportWithCount;
   if (!strcmp(n, "vkCmdSetScissor")) return (PFN_vkVoidFunction)w_arg_vkCmdSetScissor;
   if (!strcmp(n, "vkCmdSetScissorWithCount")) return (PFN_vkVoidFunction)w_arg_vkCmdSetScissorWithCount;
   if (!strcmp(n, "vkCmdBeginRendering")) return (PFN_vkVoidFunction)w_arg_vkCmdBeginRendering;
   if (!strcmp(n, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)w_arg_vkCreateGraphicsPipelines;
   if (!strcmp(n, "vkCmdSetRasterizerDiscardEnable")) return (PFN_vkVoidFunction)w_arg_vkCmdSetRasterizerDiscardEnable;
   if (!strcmp(n, "vkCmdSetCullMode")) return (PFN_vkVoidFunction)w_arg_vkCmdSetCullMode;
   if (!strcmp(n, "vkCmdSetFrontFace")) return (PFN_vkVoidFunction)w_arg_vkCmdSetFrontFace;
   if (!strcmp(n, "vkCmdSetPrimitiveTopology")) return (PFN_vkVoidFunction)w_arg_vkCmdSetPrimitiveTopology;
   if (!strcmp(n, "vkCmdSetDepthTestEnable")) return (PFN_vkVoidFunction)w_arg_vkCmdSetDepthTestEnable;
   if (!strcmp(n, "vkCmdSetDepthCompareOp")) return (PFN_vkVoidFunction)w_arg_vkCmdSetDepthCompareOp;
   if (!strcmp(n, "vkCmdSetDepthWriteEnable")) return (PFN_vkVoidFunction)w_arg_vkCmdSetDepthWriteEnable;
   if (!strcmp(n, "vkCmdSetDepthBoundsTestEnable")) return (PFN_vkVoidFunction)w_arg_vkCmdSetDepthBoundsTestEnable;
   if (!strcmp(n, "vkCmdSetStencilTestEnable")) return (PFN_vkVoidFunction)w_arg_vkCmdSetStencilTestEnable;
   if (!strcmp(n, "vkCreateImage")) return (PFN_vkVoidFunction)w_arg_vkCreateImage;
   if (!strcmp(n, "vkCreateImageView")) return (PFN_vkVoidFunction)w_arg_vkCreateImageView;
   if (!strcmp(n, "vkCmdPipelineBarrier")) return (PFN_vkVoidFunction)w_arg_vkCmdPipelineBarrier;
   if (!strcmp(n, "vkCmdCopyImageToBuffer")) return (PFN_vkVoidFunction)w_ord_vkCmdCopyImageToBuffer;
   if (!strcmp(n, "vkCmdCopyBufferToImage")) return (PFN_vkVoidFunction)w_ord_vkCmdCopyBufferToImage;
   if (!strcmp(n, "vkCmdCopyBuffer")) return (PFN_vkVoidFunction)w_ord_vkCmdCopyBuffer;
   if (!strcmp(n, "vkCmdCopyImage")) return (PFN_vkVoidFunction)w_ord_vkCmdCopyImage;
   if (!strcmp(n, "vkCmdClearColorImage")) return (PFN_vkVoidFunction)w_ord_vkCmdClearColorImage;
   if (!strcmp(n, "vkCmdBlitImage")) return (PFN_vkVoidFunction)w_ord_vkCmdBlitImage;
   if (!strcmp(n, "vkCmdResolveImage")) return (PFN_vkVoidFunction)w_ord_vkCmdResolveImage;
   if (!strcmp(n, "vkCmdFillBuffer")) return (PFN_vkVoidFunction)w_ord_vkCmdFillBuffer;
   if (!strcmp(n, "vkCmdPipelineBarrier2")) return (PFN_vkVoidFunction)w_arg_vkCmdPipelineBarrier2;
   if (!strcmp(n, "vkCmdBindVertexBuffers2")) return (PFN_vkVoidFunction)w_arg_vkCmdBindVertexBuffers2;
   if (!strcmp(n, "vkCreateBuffer")) return (PFN_vkVoidFunction)w_arg_vkCreateBuffer;
   if (!strcmp(n, "vkBindBufferMemory")) return (PFN_vkVoidFunction)w_arg_vkBindBufferMemory;
   if (!strcmp(n, "vkMapMemory")) return (PFN_vkVoidFunction)w_arg_vkMapMemory;
   if (!strcmp(n, "vkAllocateMemory")) return (PFN_vkVoidFunction)w_arg_vkAllocateMemory;
   if (!strcmp(n, "vkBindImageMemory")) return (PFN_vkVoidFunction)w_arg_vkBindImageMemory;
   if (!strcmp(n, "vkBindImageMemory2")) return (PFN_vkVoidFunction)w_arg_vkBindImageMemory2;
   if (!strcmp(n, "vkCmdSetLineWidth")) return (PFN_vkVoidFunction)w_arg_vkCmdSetLineWidth;
   if (!strcmp(n, "vkCmdSetDepthBias")) return (PFN_vkVoidFunction)w_arg_vkCmdSetDepthBias;
   if (!strcmp(n, "vkCmdSetDepthBounds")) return (PFN_vkVoidFunction)w_arg_vkCmdSetDepthBounds;
   return dflt;
}

static void maybe_summary(const char *n)
{
   static u64 total;
   if (!trace_on || (strcmp(n, "vkQueueSubmit") && strcmp(n, "vkQueueSubmit2")))
      return;
   u64 t = __atomic_add_fetch(&total, 1, __ATOMIC_RELAXED);
   if (t > 16 && (t & (t - 1)))
      return;
   fprintf(stderr, "x400-vk: submit #%llu", (unsigned long long)t);
   for (unsigned i = 0; i < TRACE_N; i++)
      if (trace_tbl[i].info->calls)
         fprintf(stderr, " %s=%llu/%llu", trace_tbl[i].name + 2, (unsigned long long)trace_tbl[i].info->calls, (unsigned long long)trace_tbl[i].info->fails);
   fprintf(stderr, "\n");
}

static PFN_vkVoidFunction trace_lookup(VkDevice d, const char *n)
{
   static const char *const alias[][2] = {
      { "vkQueueSubmit2KHR", "vkQueueSubmit2" }, { "vkCmdBeginRenderingKHR", "vkCmdBeginRendering" },
      { "vkBindImageMemory2KHR", "vkBindImageMemory2" }, { "vkBindBufferMemory2KHR", "vkBindBufferMemory2" },
      { "vkWaitSemaphoresKHR", "vkWaitSemaphores" }, { "vkCreateRenderPass2KHR", "vkCreateRenderPass2" },
      { "vkCmdBeginRenderPass2KHR", "vkCmdBeginRenderPass2" },
      { "vkCmdSetViewportWithCountEXT", "vkCmdSetViewportWithCount" }, { "vkCmdSetScissorWithCountEXT", "vkCmdSetScissorWithCount" },
      { "vkCmdBindVertexBuffers2EXT", "vkCmdBindVertexBuffers2" }, { "vkCmdSetRasterizerDiscardEnableEXT", "vkCmdSetRasterizerDiscardEnable" },
      { "vkCmdSetCullModeEXT", "vkCmdSetCullMode" }, { "vkCmdSetFrontFaceEXT", "vkCmdSetFrontFace" },
      { "vkCmdSetPrimitiveTopologyEXT", "vkCmdSetPrimitiveTopology" }, { "vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthTestEnable" },
      { "vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthWriteEnable" }, { "vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthCompareOp" },
      { "vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetDepthBoundsTestEnable" }, { "vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilTestEnable" },
      { "vkCmdSetStencilOpEXT", "vkCmdSetStencilOp" }, { "vkCmdSetPrimitiveRestartEnableEXT", "vkCmdSetPrimitiveRestartEnable" },
      { "vkCmdSetDepthBiasEnableEXT", "vkCmdSetDepthBiasEnable" }, { "vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2" },
   };
   const char *key = n;
   if (!d || !next_gdpa)
      return NULL;
   for (unsigned i = 0; i < sizeof alias / sizeof alias[0]; i++)
      if (!strcmp(n, alias[i][0]))
         key = alias[i][1];
   for (unsigned i = 0; i < TRACE_N; i++) {
      if (strcmp(key, trace_tbl[i].name))
         continue;
      if (!trace_tbl[i].info->real) {
         PFN_vkVoidFunction r = next_gdpa(d, key);
         if (!r)
            r = next_gdpa(d, n);
         if (!r)
            return NULL;
         trace_tbl[i].info->real = (void *)r;
      }
      return wrapper_for(trace_tbl[i].name, trace_tbl[i].w);
   }
   return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL x_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *al, VkInstance *out)
{
   VkLayerInstanceCreateInfo *lci = (void *)ci->pNext;
   while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
      lci = (void *)lci->pNext;
   if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
   trace_on = getenv("X400_VKTRACE") != NULL;
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

/* what a device is created with: the extensions and every feature struct in the pNext chain, with the booleans that are set. The booleans are
 * VkBool32 fields right after the header of each structure, so they can be walked as an array of uint32 up to the structure's known size. */
static const struct { int stype; const char *name; unsigned nbool; } feat_tbl[] = {
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "Features2", 55 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, "Vulkan11", 12 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, "Vulkan12", 47 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, "Vulkan13", 15 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, "EDS1", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, "EDS2", 3 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, "CustomBorderColor", 2 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, "ProvokingVertex", 2 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT, "LineRasterization", 6 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, "TransformFeedback", 2 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, "IndexUint8", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, "VertexAttrDivisor", 2 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, "ConditionalRendering", 2 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, "ScalarBlock", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, "Robustness2", 3 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, "ImageRobustness", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, "DepthClipEnable", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, "PrimitivesGenQuery", 3 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, "GPL", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, "FeedbackLoop", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, "4444", 2 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT, "MultiDraw", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, "ColorWriteEnable", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, "DynRendering13", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, "Sync2", 1 },
   { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, "Timeline", 1 },
};
static void dump_device_create(const VkDeviceCreateInfo *ci)
{
   fprintf(stderr, "x400-vk: vkCreateDevice extensions(%u):", ci->enabledExtensionCount);
   for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(stderr, " %s", ci->ppEnabledExtensionNames[i]);
   fprintf(stderr, "\n");
   if (ci->pEnabledFeatures) {
      const uint32_t *b = (const uint32_t *)ci->pEnabledFeatures;
      fprintf(stderr, "x400-vk:   pEnabledFeatures set:");
      for (unsigned i = 0; i < 55; i++) if (b[i]) fprintf(stderr, " %u", i);
      fprintf(stderr, "\n");
   }
   for (const VkBaseInStructure *n = ci->pNext; n; n = n->pNext) {
      unsigned k;
      for (k = 0; k < sizeof feat_tbl / sizeof feat_tbl[0]; k++) if (feat_tbl[k].stype == (int)n->sType) break;
      if (k == sizeof feat_tbl / sizeof feat_tbl[0]) { fprintf(stderr, "x400-vk:   pNext sType=%d\n", n->sType); continue; }
      const uint32_t *b = (const uint32_t *)((const char *)n + (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ? 16 : 16));
      fprintf(stderr, "x400-vk:   %s set:", feat_tbl[k].name);
      for (unsigned i = 0; i < feat_tbl[k].nbool; i++) if (b[i] == 1) fprintf(stderr, " %u", i);
      fprintf(stderr, "\n");
   }
}

/* X400_FEAT_OFF=Struct:idx,Struct:idx,...  clears the idx-th VkBool32 of that structure in the vkCreateDevice chain before it reaches the driver
 * (names as in feat_tbl, e.g. Features2:0 = robustBufferAccess, Vulkan13:0 = robustImageAccess). A bisection tool: it changes what Zink asks for
 * without changing Zink. Zink keeps pointers to these structures, so it also sees the cleared value afterwards. */
static void apply_feat_off(const VkDeviceCreateInfo *ci)
{
   const char *env = getenv("X400_FEAT_OFF");
   if (!env || !*env) return;
   char buf[512]; strncpy(buf, env, sizeof buf - 1); buf[sizeof buf - 1] = 0;
   for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
      char *colon = strchr(tok, ':');
      if (!colon) continue;
      *colon = 0; unsigned idx = (unsigned)atoi(colon + 1);
      for (const VkBaseInStructure *n = ci->pNext; n; n = n->pNext) {
         unsigned k;
         for (k = 0; k < sizeof feat_tbl / sizeof feat_tbl[0]; k++) if (feat_tbl[k].stype == (int)n->sType) break;
         if (k == sizeof feat_tbl / sizeof feat_tbl[0] || strcmp(feat_tbl[k].name, tok)) continue;
         uint32_t *b = (uint32_t *)((char *)n + 16);
         if (idx < feat_tbl[k].nbool && b[idx]) { b[idx] = 0; fprintf(stderr, "x400-vk: FEAT_OFF %s:%u cleared\n", tok, idx); }
      }
   }
}

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
   if (trace_on) dump_device_create(ci);
   apply_feat_off(ci);
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
   PFN_vkVoidFunction w = trace_lookup(d, n);
   if (w)
      return w;
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
