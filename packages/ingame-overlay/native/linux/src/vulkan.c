/*
 * Vulkan layer "VK_LAYER_TOSU_overlay".
 *
 * Draws the overlay onto swapchain images right before vkQueuePresentKHR:
 *   app submit -> [app wait semaphores] -> our upload + draw -> [our semaphore] -> present
 *
 * The layer is enabled with:
 *   VK_ADD_LAYER_PATH=<dir containing tosu_overlay.json>
 *   VK_INSTANCE_LAYERS=VK_LAYER_TOSU_overlay
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "frame.h"
#include "overlay.h"

static const uint32_t vert_spv[] = {
#include "overlay.vert.spv.inc"
};
static const uint32_t frag_spv[] = {
#include "overlay.frag.spv.inc"
};

#define MAX_IMAGES 16
#define MAX_PRESENT 8

typedef struct {
    float rect[4];
    int32_t mode;
    int32_t srgb;
} push_constants_t;

static void *dispatch_key(const void *handle) { return *(void *const *)handle; }

/* ---- instance ---- */

typedef struct instance_data {
    struct instance_data *next;
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
} instance_data_t;

/* ---- device ---- */

#define DEVICE_FUNCS(X)                                                                    \
    X(GetDeviceProcAddr)                                                                   \
    X(DestroyDevice)                                                                       \
    X(GetDeviceQueue)                                                                      \
    X(CreateSwapchainKHR)                                                                  \
    X(DestroySwapchainKHR)                                                                 \
    X(GetSwapchainImagesKHR)                                                               \
    X(QueuePresentKHR)                                                                     \
    X(CreateImageView)                                                                     \
    X(DestroyImageView)                                                                    \
    X(CreateRenderPass)                                                                    \
    X(DestroyRenderPass)                                                                   \
    X(CreateFramebuffer)                                                                   \
    X(DestroyFramebuffer)                                                                  \
    X(CreateShaderModule)                                                                  \
    X(DestroyShaderModule)                                                                 \
    X(CreatePipelineLayout)                                                                \
    X(DestroyPipelineLayout)                                                               \
    X(CreateGraphicsPipelines)                                                             \
    X(DestroyPipeline)                                                                     \
    X(CreateDescriptorSetLayout)                                                           \
    X(DestroyDescriptorSetLayout)                                                          \
    X(CreateDescriptorPool)                                                                \
    X(DestroyDescriptorPool)                                                               \
    X(AllocateDescriptorSets)                                                              \
    X(UpdateDescriptorSets)                                                                \
    X(CreateSampler)                                                                       \
    X(DestroySampler)                                                                      \
    X(CreateImage)                                                                         \
    X(DestroyImage)                                                                        \
    X(CreateBuffer)                                                                        \
    X(DestroyBuffer)                                                                       \
    X(GetImageMemoryRequirements)                                                          \
    X(GetBufferMemoryRequirements)                                                         \
    X(AllocateMemory)                                                                      \
    X(FreeMemory)                                                                          \
    X(BindImageMemory)                                                                     \
    X(BindBufferMemory)                                                                    \
    X(MapMemory)                                                                           \
    X(UnmapMemory)                                                                         \
    X(CreateCommandPool)                                                                   \
    X(DestroyCommandPool)                                                                  \
    X(AllocateCommandBuffers)                                                              \
    X(FreeCommandBuffers)                                                                  \
    X(BeginCommandBuffer)                                                                  \
    X(EndCommandBuffer)                                                                    \
    X(CmdPipelineBarrier)                                                                  \
    X(CmdCopyBufferToImage)                                                                \
    X(CmdBeginRenderPass)                                                                  \
    X(CmdEndRenderPass)                                                                    \
    X(CmdBindPipeline)                                                                     \
    X(CmdBindDescriptorSets)                                                               \
    X(CmdPushConstants)                                                                    \
    X(CmdSetViewport)                                                                      \
    X(CmdSetScissor)                                                                       \
    X(CmdDraw)                                                                             \
    X(CreateFence)                                                                         \
    X(DestroyFence)                                                                        \
    X(WaitForFences)                                                                       \
    X(ResetFences)                                                                         \
    X(GetFenceStatus)                                                                      \
    X(CreateSemaphore)                                                                     \
    X(DestroySemaphore)                                                                    \
    X(QueueSubmit)

typedef struct {
#define X(name) PFN_vk##name name;
    DEVICE_FUNCS(X)
#undef X
    PFN_vkGetDeviceQueue2 GetDeviceQueue2; /* Vulkan 1.1, optional */
} device_dispatch_t;

typedef struct {
    VkQueue queue;
    uint32_t family;
} queue_info_t;

struct swapchain_data;

typedef struct device_data {
    struct device_data *next;
    void *key;
    VkDevice device;
    VkPhysicalDevice physical;
    instance_data_t *instance;
    PFN_vkSetDeviceLoaderData set_loader_data;
    device_dispatch_t vk;
    VkPhysicalDeviceMemoryProperties memory;
    VkQueueFamilyProperties families[16];
    uint32_t family_count;

    queue_info_t queues[32];
    uint32_t queue_count;
    struct swapchain_data *swapchains;

    /* overlay resources shared by all swapchains of this device */
    bool failed;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkShaderModule vert;
    VkShaderModule frag;

    VkImage texture;
    VkDeviceMemory texture_memory;
    VkImageView texture_view;
    uint32_t tex_width;
    uint32_t tex_height;
    bool texture_initialized; /* layout is SHADER_READ_ONLY_OPTIMAL */
    bool has_frame;

    VkBuffer staging;
    VkDeviceMemory staging_memory;
    void *staging_ptr;
    VkFence staging_fence; /* last submission reading the staging buffer */

    uint32_t last_seq;
} device_data_t;

typedef struct swapchain_data {
    struct swapchain_data *next;
    device_data_t *device;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];

    /* created on first present */
    bool ready;
    bool failed;
    uint32_t family;
    VkRenderPass render_pass;
    VkPipeline pipeline;
    VkCommandPool pool;
    VkImageView views[MAX_IMAGES];
    VkFramebuffer framebuffers[MAX_IMAGES];
    VkCommandBuffer cmds[MAX_IMAGES];
    VkFence fences[MAX_IMAGES];
    VkSemaphore semaphores[MAX_IMAGES];
    bool submitted[MAX_IMAGES];
} swapchain_data_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static instance_data_t *instances = NULL;
static device_data_t *devices = NULL;

static instance_data_t *find_instance(const void *handle) {
    void *key = dispatch_key(handle);
    for (instance_data_t *it = instances; it; it = it->next) {
        if (it->key == key) {
            return it;
        }
    }
    return NULL;
}

static device_data_t *find_device(const void *handle) {
    void *key = dispatch_key(handle);
    for (device_data_t *it = devices; it; it = it->next) {
        if (it->key == key) {
            return it;
        }
    }
    return NULL;
}

static swapchain_data_t *find_swapchain(device_data_t *dev, VkSwapchainKHR swapchain) {
    for (swapchain_data_t *it = dev->swapchains; it; it = it->next) {
        if (it->swapchain == swapchain) {
            return it;
        }
    }
    return NULL;
}

/* ---- instance hooks ---- */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *info,
                                                           const VkAllocationCallbacks *alloc,
                                                           VkInstance *out) {
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)info->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                      chain->function == VK_LAYER_LINK_INFO)) {
        chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    }
    if (!chain || !chain->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult result = create(info, alloc, out);
    if (result != VK_SUCCESS) {
        return result;
    }

    instance_data_t *data = calloc(1, sizeof(*data));
    data->instance = *out;
    data->key = dispatch_key(*out);
    data->GetInstanceProcAddr = next_gipa;
    data->DestroyInstance = (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
    data->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)next_gipa(
        *out, "vkGetPhysicalDeviceMemoryProperties");
    data->GetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)next_gipa(
            *out, "vkGetPhysicalDeviceQueueFamilyProperties");

    pthread_mutex_lock(&lock);
    data->next = instances;
    instances = data;
    pthread_mutex_unlock(&lock);

    tosu_log("vulkan: layer active");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance instance,
                                                        const VkAllocationCallbacks *alloc) {
    pthread_mutex_lock(&lock);
    instance_data_t **link = &instances;
    instance_data_t *data = NULL;
    void *key = dispatch_key(instance);
    while (*link) {
        if ((*link)->key == key) {
            data = *link;
            *link = data->next;
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&lock);

    if (data) {
        data->DestroyInstance(instance, alloc);
        free(data);
    }
}

/* ---- device hooks ---- */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice physical,
                                                         const VkDeviceCreateInfo *info,
                                                         const VkAllocationCallbacks *alloc,
                                                         VkDevice *out) {
    VkLayerDeviceCreateInfo *link_info = NULL;
    VkLayerDeviceCreateInfo *loader_data = NULL;
    for (VkLayerDeviceCreateInfo *it = (VkLayerDeviceCreateInfo *)info->pNext; it;
         it = (VkLayerDeviceCreateInfo *)it->pNext) {
        if (it->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            continue;
        }
        if (it->function == VK_LAYER_LINK_INFO && !link_info) {
            link_info = it;
        } else if (it->function == VK_LOADER_DATA_CALLBACK && !loader_data) {
            loader_data = it;
        }
    }
    if (!link_info || !link_info->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link_info->u.pLayerInfo = link_info->u.pLayerInfo->pNext;

    pthread_mutex_lock(&lock);
    instance_data_t *inst = find_instance(physical);
    pthread_mutex_unlock(&lock);
    if (!inst) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkCreateDevice create =
        (PFN_vkCreateDevice)next_gipa(inst->instance, "vkCreateDevice");
    VkResult result = create(physical, info, alloc, out);
    if (result != VK_SUCCESS) {
        return result;
    }

    device_data_t *dev = calloc(1, sizeof(*dev));
    dev->device = *out;
    dev->key = dispatch_key(*out);
    dev->physical = physical;
    dev->instance = inst;
    dev->set_loader_data = loader_data ? loader_data->u.pfnSetDeviceLoaderData : NULL;
    dev->last_seq = 0;

#define X(name)                                                                            \
    dev->vk.name = (PFN_vk##name)next_gdpa(*out, "vk" #name);                              \
    if (!dev->vk.name) {                                                                   \
        tosu_debug("vulkan: device is missing vk" #name ", overlay disabled");            \
        dev->failed = true;                                                                \
    }
    DEVICE_FUNCS(X)
#undef X
    dev->vk.GetDeviceQueue2 = (PFN_vkGetDeviceQueue2)next_gdpa(*out, "vkGetDeviceQueue2");
    if (!dev->vk.GetDeviceProcAddr) {
        dev->vk.GetDeviceProcAddr = next_gdpa;
    }
    if (!dev->set_loader_data) {
        dev->failed = true;
    }

    inst->GetPhysicalDeviceMemoryProperties(physical, &dev->memory);
    dev->family_count = 16;
    inst->GetPhysicalDeviceQueueFamilyProperties(physical, &dev->family_count, dev->families);

    pthread_mutex_lock(&lock);
    dev->next = devices;
    devices = dev;
    pthread_mutex_unlock(&lock);

    return VK_SUCCESS;
}

/*
 * Never block the game indefinitely on our own work: if a fence does not signal
 * within the timeout the overlay is disabled for this device instead.
 */
#define FENCE_TIMEOUT_NS 2000000000ull

static bool wait_fence(device_data_t *dev, VkFence fence) {
    VkResult result = dev->vk.WaitForFences(dev->device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
    if (result != VK_SUCCESS) {
        tosu_log("vulkan: overlay fence wait failed (%d), overlay disabled", result);
        dev->failed = true;
        return false;
    }
    return true;
}

static bool wait_all(device_data_t *dev) {
    for (swapchain_data_t *sc = dev->swapchains; sc; sc = sc->next) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            if (sc->submitted[i]) {
                if (!wait_fence(dev, sc->fences[i])) return false;
                sc->submitted[i] = false;
            }
        }
    }
    /* every submission finished, nothing reads the staging buffer anymore */
    dev->staging_fence = VK_NULL_HANDLE;
    return true;
}

static void destroy_texture(device_data_t *dev) {
    VkDevice d = dev->device;
    if (dev->texture_view) dev->vk.DestroyImageView(d, dev->texture_view, NULL);
    if (dev->texture) dev->vk.DestroyImage(d, dev->texture, NULL);
    if (dev->texture_memory) dev->vk.FreeMemory(d, dev->texture_memory, NULL);
    if (dev->staging) dev->vk.DestroyBuffer(d, dev->staging, NULL);
    if (dev->staging_memory) dev->vk.FreeMemory(d, dev->staging_memory, NULL);
    dev->texture_view = VK_NULL_HANDLE;
    dev->texture = VK_NULL_HANDLE;
    dev->texture_memory = VK_NULL_HANDLE;
    dev->staging = VK_NULL_HANDLE;
    dev->staging_memory = VK_NULL_HANDLE;
    dev->staging_ptr = NULL;
    dev->staging_fence = VK_NULL_HANDLE;
    dev->tex_width = dev->tex_height = 0;
    dev->texture_initialized = false;
    dev->has_frame = false;
}

static void destroy_swapchain_resources(swapchain_data_t *sc) {
    device_data_t *dev = sc->device;
    VkDevice d = dev->device;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->submitted[i]) {
            dev->vk.WaitForFences(d, 1, &sc->fences[i], VK_TRUE, FENCE_TIMEOUT_NS);
            sc->submitted[i] = false;
        }
        if (dev->staging_fence && dev->staging_fence == sc->fences[i]) {
            dev->staging_fence = VK_NULL_HANDLE;
        }
        if (sc->framebuffers[i]) dev->vk.DestroyFramebuffer(d, sc->framebuffers[i], NULL);
        if (sc->views[i]) dev->vk.DestroyImageView(d, sc->views[i], NULL);
        if (sc->fences[i]) dev->vk.DestroyFence(d, sc->fences[i], NULL);
        if (sc->semaphores[i]) dev->vk.DestroySemaphore(d, sc->semaphores[i], NULL);
    }
    if (sc->pool) dev->vk.DestroyCommandPool(d, sc->pool, NULL);
    if (sc->pipeline) dev->vk.DestroyPipeline(d, sc->pipeline, NULL);
    if (sc->render_pass) dev->vk.DestroyRenderPass(d, sc->render_pass, NULL);
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice device,
                                                      const VkAllocationCallbacks *alloc) {
    pthread_mutex_lock(&lock);
    device_data_t **link = &devices;
    device_data_t *dev = NULL;
    void *key = dispatch_key(device);
    while (*link) {
        if ((*link)->key == key) {
            dev = *link;
            *link = dev->next;
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&lock);

    if (!dev) {
        return;
    }

    PFN_vkDestroyDevice destroy = dev->vk.DestroyDevice;
    if (!dev->failed || dev->vk.DestroyImage) {
        swapchain_data_t *sc = dev->swapchains;
        while (sc) {
            swapchain_data_t *next = sc->next;
            destroy_swapchain_resources(sc);
            free(sc);
            sc = next;
        }
        if (dev->sampler) {
            destroy_texture(dev);
            dev->vk.DestroySampler(device, dev->sampler, NULL);
            dev->vk.DestroyDescriptorPool(device, dev->descriptor_pool, NULL);
            dev->vk.DestroyDescriptorSetLayout(device, dev->set_layout, NULL);
            dev->vk.DestroyPipelineLayout(device, dev->pipeline_layout, NULL);
            dev->vk.DestroyShaderModule(device, dev->vert, NULL);
            dev->vk.DestroyShaderModule(device, dev->frag, NULL);
        }
    }
    free(dev);
    destroy(device, alloc);
}

static void record_queue(device_data_t *dev, VkQueue queue, uint32_t family) {
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < dev->queue_count; i++) {
        if (dev->queues[i].queue == queue) {
            pthread_mutex_unlock(&lock);
            return;
        }
    }
    if (dev->queue_count < 32) {
        dev->queues[dev->queue_count++] = (queue_info_t){queue, family};
    }
    pthread_mutex_unlock(&lock);
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceQueue(VkDevice device, uint32_t family,
                                                       uint32_t index, VkQueue *out) {
    pthread_mutex_lock(&lock);
    device_data_t *dev = find_device(device);
    pthread_mutex_unlock(&lock);
    dev->vk.GetDeviceQueue(device, family, index, out);
    if (*out) {
        record_queue(dev, *out, family);
    }
}

static VKAPI_ATTR void VKAPI_CALL layer_GetDeviceQueue2(VkDevice device,
                                                        const VkDeviceQueueInfo2 *info,
                                                        VkQueue *out) {
    pthread_mutex_lock(&lock);
    device_data_t *dev = find_device(device);
    pthread_mutex_unlock(&lock);
    dev->vk.GetDeviceQueue2(device, info, out);
    if (*out) {
        record_queue(dev, *out, info->queueFamilyIndex);
    }
}

/* ---- swapchain hooks ---- */

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *alloc,
    VkSwapchainKHR *out) {
    pthread_mutex_lock(&lock);
    device_data_t *dev = find_device(device);
    pthread_mutex_unlock(&lock);

    /* we render into swapchain images, COLOR_ATTACHMENT is always supported */
    VkSwapchainCreateInfoKHR patched = *info;
    patched.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkResult result = dev->vk.CreateSwapchainKHR(device, &patched, alloc, out);
    if (result != VK_SUCCESS || dev->failed) {
        return result;
    }

    swapchain_data_t *sc = calloc(1, sizeof(*sc));
    sc->device = dev;
    sc->swapchain = *out;
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;

    uint32_t count = 0;
    dev->vk.GetSwapchainImagesKHR(device, *out, &count, NULL);
    if (count > MAX_IMAGES) {
        tosu_log("vulkan: too many swapchain images (%u), overlay disabled", count);
        sc->failed = true;
        count = 0;
    }
    sc->image_count = count;
    if (count) {
        dev->vk.GetSwapchainImagesKHR(device, *out, &sc->image_count, sc->images);
    }

    pthread_mutex_lock(&lock);
    sc->next = dev->swapchains;
    dev->swapchains = sc;
    pthread_mutex_unlock(&lock);

    tosu_debug("vulkan: swapchain %ux%u format %d, %u images", sc->extent.width,
               sc->extent.height, sc->format, sc->image_count);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroySwapchainKHR(VkDevice device,
                                                            VkSwapchainKHR swapchain,
                                                            const VkAllocationCallbacks *alloc) {
    pthread_mutex_lock(&lock);
    device_data_t *dev = find_device(device);
    swapchain_data_t *sc = NULL;
    for (swapchain_data_t **link = &dev->swapchains; *link; link = &(*link)->next) {
        if ((*link)->swapchain == swapchain) {
            sc = *link;
            *link = sc->next;
            break;
        }
    }
    pthread_mutex_unlock(&lock);

    if (sc) {
        destroy_swapchain_resources(sc);
        free(sc);
    }
    dev->vk.DestroySwapchainKHR(device, swapchain, alloc);
}

/* ---- overlay resources ---- */

static int32_t find_memory(device_data_t *dev, uint32_t type_bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < dev->memory.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (dev->memory.memoryTypes[i].propertyFlags & flags) == flags) {
            return (int32_t)i;
        }
    }
    return -1;
}

static bool init_device_resources(device_data_t *dev) {
    VkDevice d = dev->device;

    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    if (dev->vk.CreateSampler(d, &sampler, NULL, &dev->sampler) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &dev->sampler,
    };
    VkDescriptorSetLayoutCreateInfo set_layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (dev->vk.CreateDescriptorSetLayout(d, &set_layout, NULL, &dev->set_layout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    if (dev->vk.CreateDescriptorPool(d, &pool, NULL, &dev->descriptor_pool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dev->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dev->set_layout,
    };
    if (dev->vk.AllocateDescriptorSets(d, &alloc, &dev->descriptor_set) != VK_SUCCESS)
        return false;

    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(push_constants_t),
    };
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dev->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };
    if (dev->vk.CreatePipelineLayout(d, &layout, NULL, &dev->pipeline_layout) != VK_SUCCESS)
        return false;

    VkShaderModuleCreateInfo vert = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(vert_spv),
        .pCode = vert_spv,
    };
    VkShaderModuleCreateInfo frag = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(frag_spv),
        .pCode = frag_spv,
    };
    if (dev->vk.CreateShaderModule(d, &vert, NULL, &dev->vert) != VK_SUCCESS) return false;
    if (dev->vk.CreateShaderModule(d, &frag, NULL, &dev->frag) != VK_SUCCESS) return false;

    return true;
}

/* (Re)create the overlay texture and its staging buffer. Caller waited for our work. */
static bool create_texture(device_data_t *dev, uint32_t width, uint32_t height) {
    VkDevice d = dev->device;
    destroy_texture(dev);

    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (dev->vk.CreateImage(d, &image, NULL, &dev->texture) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    dev->vk.GetImageMemoryRequirements(d, dev->texture, &req);
    int32_t type = find_memory(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(dev, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    if (type < 0 || dev->vk.AllocateMemory(d, &alloc, NULL, &dev->texture_memory) != VK_SUCCESS)
        return false;
    if (dev->vk.BindImageMemory(d, dev->texture, dev->texture_memory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = dev->texture,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (dev->vk.CreateImageView(d, &view, NULL, &dev->texture_view) != VK_SUCCESS) return false;

    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)width * height * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (dev->vk.CreateBuffer(d, &buffer, NULL, &dev->staging) != VK_SUCCESS) return false;

    dev->vk.GetBufferMemoryRequirements(d, dev->staging, &req);
    type = find_memory(dev, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || dev->vk.AllocateMemory(d, &alloc, NULL, &dev->staging_memory) != VK_SUCCESS)
        return false;
    if (dev->vk.BindBufferMemory(d, dev->staging, dev->staging_memory, 0) != VK_SUCCESS)
        return false;
    if (dev->vk.MapMemory(d, dev->staging_memory, 0, VK_WHOLE_SIZE, 0, &dev->staging_ptr) !=
        VK_SUCCESS)
        return false;

    VkDescriptorImageInfo image_info = {
        .sampler = dev->sampler,
        .imageView = dev->texture_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dev->descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
    };
    dev->vk.UpdateDescriptorSets(d, 1, &write, 0, NULL);

    dev->tex_width = width;
    dev->tex_height = height;
    return true;
}

static bool is_srgb(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

static bool init_swapchain_resources(swapchain_data_t *sc, uint32_t family) {
    device_data_t *dev = sc->device;
    VkDevice d = dev->device;
    sc->family = family;

    VkAttachmentDescription attachment = {
        .format = sc->format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkSubpassDependency deps[2] = {
        {
            .srcSubpass = VK_SUBPASS_EXTERNAL,
            .dstSubpass = 0,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        },
        {
            .srcSubpass = 0,
            .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = 0,
        },
    };
    VkRenderPassCreateInfo render_pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 2,
        .pDependencies = deps,
    };
    if (dev->vk.CreateRenderPass(d, &render_pass, NULL, &sc->render_pass) != VK_SUCCESS)
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = dev->vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = dev->frag,
            .pName = "main",
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = dev->pipeline_layout,
        .renderPass = sc->render_pass,
        .subpass = 0,
    };
    if (dev->vk.CreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &pipeline, NULL, &sc->pipeline) !=
        VK_SUCCESS)
        return false;

    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (dev->vk.CreateCommandPool(d, &pool, NULL, &sc->pool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo cmd_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = sc->image_count,
    };
    if (dev->vk.AllocateCommandBuffers(d, &cmd_alloc, sc->cmds) != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < sc->image_count; i++) {
        /* command buffers made by a layer need the loader's dispatch pointer */
        if (dev->set_loader_data(d, sc->cmds[i]) != VK_SUCCESS) return false;

        VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = sc->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = sc->format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (dev->vk.CreateImageView(d, &view, NULL, &sc->views[i]) != VK_SUCCESS) return false;

        VkFramebufferCreateInfo framebuffer = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = sc->render_pass,
            .attachmentCount = 1,
            .pAttachments = &sc->views[i],
            .width = sc->extent.width,
            .height = sc->extent.height,
            .layers = 1,
        };
        if (dev->vk.CreateFramebuffer(d, &framebuffer, NULL, &sc->framebuffers[i]) != VK_SUCCESS)
            return false;

        VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (dev->vk.CreateFence(d, &fence, NULL, &sc->fences[i]) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (dev->vk.CreateSemaphore(d, &semaphore, NULL, &sc->semaphores[i]) != VK_SUCCESS)
            return false;
    }

    return true;
}

static void image_barrier(device_data_t *dev, VkCommandBuffer cmd, VkImage image,
                          VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    dev->vk.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/*
 * Record and submit overlay work for one swapchain image.
 * Returns the semaphore present must wait on, or VK_NULL_HANDLE if nothing was submitted.
 */
static VkSemaphore draw_overlay(VkQueue queue, swapchain_data_t *sc, uint32_t index,
                                const VkSemaphore *waits, uint32_t wait_count, bool has_new,
                                const frame_view_t *frame, const cursor_state_t *cursor,
                                bool visible) {
    device_data_t *dev = sc->device;
    VkDevice d = dev->device;

    if (sc->submitted[index]) {
        if (!wait_fence(dev, sc->fences[index])) {
            return VK_NULL_HANDLE;
        }
        sc->submitted[index] = false;
    }
    /* the fence is about to be reset; if it guarded the staging buffer, that
     * work has just been waited for, so stop tracking it (waiting on a reset,
     * never-submitted fence would hang forever) */
    if (dev->staging_fence == sc->fences[index]) {
        dev->staging_fence = VK_NULL_HANDLE;
    }
    dev->vk.ResetFences(d, 1, &sc->fences[index]);

    bool upload = false;
    if (has_new) {
        if (frame->width != dev->tex_width || frame->height != dev->tex_height) {
            if (!wait_all(dev)) {
                return VK_NULL_HANDLE;
            }
            if (!create_texture(dev, frame->width, frame->height)) {
                tosu_log("vulkan: failed to create overlay texture");
                destroy_texture(dev);
                return VK_NULL_HANDLE;
            }
        } else if (dev->staging_fence) {
            if (!wait_fence(dev, dev->staging_fence)) {
                return VK_NULL_HANDLE;
            }
            dev->staging_fence = VK_NULL_HANDLE;
        }
        memcpy(dev->staging_ptr, frame->pixels, (size_t)frame->width * frame->height * 4);
        upload = true;
    }

    bool draw = visible && (dev->has_frame || upload || cursor->active);
    if (!upload && !draw) {
        return VK_NULL_HANDLE;
    }

    VkCommandBuffer cmd = sc->cmds[index];
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    dev->vk.BeginCommandBuffer(cmd, &begin);

    if (upload) {
        image_barrier(dev, cmd, dev->texture,
                      dev->texture_initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                               : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {frame->width, frame->height, 1},
        };
        dev->vk.CmdCopyBufferToImage(cmd, dev->staging, dev->texture,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(dev, cmd, dev->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        dev->texture_initialized = true;
        dev->has_frame = true;
        dev->staging_fence = sc->fences[index];
    }

    if (draw) {
        VkRenderPassBeginInfo pass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = sc->render_pass,
            .framebuffer = sc->framebuffers[index],
            .renderArea = {{0, 0}, sc->extent},
        };
        dev->vk.CmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        dev->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sc->pipeline);

        VkViewport viewport = {0, 0, (float)sc->extent.width, (float)sc->extent.height, 0, 1};
        VkRect2D scissor = {{0, 0}, sc->extent};
        dev->vk.CmdSetViewport(cmd, 0, 1, &viewport);
        dev->vk.CmdSetScissor(cmd, 0, 1, &scissor);

        push_constants_t pc = {.srgb = is_srgb(sc->format)};
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        if (dev->has_frame) {
            dev->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          dev->pipeline_layout, 0, 1, &dev->descriptor_set, 0,
                                          NULL);
            pc.mode = 0;
            pc.rect[0] = -1.0f;
            pc.rect[1] = -1.0f;
            pc.rect[2] = 1.0f;
            pc.rect[3] = 1.0f;
            dev->vk.CmdPushConstants(cmd, dev->pipeline_layout, stages, 0, sizeof(pc), &pc);
            dev->vk.CmdDraw(cmd, 4, 1, 0, 0);
        }

        if (cursor->active && dev->has_frame) {
            float w = (float)sc->extent.width, h = (float)sc->extent.height, r = 10.0f;
            pc.mode = 1;
            pc.rect[0] = (cursor->x - r) / w * 2.0f - 1.0f;
            pc.rect[1] = (cursor->y - r) / h * 2.0f - 1.0f;
            pc.rect[2] = (cursor->x + r) / w * 2.0f - 1.0f;
            pc.rect[3] = (cursor->y + r) / h * 2.0f - 1.0f;
            dev->vk.CmdPushConstants(cmd, dev->pipeline_layout, stages, 0, sizeof(pc), &pc);
            dev->vk.CmdDraw(cmd, 4, 1, 0, 0);
        }

        dev->vk.CmdEndRenderPass(cmd);
    }

    dev->vk.EndCommandBuffer(cmd);

    VkPipelineStageFlags stack_stages[32];
    VkPipelineStageFlags *wait_stages =
        wait_count <= 32 ? stack_stages : calloc(wait_count, sizeof(*wait_stages));
    for (uint32_t i = 0; i < wait_count; i++) {
        wait_stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait_count,
        .pWaitSemaphores = waits,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &sc->semaphores[index],
    };
    VkResult result = dev->vk.QueueSubmit(queue, 1, &submit, sc->fences[index]);
    if (wait_stages != stack_stages) {
        free(wait_stages);
    }
    if (result != VK_SUCCESS) {
        tosu_log("vulkan: overlay submit failed (%d)", result);
        return VK_NULL_HANDLE;
    }
    sc->submitted[index] = true;
    return sc->semaphores[index];
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_QueuePresentKHR(VkQueue queue,
                                                            const VkPresentInfoKHR *info) {
    pthread_mutex_lock(&lock);
    device_data_t *dev = find_device(queue);
    uint32_t family = UINT32_MAX;
    if (dev) {
        for (uint32_t i = 0; i < dev->queue_count; i++) {
            if (dev->queues[i].queue == queue) {
                family = dev->queues[i].family;
            }
        }
    }
    pthread_mutex_unlock(&lock);

    PFN_vkQueuePresentKHR present = dev->vk.QueuePresentKHR;

    channel_tick();
    if (dev->failed || !channel_ready() || family == UINT32_MAX ||
        family >= dev->family_count ||
        !(dev->families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
        info->swapchainCount > MAX_PRESENT) {
        return present(queue, info);
    }

    /* everything below runs on the presenting thread; the app externally
     * synchronizes the queue for us, swapchain data is only touched here and
     * in create/destroy, which the app may not race with present */
    if (!dev->sampler) {
        if (!init_device_resources(dev)) {
            tosu_log("vulkan: failed to create overlay resources, overlay disabled");
            dev->failed = true;
            return present(queue, info);
        }
    }

    cursor_state_t cursor;
    input_cursor(&cursor);
    bool visible = frame_visible();

    frame_view_t frame;
    bool has_new = false;
    bool size_reported = false;

    VkSemaphore semaphores[MAX_PRESENT];
    uint32_t semaphore_count = 0;
    bool waits_consumed = false;

    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        swapchain_data_t *sc = find_swapchain(dev, info->pSwapchains[i]);
        if (!sc || sc->failed) {
            continue;
        }
        if (!sc->ready || sc->family != family) {
            if (sc->ready) {
                destroy_swapchain_resources(sc);
                memset(&sc->render_pass, 0,
                       sizeof(*sc) - offsetof(swapchain_data_t, render_pass));
            }
            if (!init_swapchain_resources(sc, family)) {
                tosu_log("vulkan: failed to create swapchain resources, overlay disabled");
                destroy_swapchain_resources(sc);
                sc->failed = true;
                continue;
            }
            sc->ready = true;
        }

        if (!size_reported) {
            /* the host renders at the size of the first presented swapchain */
            frame_report_size((int)sc->extent.width, (int)sc->extent.height);
            has_new = frame_acquire(&dev->last_seq, &frame);
            size_reported = true;
        }

        VkSemaphore signal =
            draw_overlay(queue, sc, info->pImageIndices[i],
                         waits_consumed ? NULL : info->pWaitSemaphores,
                         waits_consumed ? 0 : info->waitSemaphoreCount, has_new, &frame,
                         &cursor, visible);
        if (signal) {
            semaphores[semaphore_count++] = signal;
            waits_consumed = true;
            has_new = false; /* uploaded once, other swapchains reuse the texture */
        }
    }

    if (semaphore_count == 0) {
        return present(queue, info);
    }

    VkPresentInfoKHR patched = *info;
    patched.waitSemaphoreCount = semaphore_count;
    patched.pWaitSemaphores = semaphores;
    return present(queue, &patched);
}

/* ---- dispatch ---- */

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device,
                                                                       const char *name);

static PFN_vkVoidFunction intercept_device(const char *name) {
#define HOOK(fn)                                                                           \
    if (strcmp(name, "vk" #fn) == 0) return (PFN_vkVoidFunction)layer_##fn;
    HOOK(GetDeviceProcAddr)
    HOOK(DestroyDevice)
    HOOK(GetDeviceQueue)
    HOOK(GetDeviceQueue2)
    HOOK(CreateSwapchainKHR)
    HOOK(DestroySwapchainKHR)
    HOOK(QueuePresentKHR)
#undef HOOK
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice device,
                                                                       const char *name) {
    pthread_mutex_lock(&lock);
    device_data_t *dev = find_device(device);
    pthread_mutex_unlock(&lock);
    if (!dev) {
        return NULL;
    }

    PFN_vkVoidFunction hooked = intercept_device(name);
    if (hooked && !(hooked == (PFN_vkVoidFunction)layer_GetDeviceQueue2 &&
                    !dev->vk.GetDeviceQueue2)) {
        return hooked;
    }
    return dev->vk.GetDeviceProcAddr(device, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance,
                                                                         const char *name);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance instance,
                                                                         const char *name) {
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)layer_GetInstanceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)layer_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)layer_DestroyInstance;
    if (strcmp(name, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)layer_CreateDevice;

    PFN_vkVoidFunction hooked = intercept_device(name);
    if (hooked) {
        return hooked;
    }

    if (!instance) {
        return NULL;
    }

    pthread_mutex_lock(&lock);
    instance_data_t *inst = find_instance(instance);
    pthread_mutex_unlock(&lock);
    return inst ? inst->GetInstanceProcAddr(instance, name) : NULL;
}

/* Exported under a unique name (see tosu_overlay.json) so the LD_PRELOADed copy of this
 * library never shadows another layer's or the loader's symbols. */
__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
tosu_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (version->loaderLayerInterfaceVersion >= 2) {
        version->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
        version->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
        version->pfnGetPhysicalDeviceProcAddr = NULL;
        version->loaderLayerInterfaceVersion = 2;
    }
    return VK_SUCCESS;
}
