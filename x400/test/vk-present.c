// linux-x400: stage C, piece 4. Can the display helper put an imported dma-buf on its layer with the GPU, and what does a frame cost?
// This is a stand-in for libx400display.so: it exports Java_X400Display_nRun, so the real x400-display.dex builds the layer (SurfaceControl + BLASTBufferQueue)
// and hands this code the Surface. Then, on that Surface:
//   phase "cpu":   ANativeWindow_lock + memcpy of a 2112x1320 BGRA frame from ordinary memory + unlockAndPost  (what the helper does today, best case)
//   phase "vk-*":  the same frame as a dma-buf from /dev/dma_heap imported as a VkImage (tiling DRM_FORMAT_MODIFIER, LINEAR), vkCmdCopyImage into the swapchain
//                  image, present, once per present mode the driver offers. The CPU only touches a moving bar of the dma-buf each frame, like a producer would.
// Each phase reports frames per second, the CPU time of the whole process per frame, and where a Vulkan frame waits.
// Build with the NDK (see the commit message), run with x400/display/display.sh against a directory holding x400-display.dex and this library
// renamed libx400display.so, in the session's mount namespace, as root. Env: X400_VKP_SECS (3), X400_VKP_HEAP (system-uncached), X400_VKP_HOLD (0 s of FIFO at the end,
// for a screenshot), X400_VKP_CPUWRITE (1: the moving bar), X400_VKP_ONLY (cpu | vk).
#define _GNU_SOURCE
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#define LOG(...) do { fprintf(stderr, "vkp: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("FAILED %s = %d (line %d)", #x, r_, __LINE__); return -1; } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static double cpu_now(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int env_int(const char *n, int d) { const char *v = getenv(n); return v ? atoi(v) : d; }

static VkInstance inst; static VkPhysicalDevice pd; static VkDevice dev; static VkQueue q; static VkSurfaceKHR surf;
static VkImage src; static uint8_t *srcmap; static size_t srcsize; static int srcfd = -1; static uint32_t pitch, W, H;
static VkCommandPool pool;

static void sync_cpu(int fd, int start) { struct dma_buf_sync s = { .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_READ }; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); }

static void write_bar(unsigned long n)
{
  uint32_t bar = (uint32_t)((n * 23) % (W - 60));
  sync_cpu(srcfd, 1);
  for (uint32_t y = 0; y < H; y++) memset(srcmap + (size_t)y * pitch + (size_t)bar * 4, 0xff, 60 * 4);
  if (bar >= 24) for (uint32_t y = 0; y < H; y++) { uint8_t *p = srcmap + (size_t)y * pitch + (size_t)(bar - 24) * 4; for (int i = 0; i < 24; i++) { p[i * 4] = (uint8_t)(y * 255 / H); p[i * 4 + 1] = 40; p[i * 4 + 2] = (uint8_t)((bar - 24 + i) * 255 / W); p[i * 4 + 3] = 255; } }
  sync_cpu(srcfd, 0);
}

static int phase_cpu(ANativeWindow *win, double secs)
{
  ANativeWindow_setBuffersGeometry(win, W, H, 5 /* BGRA_8888 */);
  uint8_t *frame = malloc((size_t)W * H * 4); for (size_t i = 0; i < (size_t)W * H * 4; i++) frame[i] = (uint8_t)i;
  unsigned long n = 0, fail = 0; double t0 = now(), c0 = cpu_now(), lock = 0, copy = 0, post = 0;
  while (now() - t0 < secs) {
    ANativeWindow_Buffer b; double a = now();
    if (ANativeWindow_lock(win, &b, NULL) < 0) { fail++; usleep(2000); continue; }
    double b1 = now();
    uint32_t rows = H < (uint32_t)b.height ? H : (uint32_t)b.height;
    for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, frame + (size_t)y * W * 4, W * 4);
    double c1 = now();
    ANativeWindow_unlockAndPost(win);
    double d1 = now(); lock += b1 - a; copy += c1 - b1; post += d1 - c1; n++;
    frame[n % 4096] ^= 1;
  }
  double dt = now() - t0, cpu = cpu_now() - c0;
  LOG("RESULT cpu: %lu frames in %.1f s = %.1f fps; process CPU %.1f ms/frame (%.0f%% of a core); lock %.2f ms, memcpy %.2f ms, unlockAndPost %.2f ms per frame; failed locks %lu", n, dt, n / dt,
      n ? cpu * 1000 / n : 0, cpu / dt * 100, n ? lock * 1000 / n : 0, n ? copy * 1000 / n : 0, n ? post * 1000 / n : 0, fail);
  free(frame);
  return 0;
}

static int import_source(void)
{
  const char *heap = getenv("X400_VKP_HEAP") ? getenv("X400_VKP_HEAP") : "system-uncached";
  pitch = (W * 4 + 63) & ~63u;
  VkSubresourceLayout pl = { .offset = 0, .size = 0, .rowPitch = pitch };
  VkImageDrmFormatModifierExplicitCreateInfoEXT dmi = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, 0 /* LINEAR */, 1, &pl };
  VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &dmi, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
  VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
  CK(vkCreateImage(dev, &ic, NULL, &src));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, src, &mr);
  char path[128]; snprintf(path, sizeof path, "/dev/dma_heap/%s", heap);
  int hfd = open(path, O_RDONLY | O_CLOEXEC);
  if (hfd < 0) { LOG("open %s: %s", path, strerror(errno)); return -1; }
  srcsize = ((mr.size > (size_t)pitch * H ? mr.size : (size_t)pitch * H) + 4095) & ~4095ull;
  struct dma_heap_allocation_data a = { .len = srcsize, .fd_flags = O_RDWR | O_CLOEXEC };
  if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a)) { LOG("dma_heap alloc in %s: %s", heap, strerror(errno)); return -1; }
  srcfd = a.fd; close(hfd);
  PFN_vkGetMemoryFdPropertiesKHR getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
  VkMemoryFdPropertiesKHR fdp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
  CK(getFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, srcfd, &fdp));
  uint32_t bits = mr.memoryTypeBits & fdp.memoryTypeBits, mt = 0; while (mt < 32 && !(bits & (1u << mt))) mt++;
  if (mt == 32) { LOG("no memory type fits the dma-buf"); return -1; }
  VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, src, VK_NULL_HANDLE };
  VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup(srcfd) };
  VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, mt };
  VkDeviceMemory mem; CK(vkAllocateMemory(dev, &mai, NULL, &mem)); CK(vkBindImageMemory(dev, src, mem, 0));
  srcmap = mmap(NULL, srcsize, PROT_READ | PROT_WRITE, MAP_SHARED, srcfd, 0);
  if (srcmap == MAP_FAILED) { LOG("mmap: %s", strerror(errno)); return -1; }
  sync_cpu(srcfd, 1);
  for (uint32_t y = 0; y < H; y++) { uint8_t *r = srcmap + (size_t)y * pitch; for (uint32_t x = 0; x < W; x++) { r[x * 4] = (uint8_t)(y * 255 / H); r[x * 4 + 1] = 40; r[x * 4 + 2] = (uint8_t)(x * 255 / W); r[x * 4 + 3] = 255; } }
  sync_cpu(srcfd, 0);
  LOG("source: %ux%u BGRA, dma-buf in %s (%zu bytes), pitch %u, memory type %u", W, H, heap, srcsize, pitch, mt);
  VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbi, &cb));
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
  CK(vkBeginCommandBuffer(cb, &bi));
  VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = src, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
  CK(vkEndCommandBuffer(cb));
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
  CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
  return 0;
}

static const char *mode_name(VkPresentModeKHR m) { return m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : m == VK_PRESENT_MODE_FIFO_KHR ? "fifo" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo-relaxed" : "other"; }

static int phase_vk(VkPresentModeKHR mode, double secs, VkSurfaceCapabilitiesKHR caps, VkFormat fmt, VkColorSpaceKHR cs, int cpuwrite)
{
  VkExtent2D ext = caps.currentExtent.width == 0xffffffffu ? (VkExtent2D){ W, H } : caps.currentExtent;
  uint32_t minimg = caps.minImageCount < 3 ? 3 : caps.minImageCount; if (caps.maxImageCount && minimg > caps.maxImageCount) minimg = caps.maxImageCount;
  VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = minimg, .imageFormat = fmt, .imageColorSpace = cs, .imageExtent = ext, .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE };
  VkSwapchainKHR sc; VkResult r = vkCreateSwapchainKHR(dev, &sci, NULL, &sc);
  if (r) { LOG("vk-%s: vkCreateSwapchainKHR = %d", mode_name(mode), r); return 0; }
  uint32_t nimg = 0; vkGetSwapchainImagesKHR(dev, sc, &nimg, NULL); VkImage imgs[16]; if (nimg > 16) nimg = 16; vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
  struct { VkCommandBuffer cb; VkSemaphore acq, done; VkFence fence; } f[2];
  VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
  for (int i = 0; i < 2; i++) { CK(vkAllocateCommandBuffers(dev, &cbi, &f[i].cb)); CK(vkCreateSemaphore(dev, &sem, NULL, &f[i].acq)); CK(vkCreateSemaphore(dev, &sem, NULL, &f[i].done)); CK(vkCreateFence(dev, &fci, NULL, &f[i].fence)); }
  VkExtent3D ce = { ext.width < W ? ext.width : W, ext.height < H ? ext.height : H, 1 };
  unsigned long n = 0, bad = 0; double t0 = now(), c0 = cpu_now(), tw = 0, ta = 0, ts = 0, tp = 0, tc = 0, worst = 0, last = t0; uint32_t cur = 0;
  while (now() - t0 < secs) {
    double s0 = now();
    vkWaitForFences(dev, 1, &f[cur].fence, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &f[cur].fence);
    double s1 = now(); tw += s1 - s0;
    if (cpuwrite) write_bar(n);
    double s2 = now(); tc += s2 - s1;
    uint32_t idx; r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, f[cur].acq, VK_NULL_HANDLE, &idx);
    double s3 = now(); ta += s3 - s2;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { bad++; if (bad > 3) { LOG("vk-%s: acquire = %d", mode_name(mode), r); break; } continue; }
    vkResetCommandBuffer(f[cur].cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(f[cur].cb, &bi);
    VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = imgs[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(f[cur].cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    if (getenv("X400_VKP_RAWCOPY")) {
      VkImageCopy cp = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = ce };
      vkCmdCopyImage(f[cur].cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    } else {  // the swapchain is RGBA, the source BGRA: a raw copy would swap red and blue, a blit converts
      VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { (int32_t)ce.width, (int32_t)ce.height, 1 } },
                         .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { (int32_t)ce.width, (int32_t)ce.height, 1 } } };
      vkCmdBlitImage(f[cur].cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    }
    VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0; b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(f[cur].cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
    vkEndCommandBuffer(f[cur].cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &f[cur].acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &f[cur].cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &f[cur].done };
    r = vkQueueSubmit(q, 1, &si, f[cur].fence);
    double s4 = now(); ts += s4 - s3;
    if (r) { LOG("vk-%s: submit = %d", mode_name(mode), r); break; }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &f[cur].done, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx };
    r = vkQueuePresentKHR(q, &pi);
    double s5 = now(); tp += s5 - s4;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("vk-%s: present = %d", mode_name(mode), r); break; }
    if (s5 - last > worst) worst = s5 - last; last = s5;
    cur ^= 1; n++;
  }
  double dt = now() - t0, cpu = cpu_now() - c0;
  LOG("RESULT vk-%s: %lu frames in %.1f s = %.1f fps; process CPU %.1f ms/frame (%.0f%% of a core); per frame: wait-fence %.2f ms, cpu-write %.2f ms, acquire %.2f ms, record+submit %.2f ms, present %.2f ms; worst frame %.0f ms; swapchain %u images %ux%u",
      mode_name(mode), n, dt, n / dt, n ? cpu * 1000 / n : 0, cpu / dt * 100, n ? tw * 1000 / n : 0, n ? tc * 1000 / n : 0, n ? ta * 1000 / n : 0, n ? ts * 1000 / n : 0, n ? tp * 1000 / n : 0, worst * 1000, nimg, ext.width, ext.height);
  vkDeviceWaitIdle(dev);
  for (int i = 0; i < 2; i++) { vkDestroyFence(dev, f[i].fence, NULL); vkDestroySemaphore(dev, f[i].acq, NULL); vkDestroySemaphore(dev, f[i].done, NULL); }
  vkDestroySwapchainKHR(dev, sc, NULL);
  return 0;
}

static int run(ANativeWindow *win, double secs, double hold)
{
  const char *only = getenv("X400_VKP_ONLY"); if (only && !*only) only = NULL;
  if (!only || !strcmp(only, "cpu")) { phase_cpu(win, secs); }
  if (only && !strcmp(only, "cpu")) return 0;

  uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
  VkExtensionProperties *ep = calloc(ne + 1, sizeof *ep); vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
  int have_as = 0, have_s = 0; for (uint32_t i = 0; i < ne; i++) { if (!strcmp(ep[i].extensionName, "VK_KHR_android_surface")) have_as = 1; if (!strcmp(ep[i].extensionName, "VK_KHR_surface")) have_s = 1; }
  LOG("instance extensions: %u, VK_KHR_surface %s, VK_KHR_android_surface %s", ne, have_s ? "yes" : "NO", have_as ? "yes" : "NO");
  free(ep);
  if (!have_as || !have_s) return -1;

  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
  const char *iext[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
  CK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
  VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); LOG("device %s", pp.deviceName);
  VkAndroidSurfaceCreateInfoKHR asi = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = win };
  CK(vkCreateAndroidSurfaceKHR(inst, &asi, NULL, &surf));
  VkBool32 sup = 0; CK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup)); LOG("queue family 0 can present: %u", sup);
  float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
  const char *dext[] = { "VK_KHR_swapchain", VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                         VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME };
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 6, .ppEnabledExtensionNames = dext };
  CK(vkCreateDevice(pd, &dci, NULL, &dev)); vkGetDeviceQueue(dev, 0, 0, &q);
  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
  CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
  if (import_source()) return -1;

  VkSurfaceCapabilitiesKHR caps; CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
  LOG("surface: current %ux%u, images %u..%u, transform 0x%x (supported 0x%x), usage 0x%x (TRANSFER_DST %s)", caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount, caps.maxImageCount,
      caps.currentTransform, caps.supportedTransforms, caps.supportedUsageFlags, (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? "yes" : "NO");
  uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf);
  VkFormat fmt = VK_FORMAT_UNDEFINED; VkColorSpaceKHR cs = 0;
  for (uint32_t i = 0; i < nf; i++) { LOG("surface format %u: %d colour space %d", i, sf[i].format, sf[i].colorSpace); if (fmt == VK_FORMAT_UNDEFINED && sf[i].format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = sf[i].format; cs = sf[i].colorSpace; } }
  if (fmt == VK_FORMAT_UNDEFINED) {
    for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM) { fmt = sf[i].format; cs = sf[i].colorSpace; }
    if (fmt == VK_FORMAT_UNDEFINED) { fmt = sf[0].format; cs = sf[0].colorSpace; }
    LOG("no B8G8R8A8_UNORM swapchain format: using %d, so the frame is blitted (a raw copy would swap red and blue)", fmt);
  }
  uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, NULL); VkPresentModeKHR pm[8]; if (nm > 8) nm = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, pm);
  fprintf(stderr, "vkp: present modes:"); for (uint32_t i = 0; i < nm; i++) fprintf(stderr, " %s", mode_name(pm[i])); fputc('\n', stderr);

  int cpuwrite = env_int("X400_VKP_CPUWRITE", 1);
  VkPresentModeKHR order[3] = { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR };
  for (int k = 0; k < 3; k++) for (uint32_t i = 0; i < nm; i++) if (pm[i] == order[k]) { if (phase_vk(order[k], secs, caps, fmt, cs, cpuwrite)) return -1; }
  if (hold > 0) { LOG("holding fifo for %.0f s (screenshot now)", hold); phase_vk(VK_PRESENT_MODE_FIFO_KHR, hold, caps, fmt, cs, cpuwrite); }
  return 0;
}

JNIEXPORT jint JNICALL Java_X400Display_nRun(JNIEnv *env, jclass type, jobject surface, jstring file, jint width, jint height)
{
  (void)type; (void)file;
  W = (uint32_t)width; H = (uint32_t)height;
  ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
  if (!win) { LOG("no ANativeWindow"); return -1; }
  int rc = run(win, env_int("X400_VKP_SECS", 3), env_int("X400_VKP_HOLD", 0));
  LOG("done (%d)", rc);
  ANativeWindow_release(win);
  return rc < 0 ? -1 : 0;
}
