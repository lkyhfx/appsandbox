/* Vulkan D3D12 image -> CUDA external-memory import smoke test.
 * Uses a real Vulkan-exported OPAQUE_FD, never mislabels a DRM PRIME FD.
 * Successful import alone does NOT validate pixels or fence interoperability.
 * gcc -Wall -O2 -I vulkan-dev/usr/include zero-copy-vulkan.c -l:libvulkan.so.1 -ldl -o vulkan-probe
 */
#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>

/* CUDA Driver API ABI: CUDA_EXTERNAL_MEMORY_HANDLE_DESC (opaque FD type=1).
 * https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__EXTRES__INTEROP.html
 */
struct cuda_external_desc {
    int type;
    union { int fd; struct { void *handle; const void *name; } win32; const void *nvSciBufObject; } handle;
    unsigned long long size;
    unsigned flags;
    unsigned reserved[16];
};
_Static_assert(offsetof(struct cuda_external_desc, size) == 24, "64-bit CUDA ABI");
_Static_assert(sizeof(struct cuda_external_desc) == 104, "CUDA descriptor ABI");
#define VKCHECK(call) do { VkResult r = (call); if (r) { \
    fprintf(stderr, "%s failed: %d\n", #call, r); return 1; } } while (0)
#define CULOAD(name, args) int (*name) args = dlsym(cuda, #name); \
    if (!name) { fprintf(stderr, "missing %s\n", #name); return 1; }
#define CUCHECK(call) do { int r = (call); if (r) { \
    fprintf(stderr, "%s failed: %d\n", #call, r); return 1; } } while (0)
int main(int argc, char **argv) {
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app};
    VkInstance inst; VKCHECK(vkCreateInstance(&ci, NULL, &inst));
    uint32_t count = 0; VKCHECK(vkEnumeratePhysicalDevices(inst, &count, NULL));
    if (!count) { puts("No Vulkan device"); return 2; }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    VKCHECK(vkEnumeratePhysicalDevices(inst, &count, devices));
    VkPhysicalDevice gpu = devices[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu, &props);
    printf("Vulkan GPU=%s API=%u.%u.%u\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    uint32_t n = 0; VKCHECK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, NULL));
    VkExtensionProperties *exts = calloc(n, sizeof(*exts));
    VKCHECK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, exts));
    int has_fd = 0;
    for (uint32_t i=0; i<n; i++) {
        if (strstr(exts[i].extensionName, "external_")) puts(exts[i].extensionName);
        if (!strcmp(exts[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) has_fd=1;
    }
    free(exts);
    VkPhysicalDeviceExternalSemaphoreInfo si = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkExternalSemaphoreProperties sp = {.sType=VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(gpu, &si, &sp);
    printf("opaque-FD semaphore features=0x%x compatible=0x%x\n", sp.externalSemaphoreFeatures, sp.compatibleHandleTypes);
    if (!has_fd) { puts("BLOCKED: VK_KHR_external_memory_fd not exposed"); return 2; }
    VkPhysicalDeviceExternalImageFormatInfo ei = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext=&ei, .format=VK_FORMAT_B8G8R8A8_UNORM, .type=VK_IMAGE_TYPE_2D,
        .tiling=(argc > 1 && !strcmp(argv[1], "linear")) ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
    printf("Test image=BGRA8 tiling=%s\n", fi.tiling == VK_IMAGE_TILING_LINEAR ? "linear" : "optimal");
    VkExternalImageFormatProperties ep = {.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ip = {.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext=&ep};
    VKCHECK(vkGetPhysicalDeviceImageFormatProperties2(gpu, &fi, &ip));
    printf("opaque-FD image external features=0x%x\n", ep.externalMemoryProperties.externalMemoryFeatures);
    if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        puts("BLOCKED: GPU image not exportable as opaque FD"); return 2;
    }
    uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, NULL);
    VkQueueFamilyProperties *queues=calloc(nq, sizeof(*queues));
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, queues);
    uint32_t family=0;
    while (family<nq && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family==nq) return 2;
    float priority=1;
    VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
    const char *enabled[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo di = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1,
        .pQueueCreateInfos=&qi, .enabledExtensionCount=1, .ppEnabledExtensionNames=enabled};
    VkDevice device; VKCHECK(vkCreateDevice(gpu, &di, NULL, &device));
    VkExternalMemoryImageCreateInfo external = {.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo imageinfo = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext=&external,
        .imageType=VK_IMAGE_TYPE_2D, .format=fi.format, .extent={256,256,1}, .mipLevels=1, .arrayLayers=1,
        .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=fi.tiling, .usage=fi.usage,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image; VKCHECK(vkCreateImage(device, &imageinfo, NULL, &image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, image, &req);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    uint32_t type=0;
    while (type<memory.memoryTypeCount && (!(req.memoryTypeBits & (1u<<type)) ||
          !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) type++;
    if (type==memory.memoryTypeCount) return 2;
    VkMemoryDedicatedAllocateInfo dedicated = {.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image=image};
    VkExportMemoryAllocateInfo exportinfo = {.sType=VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext=&dedicated, .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo alloc = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext=&exportinfo, .allocationSize=req.size, .memoryTypeIndex=type};
    VkDeviceMemory mem; VKCHECK(vkAllocateMemory(device, &alloc, NULL, &mem));
    VKCHECK(vkBindImageMemory(device, image, mem, 0));
    PFN_vkGetMemoryFdKHR getfd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    if (!getfd) return 2;
    VkMemoryGetFdInfoKHR getinfo = {.sType=VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory=mem, .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    int fd=-1; VKCHECK(getfd(device, &getinfo, &fd));
    printf("Exported real GPU image: size=%llu memory_flags=0x%x fd=%d\n",
           (unsigned long long)req.size, memory.memoryTypes[type].propertyFlags, fd);
    char path[128], line[256]; snprintf(path,sizeof(path),"/proc/self/fdinfo/%d",fd);
    FILE *f=fopen(path,"r"); if(f) { while(fgets(line,sizeof(line),f)) fputs(line,stdout); fclose(f); }
    void *cuda=dlopen("libcuda.so.1",RTLD_NOW); if(!cuda) { puts(dlerror()); return 1; }
    CULOAD(cuInit,(unsigned)); CULOAD(cuDeviceGet,(int *,int));
    CULOAD(cuCtxCreate_v2,(void **,unsigned,int));
    CULOAD(cuImportExternalMemory,(void **,const struct cuda_external_desc *));
    CULOAD(cuDestroyExternalMemory,(void *)); CULOAD(cuCtxDestroy_v2,(void *));
    CULOAD(cuGetErrorName,(int,const char **));
    CUCHECK(cuInit(0)); int cudev; void *ctx; CUCHECK(cuDeviceGet(&cudev,0));
    CUCHECK(cuCtxCreate_v2(&ctx,0,cudev));
    struct cuda_external_desc desc={0}; desc.type=1; desc.handle.fd=fd;
    desc.size=req.size; desc.flags=1; /* CUDA_EXTERNAL_MEMORY_DEDICATED */
    void *imported=NULL; int result=cuImportExternalMemory(&imported,&desc);
    const char *error="unknown"; cuGetErrorName(result,&error);
    printf("cuImportExternalMemory=%d (%s)\n",result,error);
    if(!result) { puts("IMPORT ONLY passed; mapping, pixels and fences NOT validated"); cuDestroyExternalMemory(imported); }
    else close(fd);
    cuCtxDestroy_v2(ctx); vkDestroyImage(device,image,NULL); vkFreeMemory(device,mem,NULL);
    vkDestroyDevice(device,NULL); vkDestroyInstance(inst,NULL); free(queues); free(devices);
    return result ? 3 : 0;
}
