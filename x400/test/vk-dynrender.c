/* linux-x400: the triangle of vk-offscreen.c drawn the way Zink 25 draws: vkCmdBeginRendering (dynamic rendering, no render pass or
 * framebuffer) and dynamic viewport/scissor. vk-offscreen (render pass, static state) draws correctly on Venus; Zink's draws write nothing.
 * This says whether the difference is in dynamic rendering / dynamic state or in Zink itself.
 *   env STATIC_STATE=1  viewport and scissor baked into the pipeline instead of dynamic
 *   env NOCLEAR=1       loadOp DONT_CARE instead of CLEAR (the image keeps whatever the clear-to-transfer wrote)
 *   env EDS=1           the extended_dynamic_state set Zink 25 turns on: VIEWPORT_WITH_COUNT/SCISSOR_WITH_COUNT, depth/stencil/cull/front-face/
 *                       topology dynamic, all set with the vkCmdSet*EXT calls before the draw
 *   env EDS2=1          plus extended_dynamic_state2: primitive restart and rasterizer discard dynamic
 *   env VBUF=1          the quad comes from a vertex buffer (4 vertices, TRIANGLE_STRIP, one vec2 attribute) with the vertex input baked in the
 *                       pipeline and vkCmdBindVertexBuffers: what Zink always has and the plain triangle does not
 *   env VBUF2=1         same, bound with vkCmdBindVertexBuffers2 (extended_dynamic_state: offsets, sizes, strides), as Zink binds it
 *   env VINDYN=1        same, with the vertex input dynamic (VK_EXT_vertex_input_dynamic_state, vkCmdSetVertexInputEXT)
 *   env ZINKVS=file ZINKFS=file  use these SPIR-V files (the modules Zink 25 sends, dumped with ZINK_DEBUG=spirv) instead of the built-in shaders; the
 *                       vertex shader of Zink reads one vec2 at location 0 (use VBUF=1), both declare a 52-byte push-constant block (the pipeline
 *                       layout gets that range) and the fragment shader writes eight outputs
 *   env LOADCLEAR=1     the pattern Zink uses on a GL framebuffer: vkCmdClearColorImage to blue (a transfer write), a barrier to colour-attachment
 *                       write, then vkCmdBeginRendering with loadOp=LOAD and the draw. A broken LOAD or barrier leaves the blue.
 *   env LOADCLEAR_NOBARRIER=1   same without the barrier between the clear and the rendering (to see what the barrier changes)
 *   env BIASDYN=1       DEPTH_BIAS_ENABLE dynamic and depthBiasEnable=1 in the pipeline, NEVER set before the draw (what Zink 25 sends: it sets
 *                       vkCmdSetDepthBias but not vkCmdSetDepthBiasEnable). BIASDYN=set sets it to FALSE first, as the spec requires.
 *   env IMGMEM=n        take the colour image's memory from memory type n (default: the first DEVICE_LOCAL one); the types are listed at start
 *   env MUTABLE=1       create the colour image as Zink does: flags MUTABLE_FORMAT|EXTENDED_USAGE with a VkImageFormatListCreateInfo {UNORM, SRGB}, usage
 *                       0x9f (adds SAMPLED and INPUT_ATTACHMENT), and the view with an explicit R,G,B,A swizzle and a VkImageViewUsageCreateInfo
 *   env MULTICB=1       the preparation barrier goes in one command buffer and the render pass + draw in a second one, both in ONE VkSubmitInfo
 *                       (Zink's reordered_cmdbuf + cmdbuf)
 *   env TIMELINE=1      plus a second VkSubmitInfo in the same vkQueueSubmit that signals a timeline semaphore (Zink's screen->sem), which the
 *                       host then waits on with vkWaitSemaphores instead of a fence. Implies MULTICB. TIMELINE=nofence submits without a fence
 *   env ENGINE=zink     VkApplicationInfo.pEngineName = "mesa zink": Mesa's Venus driver (vn_instance.c) turns on zink-specific code when it sees it
 *                       (vn_has_zink_sync_batch: the empty last batch that only signals the timeline semaphore)
 *   env PATCHCP=1       call vkCmdSetPatchControlPointsEXT(1) before the draw although the pipeline does not list PATCH_CONTROL_POINTS as dynamic: the
 *                       venus server trace shows Zink does this before every draw (52 of the first 400 commands). Needs extended_dynamic_state2.
 *   env PATCHCP=dyn     the same with PATCH_CONTROL_POINTS declared dynamic in the pipeline (the legal way)
 *   env SPLITSUBMIT=1   the copy back to the host buffer is recorded in a THIRD command buffer and sent in its own vkQueueSubmit after the one that
 *                       draws, ordered only by queue order and the image barrier (Zink: draw in one submit, glReadPixels copy in the next).
 *                       SPLITSUBMIT=sem orders them with a timeline semaphore instead (signal 1 after the draw, wait 1 before the copy).
 *   env VPZERO=1        the pipeline's VkPipelineViewportStateCreateInfo has viewportCount=0 and scissorCount=0 (as Zink's: they come from the dynamic
 *                       VIEWPORT_WITH_COUNT / SCISSOR_WITH_COUNT, which then must be set before the draw). Implies EDS.
 *   env IMGOFF=n        allocate one 4 MiB block and bind the colour image at byte offset n inside it (Zink sub-allocates its images from shared blocks:
 *                       the layer trace shows them at 0, 393216, 786432, 1179648). n must be a multiple of the image's alignment
 *   env DEPTHFMT=1      the pipeline and the render pass also carry a depth attachment (KWin and Zink always have one)
 * Build in the guest: gcc -O1 -o vk-dynrender vk-dynrender.c -lvulkan. Exit 0 = the pixels are right. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tri_vert.h"
#include "tri_frag.h"
#include "tri_vbuf_vert.h"
#define W 512
#define H 512
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static uint32_t *load_spv(const char *path, size_t *size)
{
  FILE *f = fopen(path, "rb"); if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint32_t *buf = malloc((n + 3) & ~3L); if (fread(buf, 1, n, f) != (size_t)n) { perror("read"); exit(2); } fclose(f);
  *size = n; return buf;
}

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  fprintf(stderr, "no memory type\n"); exit(2);
}

int main(void)
{
  VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3, .pEngineName = getenv("ENGINE") ? "mesa zink" : NULL };
  VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
  VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 1; VkPhysicalDevice pd; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
  if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) { fprintf(stderr, "no device\n"); return 2; }
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
  printf("device: %s  api %u.%u.%u\n", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion));

  {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      printf("memtype %u: heap %u flags 0x%x%s%s%s%s\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags,
             mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
             mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
             mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " COHERENT" : "",
             mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " CACHED" : "");
  }
  VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
  VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
  vkGetPhysicalDeviceFeatures2(pd, &f2);
  printf("dynamicRendering=%u synchronization2=%u\n", f13.dynamicRendering, f13.synchronization2);
  if (!f13.dynamicRendering) { fprintf(stderr, "no dynamic rendering\n"); return 3; }

  VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .extendedDynamicState = VK_TRUE };
  VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .extendedDynamicState2 = VK_TRUE, .extendedDynamicState2PatchControlPoints = getenv("PATCHCP") != NULL };
  int vindyn = getenv("VINDYN") != NULL, vbuf2 = getenv("VBUF2") != NULL, vbuf = getenv("VBUF") != NULL || vbuf2 || vindyn;
  int use_eds = getenv("EDS") != NULL || getenv("EDS2") != NULL || vbuf2 || getenv("VPZERO") != NULL, use_eds2 = getenv("EDS2") != NULL || getenv("BIASDYN") != NULL || getenv("PATCHCP") != NULL;
  VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .vertexInputDynamicState = VK_TRUE };
  VkPhysicalDeviceVulkan13Features want13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
  void *chain = &want13;
  if (use_eds) { eds.pNext = chain; chain = &eds; }
  if (use_eds2) { eds2.pNext = chain; chain = &eds2; }
  if (vindyn) { vids.pNext = chain; chain = &vids; }
  const char *exts[3]; uint32_t next_ = 0;
  if (use_eds) exts[next_++] = "VK_EXT_extended_dynamic_state";
  if (use_eds2 || getenv("BIASDYN")) exts[next_++] = "VK_EXT_extended_dynamic_state2";
  if (vindyn) exts[next_++] = "VK_EXT_vertex_input_dynamic_state";
  float prio = 1; VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
  VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = chain, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = next_, .ppEnabledExtensionNames = exts };
  VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
  PFN_vkCmdBeginRendering cmdBegin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering");
  PFN_vkCmdEndRendering cmdEnd = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering");
  if (!cmdBegin || !cmdEnd) { fprintf(stderr, "vkCmdBeginRendering not resolved\n"); return 3; }

  VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
    .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
  VkFormat fl[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB };
  VkImageFormatListCreateInfo fli = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = fl };
  int mutable_ = getenv("MUTABLE") != NULL;
  if (mutable_) {
    ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    ii.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    ii.pNext = &fli;
  }
  VkImage img; CK(vkCreateImage(dev, &ii, NULL, &img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
  VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = getenv("IMGMEM") ? (uint32_t)atoi(getenv("IMGMEM")) : mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
  printf("image: memoryTypeBits=0x%x size=%llu -> type %u\n", mr.memoryTypeBits, (unsigned long long)mr.size, ma.memoryTypeIndex);
  VkDeviceSize imgoff = getenv("IMGOFF") ? (VkDeviceSize)strtoull(getenv("IMGOFF"), NULL, 0) : 0;
  if (imgoff) { if (imgoff % mr.alignment) { printf("IMGOFF %llu is not a multiple of the image alignment %llu\n", (unsigned long long)imgoff, (unsigned long long)mr.alignment); return 3; } ma.allocationSize = 4 * 1048576; if (imgoff + mr.size > ma.allocationSize) { printf("IMGOFF %llu + image %llu does not fit in the block\n", (unsigned long long)imgoff, (unsigned long long)mr.size); return 3; } }
  VkDeviceMemory imem; CK(vkAllocateMemory(dev, &ma, NULL, &imem)); CK(vkBindImageMemory(dev, img, imem, imgoff));
  printf("image: alignment=%llu bound at offset %llu of %llu bytes\n", (unsigned long long)mr.alignment, (unsigned long long)imgoff, (unsigned long long)ma.allocationSize);
  VkImageViewUsageCreateInfo vuc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
  VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ii.format,
    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
  if (mutable_) {
    vi.pNext = &vuc;
    vi.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
  }
  VkImageView view; CK(vkCreateImageView(dev, &vi, NULL, &view));

  VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
  VkBuffer buf; CK(vkCreateBuffer(dev, &bi, NULL, &buf)); vkGetBufferMemoryRequirements(dev, buf, &mr);
  ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bmem; CK(vkAllocateMemory(dev, &ma, NULL, &bmem)); CK(vkBindBufferMemory(dev, buf, bmem, 0));
  uint8_t *px; CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
  memset(px, 0xAB, W * H * 4);
  VkBuffer vb = VK_NULL_HANDLE;
  if (vbuf) {
    static const float quad[8] = { -0.8f, -0.8f, 0.8f, -0.8f, -0.8f, 0.8f, 0.8f, 0.8f };
    VkBufferCreateInfo vbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof quad, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
    CK(vkCreateBuffer(dev, &vbi, NULL, &vb)); VkMemoryRequirements vmr; vkGetBufferMemoryRequirements(dev, vb, &vmr);
    VkMemoryAllocateInfo vma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = vmr.size,
      .memoryTypeIndex = mem_type(pd, vmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory vmem; CK(vkAllocateMemory(dev, &vma, NULL, &vmem)); CK(vkBindBufferMemory(dev, vb, vmem, 0));
    void *vp_; CK(vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, &vp_)); memcpy(vp_, quad, sizeof quad);
  }

  VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  VkShaderModule vs, fs;
  if (vbuf) { smi.codeSize = sizeof tri_vbuf_vert; smi.pCode = tri_vbuf_vert; } else { smi.codeSize = sizeof tri_vert; smi.pCode = tri_vert; }
  if (getenv("ZINKVS")) { smi.pCode = load_spv(getenv("ZINKVS"), &smi.codeSize); }
  CK(vkCreateShaderModule(dev, &smi, NULL, &vs));
  smi.codeSize = sizeof tri_frag; smi.pCode = tri_frag;
  if (getenv("ZINKFS")) { smi.pCode = load_spv(getenv("ZINKFS"), &smi.codeSize); }
  CK(vkCreateShaderModule(dev, &smi, NULL, &fs));
  VkPipelineShaderStageCreateInfo st[2] = {
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
  VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 52 };
  int zinkpc = getenv("ZINKVS") != NULL || getenv("ZINKFS") != NULL;
  VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = zinkpc ? 1 : 0, .pPushConstantRanges = &pcr };
  VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
  VkVertexInputBindingDescription vbd = { .binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription vad = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };
  VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
  if (vbuf && !vindyn) { vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd; vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &vad; }
  VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = vbuf ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
  int stat = getenv("STATIC_STATE") != NULL;
  VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
  VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = getenv("VPZERO") ? 0 : 1, .scissorCount = getenv("VPZERO") ? 0 : 1,
    .pViewports = stat ? &vp : NULL, .pScissors = stat ? &sc : NULL };
  VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1, .depthBiasEnable = getenv("BIASDYN") != NULL };
  VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
  VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
  VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
  VkDynamicState dyn[40]; uint32_t nd = 0;
  if (!stat) {
    if (use_eds) {
      dyn[nd++] = VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT; dyn[nd++] = VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT;
      dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE; dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE; dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
      dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE; dyn[nd++] = VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE; dyn[nd++] = VK_DYNAMIC_STATE_STENCIL_OP;
      dyn[nd++] = VK_DYNAMIC_STATE_FRONT_FACE; dyn[nd++] = VK_DYNAMIC_STATE_CULL_MODE; dyn[nd++] = VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
    } else {
      dyn[nd++] = VK_DYNAMIC_STATE_VIEWPORT; dyn[nd++] = VK_DYNAMIC_STATE_SCISSOR;
    }
    if (vindyn) dyn[nd++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    if (getenv("BIASDYN")) dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE;
    if (getenv("PATCHCP") && !strcmp(getenv("PATCHCP"), "dyn")) dyn[nd++] = VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT;
    if (use_eds2) { dyn[nd++] = VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE; dyn[nd++] = VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE; }
  }
  VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = nd, .pDynamicStates = dyn };
  VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthCompareOp = VK_COMPARE_OP_ALWAYS };
  int use_depth = getenv("DEPTHFMT") != NULL;
  const char *biasdyn = getenv("BIASDYN");
  VkFormat cfmt = ii.format;
  VkPipelineRenderingCreateInfo pri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt, .depthAttachmentFormat = use_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED };
  VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pri, .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
    .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDepthStencilState = (use_depth || use_eds) ? &dss : NULL, .pDynamicState = &ds, .layout = pl };
  VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

  VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
  VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
  VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbi, &cmd));
  VkCommandBuffer cmd2 = VK_NULL_HANDLE, cmd3 = VK_NULL_HANDLE;
  int split = getenv("SPLITSUBMIT") != NULL;
  if (split) CK(vkAllocateCommandBuffers(dev, &cbi, &cmd3));
  int timeline = getenv("TIMELINE") != NULL || (split && !strcmp(getenv("SPLITSUBMIT"), "sem")), multicb = getenv("MULTICB") != NULL || timeline;
  if (multicb) CK(vkAllocateCommandBuffers(dev, &cbi, &cmd2));
  VkSemaphore tsem = VK_NULL_HANDLE;
  if (timeline) {
    VkSemaphoreTypeCreateInfo sti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti };
    CK(vkCreateSemaphore(dev, &sci, NULL, &tsem));
  }
  VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  VkFence fence; CK(vkCreateFence(dev, &fci, NULL, &fence));

  VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  CK(vkBeginCommandBuffer(cmd, &bgi));
  VkImageMemoryBarrier toColor = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
  int loadclear = getenv("LOADCLEAR") != NULL || getenv("LOADCLEAR_NOBARRIER") != NULL;
  if (loadclear) {
    VkImageMemoryBarrier toDst = toColor; toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
    VkClearColorValue blue = { .float32 = { 0.1f, 0.2f, 0.9f, 1.0f } }; VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &rg);
    VkImageMemoryBarrier toAtt = toColor; toAtt.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toAtt.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAtt.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toAtt.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (getenv("LOADCLEAR_NOBARRIER")) { toAtt.srcAccessMask = 0; toAtt.dstAccessMask = 0; }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &toAtt);
  } else {
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &toColor);
  }

  if (multicb) {
    CK(vkEndCommandBuffer(cmd));
    CK(vkBeginCommandBuffer(cmd2, &bgi));
  }
  VkCommandBuffer rec = multicb ? cmd2 : cmd;
  VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .loadOp = loadclear ? VK_ATTACHMENT_LOAD_OP_LOAD : (getenv("NOCLEAR") ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR), .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .clearValue = { .color = { { 0.1f, 0.2f, 0.9f, 1.0f } } } };
  VkRenderingInfo rinfo = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
  cmdBegin(rec, &rinfo);
  vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  if (!stat) {
    if (use_eds) {
      PFN_vkCmdSetViewportWithCount svp = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetViewportWithCountEXT");
      PFN_vkCmdSetScissorWithCount ssc = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetScissorWithCountEXT");
      PFN_vkCmdSetDepthTestEnable sdt = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetDepthTestEnableEXT");
      PFN_vkCmdSetDepthWriteEnable sdw = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetDepthWriteEnableEXT");
      PFN_vkCmdSetDepthCompareOp sdc = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetDepthCompareOpEXT");
      PFN_vkCmdSetDepthBoundsTestEnable sdb = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBoundsTestEnableEXT");
      PFN_vkCmdSetStencilTestEnable sst = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetStencilTestEnableEXT");
      PFN_vkCmdSetStencilOp sso = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetStencilOpEXT");
      PFN_vkCmdSetFrontFace sff = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetFrontFaceEXT");
      PFN_vkCmdSetCullMode scm = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetCullModeEXT");
      PFN_vkCmdSetPrimitiveTopology spt = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetPrimitiveTopologyEXT");
      svp(rec, 1, &vp); ssc(rec, 1, &sc);
      sdt(rec, VK_FALSE); sdw(rec, VK_FALSE); sdc(rec, VK_COMPARE_OP_ALWAYS); sdb(rec, VK_FALSE); sst(rec, VK_FALSE);
      sso(rec, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
      sff(rec, VK_FRONT_FACE_COUNTER_CLOCKWISE); scm(rec, VK_CULL_MODE_NONE); spt(rec, vbuf ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    } else {
      vkCmdSetViewport(rec, 0, 1, &vp); vkCmdSetScissor(rec, 0, 1, &sc);
    }
    if (biasdyn && !strcmp(biasdyn, "set")) {
      PFN_vkCmdSetDepthBiasEnable sbe = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBiasEnableEXT");
      sbe(rec, VK_FALSE);
    }
    if (biasdyn) vkCmdSetDepthBias(rec, 0.0f, 0.0f, 0.0f);
    if (use_eds2) {
      PFN_vkCmdSetPrimitiveRestartEnable spr = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetPrimitiveRestartEnableEXT");
      PFN_vkCmdSetRasterizerDiscardEnable srd = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetRasterizerDiscardEnableEXT");
      spr(rec, VK_FALSE); srd(rec, VK_FALSE);
      if (getenv("PATCHCP")) {
        PFN_vkCmdSetPatchControlPointsEXT spc = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetPatchControlPointsEXT");
        if (!spc) { fprintf(stderr, "vkCmdSetPatchControlPointsEXT not resolved\n"); exit(3); }
        spc(rec, 1);
      }
    }
  }
  if (vbuf) {
    VkDeviceSize off = 0, size = 32, stride = 8;
    if (vindyn) {
      PFN_vkCmdSetVertexInputEXT svi = (void *)vkGetDeviceProcAddr(dev, "vkCmdSetVertexInputEXT");
      VkVertexInputBindingDescription2EXT b2 = { .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = 0, .stride = 8, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1 };
      VkVertexInputAttributeDescription2EXT a2 = { .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT };
      svi(rec, 1, &b2, 1, &a2);
    }
    if (vbuf2) {
      PFN_vkCmdBindVertexBuffers2 bvb2 = (void *)vkGetDeviceProcAddr(dev, "vkCmdBindVertexBuffers2EXT");
      bvb2(rec, 0, 1, &vb, &off, &size, &stride);
    } else {
      vkCmdBindVertexBuffers(rec, 0, 1, &vb, &off);
    }
  }
  vkCmdDraw(rec, vbuf ? 4 : 3, 1, 0, 0);
  cmdEnd(rec);

  VkImageMemoryBarrier toSrc = toColor; toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkBufferImageCopy bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
  if (split) {
    /* the draw's command buffer ends with the layout transition; the copy goes in its own command buffer */
    vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSrc);
    CK(vkBeginCommandBuffer(cmd3, &bgi));
    vkCmdCopyImageToBuffer(cmd3, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
    CK(vkEndCommandBuffer(cmd3));
  } else {
    vkCmdPipelineBarrier(rec, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSrc);
    vkCmdCopyImageToBuffer(rec, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
  }
  CK(vkEndCommandBuffer(rec));
  if (multicb) {
    VkCommandBuffer cbs[2] = { cmd, cmd2 };
    VkSubmitInfo sub[2] = {
      { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 2, .pCommandBuffers = cbs },
      { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO } };
    uint64_t one = 1;
    VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &one };
    uint32_t nsub = 1;
    if (timeline) { sub[1].pNext = &tsi; sub[1].signalSemaphoreCount = 1; sub[1].pSignalSemaphores = &tsem; nsub = 2; }
    int nofence = timeline && getenv("TIMELINE") && !strcmp(getenv("TIMELINE"), "nofence");
    CK(vkQueueSubmit(q, nsub, sub, nofence ? VK_NULL_HANDLE : fence));
    if (timeline) {
      VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &tsem, .pValues = &one };
      CK(vkWaitSemaphores(dev, &wi, 20000000000ull));
    } else {
      CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 20000000000ull));
    }
  } else {
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CK(vkQueueSubmit(q, 1, &si, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 20000000000ull));
  }
  if (split) {
    VkFence f2; CK(vkCreateFence(dev, &fci, NULL, &f2));
    uint64_t one = 1;
    VkTimelineSemaphoreSubmitInfo wt = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &one };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo cs = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd3 };
    if (timeline && !strcmp(getenv("SPLITSUBMIT"), "sem")) { cs.pNext = &wt; cs.waitSemaphoreCount = 1; cs.pWaitSemaphores = &tsem; cs.pWaitDstStageMask = &ws; }
    CK(vkQueueSubmit(q, 1, &cs, f2));
    CK(vkWaitForFences(dev, 1, &f2, VK_TRUE, 20000000000ull));
  }

  uint8_t *c = px + (H / 2 * W + W / 2) * 4, *k = px + (4 * W + 4) * 4;
  printf("%s%s%s state%s%s%s%s%s%s%s, %s\n", multicb ? (timeline ? "MULTICB+TIMELINE " : "MULTICB ") : "", getenv("ZINKVS") ? "ZINK-VS " : "", getenv("ZINKFS") ? "ZINK-FS" : "", stat ? "static" : "dynamic", use_eds ? " +EDS" : "", use_eds2 ? " +EDS2" : "", use_depth ? " +depth fmt" : "", vbuf ? " +vertex buffer" : "", vbuf2 ? " (BindVertexBuffers2)" : "", vindyn ? " +dynamic vertex input" : "", getenv("NOCLEAR") ? "no clear" : "clear");
  printf("center  RGBA %3u %3u %3u %3u (expect 255 128 0 255 = triangle)\n", c[0], c[1], c[2], c[3]);
  printf("corner  RGBA %3u %3u %3u %3u (expect  26  51 230 255 = clear colour)\n", k[0], k[1], k[2], k[3]);
  int ok = c[0] > 250 && c[1] > 120 && c[1] < 136 && c[2] < 4 && k[0] > 22 && k[0] < 30 && k[2] > 226;
  printf(ok ? "PIXELS OK\n" : "PIXELS WRONG\n");
  return ok ? 0 : 1;
}
