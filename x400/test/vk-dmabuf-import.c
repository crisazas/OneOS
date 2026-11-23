// linux-x400: can Samsung's Vulkan (Mali-G68) import a dma-buf made outside it as a VkImage, and do the GPU and the CPU then see the same memory?
// Stage C of removing the CPU copy needs this: the guest's frame is a dma-buf, and the host would copy it to the layer with the GPU.
//   1. dma-buf from /dev/dma_heap/<heap>, sized from the image's own requirements
//   2. VkImage (B8G8R8A8, tiling DRM_FORMAT_MODIFIER with DRM_FORMAT_MOD_LINEAR and an explicit rowPitch; plain VK_IMAGE_TILING_LINEAR + DMA_BUF is not
//      supported by this driver), external memory handle type DMA_BUF, imported with VkImportMemoryFdInfoKHR
//   3. CPU writes a pattern through mmap(dma-buf), the GPU copies the image to a host buffer, compare          (CPU -> GPU)
//   4. the GPU clears the image to a colour, the CPU reads it through mmap(dma-buf), compare                   (GPU -> CPU)
// Built with the NDK, run as root on the tablet with One UI up. Usage: vk-dmabuf-import [HEAP (default system)] [W H]
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int find_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
  for (uint32_t i = 0; i < mp->memoryTypeCount; i++) if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
  return -1;
}

static void sync_cpu(int fd, int start, int write) { struct dma_buf_sync s = { .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_READ | (write ? DMA_BUF_SYNC_WRITE : 0) }; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); }

int main(int argc, char **argv)
{
  const char *heap = argc > 1 ? argv[1] : "system";
  uint32_t W = argc > 3 ? atoi(argv[2]) : 2112, H = argc > 3 ? atoi(argv[3]) : 1320;

  VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
  VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 1; VkPhysicalDevice pd; CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
  VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
  printf("device %s, Vulkan %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

  VkPhysicalDeviceExternalImageFormatInfo efi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT pmi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &efi, 0 /* LINEAR */, VK_SHARING_MODE_EXCLUSIVE };
  VkPhysicalDeviceImageFormatInfo2 fi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &pmi, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0 };
  VkExternalImageFormatProperties efp = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
  VkImageFormatProperties2 fp = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &efp };
  VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &fp);
  printf("0. BGRA8 DRM_MODIFIER(LINEAR) + dma-buf: %d; importable=%d exportable=%d, max %ux%u\n", fr,
         !!(efp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
         !!(efp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
         fp.imageFormatProperties.maxExtent.width, fp.imageFormatProperties.maxExtent.height);
  if (fr) return 1;

  float prio = 1;
  VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
  const char *ext[] = { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME };
  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 5, .ppEnabledExtensionNames = ext };
  VkDevice dev; CK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
  PFN_vkGetMemoryFdPropertiesKHR getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);

  uint32_t pitch = (W * 4 + 63) & ~63u;
  VkSubresourceLayout pl = { .offset = 0, .size = 0 /* must be 0 for an explicit layout */, .rowPitch = pitch };
  VkImageDrmFormatModifierExplicitCreateInfoEXT dmi = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, 0 /* LINEAR */, 1, &pl };
  VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &dmi, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
  VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
  VkImage img; CK(vkCreateImage(dev, &ic, NULL, &img));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
  VkSubresourceLayout sl = pl;
  printf("1. image %ux%u: requirements %llu bytes, align %llu, memoryTypeBits 0x%x; rowPitch %llu (tight would be %u), offset %llu\n", W, H, (unsigned long long)mr.size,
         (unsigned long long)mr.alignment, mr.memoryTypeBits, (unsigned long long)sl.rowPitch, W * 4, (unsigned long long)sl.offset);

  char path[128]; snprintf(path, sizeof path, "/dev/dma_heap/%s", heap);
  int hfd = open(path, O_RDONLY | O_CLOEXEC);
  if (hfd < 0) { printf("open %s: %s\n", path, strerror(errno)); return 1; }
  size_t size = (mr.size > (size_t)pitch * H ? mr.size : (size_t)pitch * H); size = (size + 4095) & ~4095ull;
  struct dma_heap_allocation_data a = { .len = size, .fd_flags = O_RDWR | O_CLOEXEC };
  if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a)) { printf("dma_heap alloc %zu in %s: %s\n", size, heap, strerror(errno)); return 1; }
  printf("2. dma-buf in %s: %zu bytes, fd %d\n", heap, size, a.fd);

  VkMemoryFdPropertiesKHR fdp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
  VkResult pr = getFdProps(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, a.fd, &fdp);
  printf("   vkGetMemoryFdPropertiesKHR: %d, memoryTypeBits 0x%x\n", pr, fdp.memoryTypeBits);
  uint32_t bits = mr.memoryTypeBits & (pr == VK_SUCCESS ? fdp.memoryTypeBits : ~0u);
  int mt = find_type(&mp, bits, 0);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (bits & (1u << i)) printf("   usable memory type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
  if (mt < 0) { printf("no memory type fits both\n"); return 1; }

  VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE };
  VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup(a.fd) };
  VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, (uint32_t)mt };
  VkDeviceMemory mem; VkResult ar = vkAllocateMemory(dev, &mai, NULL, &mem);
  printf("3. vkAllocateMemory (import, type %d): %d\n", mt, ar);
  if (ar) return 1;
  CK(vkBindImageMemory(dev, img, mem, 0));
  printf("   bound\n");

  unsigned char *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
  if (m == MAP_FAILED) { printf("mmap dma-buf: %s\n", strerror(errno)); return 1; }

  size_t rb = (size_t)pitch * H;
  VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = rb, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
  VkBuffer buf; CK(vkCreateBuffer(dev, &bci, NULL, &buf));
  VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev, buf, &bmr);
  int ht = find_type(&mp, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo bai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bmr.size, (uint32_t)ht };
  VkDeviceMemory bmem; CK(vkAllocateMemory(dev, &bai, NULL, &bmem)); CK(vkBindBufferMemory(dev, buf, bmem, 0));
  unsigned char *bp; CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&bp));

  VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
  VkCommandPool pool; CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
  VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbi, &cb));
  VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkFence fence; CK(vkCreateFence(dev, &fci, NULL, &fence));
  VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

  // 4. CPU -> GPU: pattern per row = row number in the first 4 bytes, then a ramp
  sync_cpu(a.fd, 1, 1);
  for (uint32_t y = 0; y < H; y++) { unsigned char *r = m + sl.offset + (size_t)y * sl.rowPitch; for (uint32_t x = 0; x < W * 4; x++) r[x] = (unsigned char)(x + y * 7); }
  sync_cpu(a.fd, 0, 1);
  memset(bp, 0, rb);
  VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
  CK(vkBeginCommandBuffer(cb, &bi));
  VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
                              .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = range };
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
  VkBufferImageCopy bic = { .bufferRowLength = pitch / 4, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
  vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
  CK(vkEndCommandBuffer(cb));
  VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
  double t0 = now(); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)); double t1 = now();
  unsigned long bad = 0, firstbad = (unsigned long)-1;
  for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W * 4; x++) if (bp[(size_t)y * pitch + x] != (unsigned char)(x + y * 7)) { if (bad == 0) firstbad = (unsigned long)y * pitch + x; bad++; }
  printf("4. CPU wrote -> GPU copied the image to a buffer in %.1f ms: %lu bytes differ (of %u used)%s", (t1 - t0) * 1000, bad, W * 4 * H, bad ? "" : "  -> OK, the GPU sees what the CPU wrote\n");
  if (bad) printf(" (first at byte %lu: got 0x%02x, wanted 0x%02x)\n", firstbad, bp[firstbad], (unsigned char)((firstbad % pitch) + (firstbad / pitch) * 7));

  // 5. GPU -> CPU: clear to a colour, read through the dma-buf mapping
  CK(vkResetFences(dev, 1, &fence)); CK(vkResetCommandBuffer(cb, 0));
  CK(vkBeginCommandBuffer(cb, &bi));
  VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
  VkClearColorValue cc = { .float32 = { 0.0f, 0.5f, 1.0f, 1.0f } };
  vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
  CK(vkEndCommandBuffer(cb));
  CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
  sync_cpu(a.fd, 1, 0);
  const unsigned char *px = m + sl.offset;
  printf("5. GPU cleared the image to (0, 0.5, 1) BGRA: the CPU reads through the dma-buf B=%u G=%u R=%u A=%u at (0,0); at (W-1,H-1) B=%u G=%u R=%u A=%u\n", px[0], px[1], px[2], px[3],
         m[sl.offset + (size_t)(H - 1) * sl.rowPitch + (W - 1) * 4], m[sl.offset + (size_t)(H - 1) * sl.rowPitch + (W - 1) * 4 + 1], m[sl.offset + (size_t)(H - 1) * sl.rowPitch + (W - 1) * 4 + 2], m[sl.offset + (size_t)(H - 1) * sl.rowPitch + (W - 1) * 4 + 3]);
  sync_cpu(a.fd, 0, 0);
  printf("   expected B=255 G=128 R=0 A=255 (BGRA)\n");

  // 6. how long a whole-frame GPU copy of this image takes (what the display would pay per frame)
  double best = 1e9, sum = 0; int N = 30;
  for (int i = 0; i < N; i++) {
    CK(vkResetFences(dev, 1, &fence)); CK(vkResetCommandBuffer(cb, 0)); CK(vkBeginCommandBuffer(cb, &bi));
    VkImageMemoryBarrier b3 = b1; b3.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b3.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b3);
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    VkImageMemoryBarrier b4 = b1; b4.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b4.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b4.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b4.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b4);
    CK(vkEndCommandBuffer(cb));
    double s0 = now(); CK(vkQueueSubmit(q, 1, &si, fence)); CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)); double e = (now() - s0) * 1000;
    if (e < best) best = e; sum += e;
  }
  printf("6. GPU copy of the imported %ux%u image to a buffer: best %.2f ms, mean %.2f ms over %d (a CPU memcpy of the same bytes is the number to beat)\n", W, H, best, sum / N, N);
  { // what the helper pays today: a CPU copy of the frame out of the dma-buf mapping (the sum keeps the compiler from dropping it)
    unsigned char *tmp = malloc(rb); unsigned long sum2 = 0; double c0, e;
    memset(tmp, 1, rb);
    sync_cpu(a.fd, 1, 0);
    c0 = now(); for (int i = 0; i < 10; i++) { memcpy(tmp, m + sl.offset, (size_t)pitch * H); sum2 += tmp[(i * 4099) % rb]; __asm__ volatile("" ::: "memory"); } e = (now() - c0) * 100;
    sync_cpu(a.fd, 0, 0);
    printf("   CPU memcpy of %zu bytes OUT of the dma-buf mapping (heap %s): %.2f ms each (%.0f MB/s) [%lu]\n", rb, heap, e, rb / (e / 1000) / 1e6, sum2 & 1);
    c0 = now(); for (int i = 0; i < 10; i++) { memcpy(m + sl.offset, tmp, (size_t)pitch * H); __asm__ volatile("" ::: "memory"); } e = (now() - c0) * 100;
    printf("   CPU memcpy of %zu bytes INTO the dma-buf mapping: %.2f ms each (%.0f MB/s)\n", rb, e, rb / (e / 1000) / 1e6);
    free(tmp);
  }
  return 0;
}
