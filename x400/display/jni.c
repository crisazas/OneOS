// Credit: the window copy follows OneiOS's runtime/display/src/jni.c (MIT), by Akshay CM (akshaynexus), Musicalista and contributors,
// https://github.com/Musicalista/OneiOS-A56. Written here, not copied. See x400/CREDITS.md.
// linux-x400: liboneios-style native half of X400Display: maps the frame file and copies each new frame into the layer.
#define _GNU_SOURCE
#include "fb.h"
#include "fdframe.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <errno.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <fcntl.h>
#include <jni.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

static volatile int stopping;
static void on_signal(int n) { (void)n; stopping = 1; }

// Frames by descriptor (fdframe.h): capture keeps a PipeWire buffer and sends its descriptor. A memfd is read by the CPU and copied into the
// layer (ANativeWindow_lock). A dma-buf is imported as a VkImage and copied into the layer's swapchain by the GPU (present mode mailbox: the thread
// that has to answer DONE to capture never waits for the display). One surface cannot have both: the path is chosen once, from the first frame.
struct fdbuf { uint32_t id; uint8_t *base; size_t size; VkImage img; VkDeviceMemory mem; };
static struct fdbuf fdbufs[32];

#define VKP_FRAMES 2
static struct {
  int up, failed; VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; VkSurfaceKHR surf; VkSwapchainKHR sc; VkCommandPool pool;
  VkImage imgs[16]; uint32_t nimg; VkExtent2D ext; VkFormat fmt; VkColorSpaceKHR cs; VkPresentModeKHR mode; int portrait;
  VkSurfaceTransformFlagBitsKHR pre_cur;
  struct { VkCommandBuffer cb; VkSemaphore acq, done; VkFence fence; } f[VKP_FRAMES]; uint32_t cur;
  PFN_vkGetMemoryFdPropertiesKHR getFdProps;
  uint64_t blits, acquire_max_us, acquire_sum_us, submit_sum_us;
} vk;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

static int sock_open(const char *path)
{
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s.sock", path) >= (int)sizeof sa.sun_path) return -1;
  int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (s < 0) return -1;
  if (connect(s, (struct sockaddr *)&sa, sizeof sa)) { close(s); return -1; }
  return s;
}

static void fdbuf_drop(uint32_t id)
{
  for (int i = 0; i < 32; i++) if (fdbufs[i].id == id && (fdbufs[i].base || fdbufs[i].img)) {
    if (fdbufs[i].img) { vkDeviceWaitIdle(vk.dev); vkDestroyImage(vk.dev, fdbufs[i].img, NULL); vkFreeMemory(vk.dev, fdbufs[i].mem, NULL); }
    if (fdbufs[i].base) munmap(fdbufs[i].base, fdbufs[i].size);
    memset(&fdbufs[i], 0, sizeof fdbufs[i]);
  }
}

static void fdbuf_clear(void) { for (int i = 0; i < 32; i++) if (fdbufs[i].base || fdbufs[i].img) fdbuf_drop(fdbufs[i].id); }


// ---- the Vulkan path ----
static void vk_log(const char *what, VkResult r) { fprintf(stderr, "x400-display: vulkan: %s = %d\n", what, r); }
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { vk_log(#x, r_); return -1; } } while (0)

// (Re)creates the swapchain for a landscape (layer-sized, preTransform IDENTITY: SurfaceFlinger turns it with the display) or a portrait frame
// (the panel's native 1320x2112, preTransform = the surface's current transform: the buffer is already in the panel's own orientation, nothing turns it).
static int vk_swapchain(int width, int height, int portrait)
{
  VkSurfaceCapabilitiesKHR caps; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.pd, vk.surf, &caps));
  VkSurfaceTransformFlagBitsKHR pre = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  if (portrait) pre = caps.currentTransform;
  else if (!(caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)) pre = caps.currentTransform;
  vk.ext = portrait ? (VkExtent2D){ (uint32_t)(width < height ? width : height), (uint32_t)(width < height ? height : width) } : (VkExtent2D){ (uint32_t)(width < height ? height : width), (uint32_t)(width < height ? width : height) };
  uint32_t minimg = 5; if (minimg < caps.minImageCount) minimg = caps.minImageCount; if (caps.maxImageCount && minimg > caps.maxImageCount) minimg = caps.maxImageCount;
  VkSwapchainKHR old = vk.sc;
  if (old) { vkDeviceWaitIdle(vk.dev); vkDestroySwapchainKHR(vk.dev, old, NULL); vk.sc = VK_NULL_HANDLE; }
  VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surf, .minImageCount = minimg, .imageFormat = vk.fmt, .imageColorSpace = vk.cs, .imageExtent = vk.ext,
                                   .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = pre,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = vk.mode, .clipped = VK_TRUE };
  VKC(vkCreateSwapchainKHR(vk.dev, &sci, NULL, &vk.sc));
  vk.nimg = 0; vkGetSwapchainImagesKHR(vk.dev, vk.sc, &vk.nimg, NULL); if (vk.nimg > 16) vk.nimg = 16; vkGetSwapchainImagesKHR(vk.dev, vk.sc, &vk.nimg, vk.imgs);
  vk.portrait = portrait; vk.pre_cur = caps.currentTransform;
  if (old) fprintf(stderr, "x400-display: vulkan: swapchain %ux%u (%s, preTransform %d, surface current %d extent %ux%u)\n", vk.ext.width, vk.ext.height, portrait ? "portrait" : "landscape", (int)pre, (int)caps.currentTransform, caps.currentExtent.width, caps.currentExtent.height);
  return 0;
}

// Brings up the instance, device and a swapchain on the layer's window. Returns 0, or -1 (then the CPU path is used and Vulkan is left alone).
static int vk_init(ANativeWindow *win, int width, int height)
{
  uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
  VkExtensionProperties *ep = calloc(ne + 1, sizeof *ep); vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
  int has = 0; for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_android_surface")) has = 1;
  free(ep);
  if (!has) { fprintf(stderr, "x400-display: vulkan: no VK_KHR_android_surface\n"); return -1; }
  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
  const char *iext[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
  VKC(vkCreateInstance(&ici, NULL, &vk.inst));
  uint32_t n = 1; VKC(vkEnumeratePhysicalDevices(vk.inst, &n, &vk.pd));
  VkAndroidSurfaceCreateInfoKHR asi = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = win };
  VKC(vkCreateAndroidSurfaceKHR(vk.inst, &asi, NULL, &vk.surf));
  VkBool32 sup = 0; VKC(vkGetPhysicalDeviceSurfaceSupportKHR(vk.pd, 0, vk.surf, &sup)); if (!sup) { fprintf(stderr, "x400-display: vulkan: queue 0 cannot present\n"); return -1; }
  float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
  const char *dext[] = { "VK_KHR_swapchain", VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                         VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME };
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 6, .ppEnabledExtensionNames = dext };
  VKC(vkCreateDevice(vk.pd, &dci, NULL, &vk.dev)); vkGetDeviceQueue(vk.dev, 0, 0, &vk.q);
  vk.getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk.dev, "vkGetMemoryFdPropertiesKHR");
  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
  VKC(vkCreateCommandPool(vk.dev, &cpi, NULL, &vk.pool));

  VkSurfaceCapabilitiesKHR caps; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.pd, vk.surf, &caps));
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fprintf(stderr, "x400-display: vulkan: the surface cannot be a transfer destination\n"); return -1; }
  uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(vk.pd, vk.surf, &nf, NULL); VkSurfaceFormatKHR sf[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(vk.pd, vk.surf, &nf, sf);
  vk.cs = 0; vk.fmt = VK_FORMAT_UNDEFINED;
  // the swapchain of this driver is RGBA only; the frames are BGRA, so each one is blitted (a raw copy would swap red and blue)
  for (uint32_t i = 0; i < nf; i++) if (sf[i].format == VK_FORMAT_B8G8R8A8_UNORM) { vk.fmt = sf[i].format; vk.cs = sf[i].colorSpace; }
  for (uint32_t i = 0; vk.fmt == VK_FORMAT_UNDEFINED && i < nf; i++) if (sf[i].format == VK_FORMAT_R8G8B8A8_UNORM) { vk.fmt = sf[i].format; vk.cs = sf[i].colorSpace; }
  if (vk.fmt == VK_FORMAT_UNDEFINED) { fprintf(stderr, "x400-display: vulkan: no BGRA or RGBA swapchain format\n"); return -1; }
  uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(vk.pd, vk.surf, &nm, NULL); VkPresentModeKHR pm[16]; if (nm > 16) nm = 16; vkGetPhysicalDeviceSurfacePresentModesKHR(vk.pd, vk.surf, &nm, pm);
  vk.mode = VK_PRESENT_MODE_FIFO_KHR; int have_mailbox = 0;
  for (uint32_t i = 0; i < nm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) have_mailbox = 1;
  if (have_mailbox && !getenv("X400_FIFO")) vk.mode = VK_PRESENT_MODE_MAILBOX_KHR;
  if (vk_swapchain(width, height, 0)) return -1;
  VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
  for (int i = 0; i < VKP_FRAMES; i++) { VKC(vkAllocateCommandBuffers(vk.dev, &cbi, &vk.f[i].cb)); VKC(vkCreateSemaphore(vk.dev, &sem, NULL, &vk.f[i].acq));
                                         VKC(vkCreateSemaphore(vk.dev, &sem, NULL, &vk.f[i].done)); VKC(vkCreateFence(vk.dev, &fci, NULL, &vk.f[i].fence)); }
  fprintf(stderr, "x400-display: vulkan up: swapchain %u images %ux%u format %d, present mode %s\n", vk.nimg, vk.ext.width, vk.ext.height, vk.fmt, vk.mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
  vk.up = 1;
  return 0;
}

// imports a dma-buf (linear BGRA, one plane) as a VkImage; takes ownership of nothing: it dups fd
static int vk_import(struct fdbuf *fb, int fd, const struct fd_msg *m)
{
  VkSubresourceLayout pl = { .offset = m->offset, .size = 0, .rowPitch = m->stride };
  VkImageDrmFormatModifierExplicitCreateInfoEXT dmi = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, 0 /* LINEAR */, 1, &pl };
  VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &dmi, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
  VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { m->width, m->height, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
  VKC(vkCreateImage(vk.dev, &ic, NULL, &fb->img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(vk.dev, fb->img, &mr);
  VkMemoryFdPropertiesKHR fdp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
  VKC(vk.getFdProps(vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp));
  uint32_t bits = mr.memoryTypeBits & fdp.memoryTypeBits, mt = 0; while (mt < 32 && !(bits & (1u << mt))) mt++;
  if (mt == 32) { fprintf(stderr, "x400-display: vulkan: no memory type fits the dma-buf\n"); return -1; }
  VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, fb->img, VK_NULL_HANDLE };
  VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup(fd) };
  VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, mt };
  VKC(vkAllocateMemory(vk.dev, &mai, NULL, &fb->mem)); VKC(vkBindImageMemory(vk.dev, fb->img, fb->mem, 0));
  return 0;
}

// Blits one frame into the swapchain and presents it. The image is in PREINITIALIZED the first time and TRANSFER_SRC after: one barrier handles both with UNDEFINED
// (the contents are written by KWin's GPU work, not by us, and a linear image keeps them through a layout change).
// Returns 0 shown, -1 failed (the caller drops to the CPU path for good).
static int vk_show(struct fdbuf *fb, const struct fd_msg *m, int flip)
{
  uint64_t t0 = now_us();
  uint32_t k = vk.cur; vk.cur = (vk.cur + 1) % VKP_FRAMES;
  vkWaitForFences(vk.dev, 1, &vk.f[k].fence, VK_TRUE, UINT64_MAX); vkResetFences(vk.dev, 1, &vk.f[k].fence);
  uint32_t idx; VkResult r = vkAcquireNextImageKHR(vk.dev, vk.sc, UINT64_MAX, vk.f[k].acq, VK_NULL_HANDLE, &idx);
  uint64_t t1 = now_us();
  if (r == VK_ERROR_OUT_OF_DATE_KHR) { VkSubmitInfo e = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; vkQueueSubmit(vk.q, 1, &e, vk.f[k].fence); return 1; }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_log("vkAcquireNextImageKHR", r); return -1; }
  vkResetCommandBuffer(vk.f[k].cb, 0);
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
  vkBeginCommandBuffer(vk.f[k].cb, &bi);
  VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  VkImageMemoryBarrier bs = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = fb->img, .subresourceRange = rg };
  VkImageMemoryBarrier bd = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = vk.imgs[idx], .subresourceRange = rg };
  VkImageMemoryBarrier bb[2] = { bs, bd };
  vkCmdPipelineBarrier(vk.f[k].cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, bb);
  int32_t w = (int32_t)(vk.ext.width < m->width ? vk.ext.width : m->width), h = (int32_t)(vk.ext.height < m->height ? vk.ext.height : m->height);
  // a turn by 180 degrees is a blit with the destination corners swapped: no shader needed
  VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { w, h, 1 } }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                     .dstOffsets = { { flip ? w : 0, flip ? h : 0, 0 }, { flip ? 0 : w, flip ? 0 : h, 1 } } };
  vkCmdBlitImage(vk.f[k].cb, fb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
  VkImageMemoryBarrier bp = bd; bp.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bp.dstAccessMask = 0; bp.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bp.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(vk.f[k].cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &bp);
  vkEndCommandBuffer(vk.f[k].cb);
  VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.f[k].acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &vk.f[k].cb,
                      .signalSemaphoreCount = 1, .pSignalSemaphores = &vk.f[k].done };
  r = vkQueueSubmit(vk.q, 1, &si, vk.f[k].fence);
  if (r) { vk_log("vkQueueSubmit", r); return -1; }
  VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &vk.f[k].done, .swapchainCount = 1, .pSwapchains = &vk.sc, .pImageIndices = &idx };
  r = vkQueuePresentKHR(vk.q, &pi);
  uint64_t t2 = now_us();
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_log("vkQueuePresentKHR", r); return -1; }
  vk.blits++; vk.acquire_sum_us += t1 - t0; if (t1 - t0 > vk.acquire_max_us) vk.acquire_max_us = t1 - t0; vk.submit_sum_us += t2 - t1;
  return 0;
}

// 1: a frame was shown (or skipped), 0: nothing yet, -1: the socket is gone.
static int path_chosen, geometry_set;
static unsigned flip_mask = 0x6;   // bit n: the frames of the output transform n are blitted turned by 180 degrees; X400_FLIP overrides (a test knob)
static void cpu_geometry(ANativeWindow *win, int width, int height)
{
  if (geometry_set) return;
  geometry_set = 1;
  if (ANativeWindow_setBuffersGeometry(win, width, height, 5 /* BGRA_8888 */) < 0) fprintf(stderr, "x400-display: setBuffersGeometry failed\n");
}
static int sock_frame(int s, ANativeWindow *win, int width, int height, uint64_t *frames)
{
  struct fd_msg m; struct iovec iov = { &m, sizeof m };
  char cb[CMSG_SPACE(sizeof(int))];
  struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
  ssize_t n = recvmsg(s, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  if (n == 0) return -1;
  if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
  int fd = -1;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
  if (n != (ssize_t)sizeof m) { if (fd >= 0) close(fd); return 0; }
  if (m.type == FDM_DROP) { fdbuf_drop(m.id); if (fd >= 0) close(fd); return 0; }
  if (m.type != FDM_FRAME) { if (fd >= 0) close(fd); return 0; }
  struct fdbuf *fb = NULL;
  for (int i = 0; i < 32; i++) if ((fdbufs[i].base || fdbufs[i].img) && fdbufs[i].id == m.id) fb = &fdbufs[i];
  // the first frame fixes the path for this surface: a dma-buf goes by Vulkan if it comes up, anything else by the CPU
  if (!vk.up && !vk.failed && !path_chosen) { path_chosen = 1; if ((m.flags & FDF_DMABUF) && !getenv("X400_NOVULKAN") && vk_init(win, width, height)) vk.failed = 1; }
  if (!fb && fd >= 0) {
    if ((m.flags & FDF_DMABUF) && vk.up) {
      for (int i = 0; i < 32 && !fb; i++) if (!fdbufs[i].base && !fdbufs[i].img) { fdbufs[i].id = m.id; if (vk_import(&fdbufs[i], fd, &m)) { fdbuf_drop(m.id); vk.failed = 1; vk.up = 0; } else fb = &fdbufs[i]; }
    } else if (!(m.flags & FDF_DMABUF)) {
      size_t size = (size_t)m.mapoffset + m.maxsize;
      uint8_t *base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
      if (base != MAP_FAILED) for (int i = 0; i < 32; i++) if (!fdbufs[i].base && !fdbufs[i].img) { fdbufs[i] = (struct fdbuf){ .id = m.id, .base = base, .size = size }; fb = &fdbufs[i]; break; }
      if (!fb && base != MAP_FAILED) munmap(base, size);
    }
  }
  if (fd >= 0) close(fd);
  struct fd_reply r = { FDM_DONE, m.id };
  if (fb && fb->img && vk.up) {
    // The frame is the upright desktop. Landscape frames fill the layer (SurfaceFlinger turns them with the display); a portrait frame is the panel's native
    // orientation and needs a swapchain of its own. Measured with screencap (ROTATION.md): the frames of transform 1 (left) and 2 (inverted) come out
    // 180 degrees off in the layer, so they are blitted with the destination corners swapped; those of 0 and 3 (right) are not.
    int portrait = m.height > m.width, rot = (int)((m.flags >> FDF_ROT_SHIFT) & FDF_ROT_MASK);
    int flip = (flip_mask >> rot) & 1;
    if (portrait != vk.portrait) { if (vk_swapchain(width, height, portrait)) { vk.up = 0; vk.failed = 1; } }
    if (!vk.up) { fprintf(stderr, "x400-display: vulkan failed while resizing\n"); }
    else if (m.width == vk.ext.width && m.height == vk.ext.height) {
      int rc = vk_show(fb, &m, flip);
      if (rc < 0) { vk.up = 0; vk.failed = 1; fprintf(stderr, "x400-display: vulkan failed: this surface cannot be drawn with the CPU any more; the layer will be recreated at the next session\n"); }
      else if (rc == 0) (*frames)++;
    }
  } else if (fb && fb->base && m.width == (uint32_t)width && m.height == (uint32_t)height && !vk.up) {
    cpu_geometry(win, width, height);
    ANativeWindow_Buffer b;
    if (ANativeWindow_lock(win, &b, NULL) >= 0) {
      const uint8_t *src = fb->base + m.mapoffset + m.offset;
      uint32_t rows = (uint32_t)height < (uint32_t)b.height ? (uint32_t)height : (uint32_t)b.height, row = (uint32_t)width * 4;
      for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, src + (size_t)y * m.stride, row);
      ANativeWindow_unlockAndPost(win);
      (*frames)++;
    }
  }
  if (send(s, &r, sizeof r, MSG_NOSIGNAL) != (ssize_t)sizeof r) return -1;
  return 1;
}

JNIEXPORT jint JNICALL Java_X400Display_nRun(JNIEnv *env, jclass type, jobject surface, jstring file, jint width, jint height)
{
  (void)type;
  struct sigaction sa = { .sa_handler = on_signal };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
  if (getenv("X400_FLIP")) flip_mask = (unsigned)strtoul(getenv("X400_FLIP"), NULL, 0);
  const char *path = (*env)->GetStringUTFChars(env, file, NULL);
  char path_copy[256]; snprintf(path_copy, sizeof path_copy, "%s", path);
  // The guest creates the file when its capture starts, which can be after the layer is up: wait for it (up to 5 minutes).
  int fd = -1;
  for (int waited = 0; fd < 0 && waited < 3000 && !stopping; waited++) {
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) usleep(100000);
  }
  if (fd < 0) { fprintf(stderr, "x400-display: %s: %s\n", path, stopping ? "stopped while waiting" : strerror(errno)); (*env)->ReleaseStringUTFChars(env, file, path); return -1; }
  (*env)->ReleaseStringUTFChars(env, file, path);
  struct stat st; fstat(fd, &st);
  size_t size = (size_t)st.st_size;
  uint8_t *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED || size < FB_PIXELS) { fprintf(stderr, "x400-display: cannot map the frame file (%zu bytes)\n", size); return -1; }
  const struct fb_header *h = (const struct fb_header *)map;
  ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
  if (!win) return -1;
  // Not touched here: ANativeWindow_setBuffersGeometry would claim the surface for the CPU path, and a Vulkan swapchain could no longer be created on it.
  // The CPU path sets it when it first draws.
  uint64_t shown = 0, frames = 0, last_frames = 0;
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
  int sock = -1, tries = 0; int by_fd = getenv("X400_NOFD") == NULL;
  while (!stopping) {
    if (by_fd && sock < 0 && tries++ % 500 == 0 && (sock = sock_open(path_copy)) >= 0) fprintf(stderr, "x400-display: frames by descriptor from capture\n");
    if (sock >= 0) {
      struct pollfd pf = { sock, POLLIN, 0 };
      if (poll(&pf, 1, 100) > 0) {
        int r = sock_frame(sock, win, width, height, &frames);
        if (r < 0) { close(sock); sock = -1; fdbuf_clear(); fprintf(stderr, "x400-display: the capture socket is gone: frame file again\n"); }
      }
      if (sock >= 0) goto stats;
    }
    uint64_t seq = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
    if (seq == shown || h->magic != FB_MAGIC || h->width != (uint32_t)width || h->height != (uint32_t)height) { usleep(2000); continue; }
    ANativeWindow_Buffer b;
    cpu_geometry(win, width, height);
    if (ANativeWindow_lock(win, &b, NULL) < 0) { usleep(5000); continue; }
    const uint8_t *src = map + FB_PIXELS;
    uint32_t rows = (uint32_t)height < (uint32_t)b.height ? (uint32_t)height : (uint32_t)b.height;
    uint32_t row = (uint32_t)width * 4;
    for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, src + (size_t)y * h->stride, row);
    ANativeWindow_unlockAndPost(win);
    shown = seq; frames++;
  stats:
    if (frames - last_frames >= 300) {
      last_frames = frames;
      struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
      double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
      fprintf(stderr, "x400-display: %llu frames shown, %.1f fps over the last %.1f s", (unsigned long long)frames, 300 / dt, dt);
      if (vk.up && vk.blits) fprintf(stderr, " (vulkan: acquire mean %.2f ms max %.2f ms, record+submit+present mean %.2f ms)", vk.acquire_sum_us / 1000.0 / vk.blits, vk.acquire_max_us / 1000.0, vk.submit_sum_us / 1000.0 / vk.blits);
      fputc('\n', stderr); vk.acquire_max_us = 0; vk.acquire_sum_us = 0; vk.submit_sum_us = 0; vk.blits = 0;
      t0 = t1;
    }
  }
  ANativeWindow_release(win);
  munmap(map, size); close(fd);
  return 0;
}
