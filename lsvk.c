/* lsvk.c -- Vulkan loader and bindings for Litesrpent.
 *
 * Dynamically loads vulkan-1.dll, defines minimal Vulkan structs inline
 * (no vulkan.h required), and exposes Lisp-level instance/device/queue/
 * command buffer/render pass/pipeline operations.
 */
#include "lscore.h"
#include "lseval.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
#else
#include <dlfcn.h>
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
#endif

/* Minimal Vulkan type definitions */
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef int32_t  VkResult;

VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkCommandPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkRenderPass)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFramebuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipeline)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFence)

#define VK_SUCCESS                      0
#define VK_STRUCTURE_TYPE_APPLICATION_INFO      0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO  1
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO  2
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO    3
#define VK_STRUCTURE_TYPE_SUBMIT_INFO           4
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO 39
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO 42
#define VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO 43
#define VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO 38
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY 0
#define VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT 0x1
#define VK_SUBPASS_CONTENTS_INLINE      0
#define VK_PIPELINE_BIND_POINT_GRAPHICS 0
#define VK_API_VERSION_1_0              ((1u<<22)|(0u<<12)|0u)
#define VK_QUEUE_GRAPHICS_BIT           0x1
#define VK_FORMAT_B8G8R8A8_UNORM        44
#define VK_ATTACHMENT_LOAD_OP_CLEAR     1
#define VK_ATTACHMENT_STORE_OP_STORE    0
#define VK_IMAGE_LAYOUT_UNDEFINED       0
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR 1000001002
#define VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL 2

typedef struct VkApplicationInfo {
    uint32_t sType; const void *pNext; const char *pApplicationName;
    uint32_t applicationVersion; const char *pEngineName;
    uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkPhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
    char deviceName[256]; uint8_t pipelineCacheUUID[16];
    /* limits and sparse properties omitted for brevity */
    uint8_t _padding[4096];
} VkPhysicalDeviceProperties;

typedef struct VkQueueFamilyProperties {
    VkFlags queueFlags; uint32_t queueCount;
    uint32_t timestampValidBits; uint32_t minImageTransferGranularity[3];
} VkQueueFamilyProperties;

typedef struct VkDeviceQueueCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex; uint32_t queueCount;
    const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueCreateInfoCount;
    const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkCommandPoolCreateInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
    uint32_t sType; const void *pNext;
    VkCommandPool commandPool; uint32_t level; uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkCommandBufferBeginInfo {
    uint32_t sType; const void *pNext; VkFlags flags;
    const void *pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct VkClearColorValue {
    float float32[4];
} VkClearColorValue;

typedef struct VkClearValue {
    VkClearColorValue color;
} VkClearValue;

typedef struct VkRect2D {
    int32_t offset[2]; uint32_t extent[2];
} VkRect2D;

typedef struct VkRenderPassBeginInfo {
    uint32_t sType; const void *pNext;
    VkRenderPass renderPass; VkFramebuffer framebuffer;
    VkRect2D renderArea;
    uint32_t clearValueCount; const VkClearValue *pClearValues;
} VkRenderPassBeginInfo;

typedef struct VkSubmitInfo {
    uint32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    const VkFlags *pWaitDstStageMask;
    uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;

/* Function pointer types */
typedef void* (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef void     (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void     (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
typedef void     (*PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void     (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);
typedef VkResult (*PFN_vkCreateCommandPool)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
typedef VkResult (*PFN_vkAllocateCommandBuffers)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
typedef VkResult (*PFN_vkBeginCommandBuffer)(VkCommandBuffer, const VkCommandBufferBeginInfo*);
typedef VkResult (*PFN_vkEndCommandBuffer)(VkCommandBuffer);
typedef VkResult (*PFN_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
typedef VkResult (*PFN_vkQueueWaitIdle)(VkQueue);
typedef VkResult (*PFN_vkDeviceWaitIdle)(VkDevice);
typedef void     (*PFN_vkCmdBeginRenderPass)(VkCommandBuffer, const VkRenderPassBeginInfo*, uint32_t);
typedef void     (*PFN_vkCmdEndRenderPass)(VkCommandBuffer);
typedef void     (*PFN_vkCmdBindPipeline)(VkCommandBuffer, uint32_t, VkPipeline);
typedef void     (*PFN_vkCmdDraw)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void     (*PFN_vkDestroyCommandPool)(VkDevice, VkCommandPool, const void*);

static PFN_vkGetInstanceProcAddr  pfn_vkGetInstanceProcAddr;
static PFN_vkCreateInstance       pfn_vkCreateInstance;
static PFN_vkDestroyInstance      pfn_vkDestroyInstance;
static PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties pfn_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkCreateDevice         pfn_vkCreateDevice;
static PFN_vkDestroyDevice        pfn_vkDestroyDevice;
static PFN_vkGetDeviceQueue       pfn_vkGetDeviceQueue;
static PFN_vkCreateCommandPool    pfn_vkCreateCommandPool;
static PFN_vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer   pfn_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer     pfn_vkEndCommandBuffer;
static PFN_vkQueueSubmit          pfn_vkQueueSubmit;
static PFN_vkQueueWaitIdle        pfn_vkQueueWaitIdle;
static PFN_vkDeviceWaitIdle       pfn_vkDeviceWaitIdle;
static PFN_vkCmdBeginRenderPass   pfn_vkCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass     pfn_vkCmdEndRenderPass;
static PFN_vkCmdBindPipeline      pfn_vkCmdBindPipeline;
static PFN_vkCmdDraw              pfn_vkCmdDraw;
static PFN_vkDestroyCommandPool   pfn_vkDestroyCommandPool;

#ifdef _WIN32
static HMODULE vk_lib = NULL;
#else
static void *vk_lib = NULL;
#endif
static int vk_loaded = 0;

static int load_vk(void) {
    if (vk_loaded) return 1;
#ifdef _WIN32
    vk_lib = LoadLibraryA("vulkan-1.dll");
    if (!vk_lib) return 0;
    pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk_lib, "vkGetInstanceProcAddr");
#else
    vk_lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!vk_lib) return 0;
    pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vk_lib, "vkGetInstanceProcAddr");
#endif
    if (!pfn_vkGetInstanceProcAddr) return 0;
    pfn_vkCreateInstance = (PFN_vkCreateInstance)pfn_vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    vk_loaded = 1;
    return 1;
}

static void load_instance_fns(VkInstance inst) {
#define VK_LOAD(name) pfn_##name = (PFN_##name)pfn_vkGetInstanceProcAddr(inst, #name)
    VK_LOAD(vkDestroyInstance);
    VK_LOAD(vkEnumeratePhysicalDevices);
    VK_LOAD(vkGetPhysicalDeviceProperties);
    VK_LOAD(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_LOAD(vkCreateDevice);
    VK_LOAD(vkDestroyDevice);
    VK_LOAD(vkGetDeviceQueue);
    VK_LOAD(vkCreateCommandPool);
    VK_LOAD(vkAllocateCommandBuffers);
    VK_LOAD(vkBeginCommandBuffer);
    VK_LOAD(vkEndCommandBuffer);
    VK_LOAD(vkQueueSubmit);
    VK_LOAD(vkQueueWaitIdle);
    VK_LOAD(vkDeviceWaitIdle);
    VK_LOAD(vkCmdBeginRenderPass);
    VK_LOAD(vkCmdEndRenderPass);
    VK_LOAD(vkCmdBindPipeline);
    VK_LOAD(vkCmdDraw);
    VK_LOAD(vkDestroyCommandPool);
#undef VK_LOAD
}

/* ---- Lisp wrappers ---- */

static ls_value_t bi_vk_create_instance(ls_state_t *L, int n, ls_value_t *a) {
    if (!load_vk()) { ls_error(L, "cannot load vulkan-1.dll"); return ls_nil_v(); }
    const char *app_name = "Litesrpent";
    if (n >= 1 && a[0].tag == LS_T_STRING) app_name = ((ls_string_t*)a[0].u.ptr)->chars;

    VkApplicationInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = app_name;
    ai.applicationVersion = 1;
    ai.pEngineName = "Litesrpent";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;

    VkInstance instance = NULL;
    VkResult res = pfn_vkCreateInstance(&ci, NULL, &instance);
    if (res != VK_SUCCESS) { ls_error(L, "vkCreateInstance failed: %d", res); return ls_nil_v(); }

    load_instance_fns(instance);

    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ((ls_foreign_t*)v.u.ptr)->ptr = instance;
    return v;
}

static ls_value_t bi_vk_destroy_instance(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 1 && a[0].tag == LS_T_FOREIGN && pfn_vkDestroyInstance) {
        pfn_vkDestroyInstance((VkInstance)((ls_foreign_t*)a[0].u.ptr)->ptr, NULL);
    }
    return ls_nil_v();
}

static ls_value_t bi_vk_enumerate_devices(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || !pfn_vkEnumeratePhysicalDevices) return ls_nil_v();
    VkInstance inst = (VkInstance)((ls_foreign_t*)a[0].u.ptr)->ptr;
    uint32_t count = 0;
    pfn_vkEnumeratePhysicalDevices(inst, &count, NULL);
    if (count == 0) return ls_nil_v();
    VkPhysicalDevice *devs = (VkPhysicalDevice*)calloc(count, sizeof(VkPhysicalDevice));
    pfn_vkEnumeratePhysicalDevices(inst, &count, devs);
    ls_value_t list = ls_nil_v();
    for (uint32_t i = count; i-- > 0;) {
        ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
        ((ls_foreign_t*)v.u.ptr)->ptr = devs[i];
        list = ls_cons(L, v, list);
    }
    free(devs);
    return list;
}

static ls_value_t bi_vk_device_name(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || !pfn_vkGetPhysicalDeviceProperties) return ls_make_string(L, "unknown", 7);
    VkPhysicalDevice dev = (VkPhysicalDevice)((ls_foreign_t*)a[0].u.ptr)->ptr;
    VkPhysicalDeviceProperties props; memset(&props, 0, sizeof props);
    pfn_vkGetPhysicalDeviceProperties(dev, &props);
    return ls_make_string(L, props.deviceName, strlen(props.deviceName));
}

static ls_value_t bi_vk_find_graphics_queue(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || !pfn_vkGetPhysicalDeviceQueueFamilyProperties) return ls_make_fixnum(-1);
    VkPhysicalDevice dev = (VkPhysicalDevice)((ls_foreign_t*)a[0].u.ptr)->ptr;
    uint32_t count = 0;
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, NULL);
    VkQueueFamilyProperties *props = (VkQueueFamilyProperties*)calloc(count, sizeof(VkQueueFamilyProperties));
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, props);
    int idx = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = (int)i; break; }
    }
    free(props);
    return ls_make_fixnum(idx);
}

static ls_value_t bi_vk_create_device(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2 || !pfn_vkCreateDevice) { ls_error(L, "vk-create-device: need physical-device queue-family"); return ls_nil_v(); }
    VkPhysicalDevice pdev = (VkPhysicalDevice)((ls_foreign_t*)a[0].u.ptr)->ptr;
    uint32_t qfamily = (uint32_t)a[1].u.fixnum;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci; memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci; memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = NULL;
    VkResult res = pfn_vkCreateDevice(pdev, &dci, NULL, &device);
    if (res != VK_SUCCESS) { ls_error(L, "vkCreateDevice failed: %d", res); return ls_nil_v(); }
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ((ls_foreign_t*)v.u.ptr)->ptr = device;
    return v;
}

static ls_value_t bi_vk_get_queue(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2 || !pfn_vkGetDeviceQueue) return ls_nil_v();
    VkDevice dev = (VkDevice)((ls_foreign_t*)a[0].u.ptr)->ptr;
    VkQueue queue = NULL;
    pfn_vkGetDeviceQueue(dev, (uint32_t)a[1].u.fixnum, 0, &queue);
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ((ls_foreign_t*)v.u.ptr)->ptr = queue;
    return v;
}

static ls_value_t bi_vk_create_command_pool(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2 || !pfn_vkCreateCommandPool) return ls_nil_v();
    VkDevice dev = (VkDevice)((ls_foreign_t*)a[0].u.ptr)->ptr;
    VkCommandPoolCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = (uint32_t)a[1].u.fixnum;
    ci.flags = 0x2; /* VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT */
    VkCommandPool pool = 0;
    VkResult res = pfn_vkCreateCommandPool(dev, &ci, NULL, &pool);
    if (res != VK_SUCCESS) { ls_error(L, "vkCreateCommandPool failed"); return ls_nil_v(); }
    return ls_make_fixnum((int64_t)pool);
}

static ls_value_t bi_vk_alloc_command_buffer(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2 || !pfn_vkAllocateCommandBuffers) return ls_nil_v();
    VkDevice dev = (VkDevice)((ls_foreign_t*)a[0].u.ptr)->ptr;
    VkCommandBufferAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = (VkCommandPool)a[1].u.fixnum;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = NULL;
    pfn_vkAllocateCommandBuffers(dev, &ai, &cb);
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ((ls_foreign_t*)v.u.ptr)->ptr = cb;
    return v;
}

static ls_value_t bi_vk_begin_command_buffer(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n < 1 || !pfn_vkBeginCommandBuffer) return ls_nil_v();
    VkCommandBuffer cb = (VkCommandBuffer)((ls_foreign_t*)a[0].u.ptr)->ptr;
    VkCommandBufferBeginInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfn_vkBeginCommandBuffer(cb, &bi);
    return ls_nil_v();
}

static ls_value_t bi_vk_end_command_buffer(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 1 && pfn_vkEndCommandBuffer)
        pfn_vkEndCommandBuffer((VkCommandBuffer)((ls_foreign_t*)a[0].u.ptr)->ptr);
    return ls_nil_v();
}

static ls_value_t bi_vk_queue_submit(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2 || !pfn_vkQueueSubmit) return ls_nil_v();
    VkQueue queue = (VkQueue)((ls_foreign_t*)a[0].u.ptr)->ptr;
    VkCommandBuffer cb = (VkCommandBuffer)((ls_foreign_t*)a[1].u.ptr)->ptr;
    VkSubmitInfo si; memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    pfn_vkQueueSubmit(queue, 1, &si, 0);
    return ls_nil_v();
}

static ls_value_t bi_vk_queue_wait_idle(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 1 && pfn_vkQueueWaitIdle)
        pfn_vkQueueWaitIdle((VkQueue)((ls_foreign_t*)a[0].u.ptr)->ptr);
    return ls_nil_v();
}

static ls_value_t bi_vk_cmd_draw(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 3 && pfn_vkCmdDraw) {
        VkCommandBuffer cb = (VkCommandBuffer)((ls_foreign_t*)a[0].u.ptr)->ptr;
        uint32_t vc = (uint32_t)a[1].u.fixnum;
        uint32_t ic = n >= 3 ? (uint32_t)a[2].u.fixnum : 1;
        pfn_vkCmdDraw(cb, vc, ic, 0, 0);
    }
    return ls_nil_v();
}

static ls_value_t bi_vk_destroy_device(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 1 && pfn_vkDestroyDevice)
        pfn_vkDestroyDevice((VkDevice)((ls_foreign_t*)a[0].u.ptr)->ptr, NULL);
    return ls_nil_v();
}

void ls_init_vk(ls_state_t *L) {
    ls_ensure_package(L, "LITESRPENT-VK");
#define VKDEF(n,fn,mi,ma) ls_defun(L,"LITESRPENT-VK",n,fn,mi,ma)
    VKDEF("VK-CREATE-INSTANCE",bi_vk_create_instance,0,1);
    VKDEF("VK-DESTROY-INSTANCE",bi_vk_destroy_instance,1,1);
    VKDEF("VK-ENUMERATE-DEVICES",bi_vk_enumerate_devices,1,1);
    VKDEF("VK-DEVICE-NAME",bi_vk_device_name,1,1);
    VKDEF("VK-FIND-GRAPHICS-QUEUE",bi_vk_find_graphics_queue,1,1);
    VKDEF("VK-CREATE-DEVICE",bi_vk_create_device,2,2);
    VKDEF("VK-GET-QUEUE",bi_vk_get_queue,2,2);
    VKDEF("VK-CREATE-COMMAND-POOL",bi_vk_create_command_pool,2,2);
    VKDEF("VK-ALLOC-COMMAND-BUFFER",bi_vk_alloc_command_buffer,2,2);
    VKDEF("VK-BEGIN-COMMAND-BUFFER",bi_vk_begin_command_buffer,1,1);
    VKDEF("VK-END-COMMAND-BUFFER",bi_vk_end_command_buffer,1,1);
    VKDEF("VK-QUEUE-SUBMIT",bi_vk_queue_submit,2,2);
    VKDEF("VK-QUEUE-WAIT-IDLE",bi_vk_queue_wait_idle,1,1);
    VKDEF("VK-CMD-DRAW",bi_vk_cmd_draw,3,3);
    VKDEF("VK-DESTROY-DEVICE",bi_vk_destroy_device,1,1);
#undef VKDEF
}
