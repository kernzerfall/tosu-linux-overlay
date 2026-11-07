/*
 * Minimal stand-in for osu!lazer: an SDL3 window rendered with OpenGL or Vulkan,
 * with every graphics entry point resolved at runtime (like SDL3 + .NET do).
 *
 *   overlay_test gl|vk [seconds]
 *
 * Fills the window with a dark blue background, logs key/mouse events it
 * receives (to check input capture), and exits after the given time.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

static void log_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        printf("game: key %s scancode=%d\n", e->key.down ? "down" : "up", e->key.scancode);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        printf("game: button %s %d\n", e->button.down ? "down" : "up", e->button.button);
        break;
    default:
        break;
    }
    fflush(stdout);
}

/* Mirrors osu!framework's SDL3Window.HandleEventFromFilter: mouse events, and
 * keyboard events while no text input is active, are handled in the event
 * filter and dropped from the queue. */
static SDL_Window *main_window;

static bool event_filter(void *userdata, SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!SDL_TextInputActive(main_window)) {
            printf("game(filter): ");
            log_event(e);
            return false;
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        printf("game(filter): ");
        log_event(e);
        return false;
    default:
        return true;
    }
}

/* .NET resolves every SDL export with dlsym; do the same so the hooks apply */
static int (*p_PeepEvents)(SDL_Event *, int, SDL_EventAction, Uint32, Uint32);
static bool (*p_PollEvent)(SDL_Event *);
static void (*p_SetEventFilter)(SDL_EventFilter, void *);

static void resolve_sdl(void) {
    void *sdl = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD);
    p_PeepEvents = dlsym(sdl, "SDL_PeepEvents");
    p_PollEvent = dlsym(sdl, "SDL_PollEvent");
    p_SetEventFilter = dlsym(sdl, "SDL_SetEventFilter");
}

static bool pump(void) {
    SDL_Event e;
    SDL_PumpEvents();
    /* osu!framework drains the queue with SDL_PeepEvents; exercise both paths */
    while (p_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
        if (e.type == SDL_EVENT_QUIT) return false;
        log_event(&e);
    }
    while (p_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return false;
        log_event(&e);
    }
    return true;
}

/* ---------------- OpenGL ---------------- */

typedef void (*gl_clear_color_fn)(float, float, float, float);
typedef void (*gl_clear_fn)(unsigned int);

static int run_gl(SDL_Window *window, Uint64 deadline) {
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        fprintf(stderr, "GL context: %s\n", SDL_GetError());
        return 1;
    }
    gl_clear_color_fn clear_color = (gl_clear_color_fn)SDL_GL_GetProcAddress("glClearColor");
    gl_clear_fn clear = (gl_clear_fn)SDL_GL_GetProcAddress("glClear");
    SDL_GL_SetSwapInterval(1);

    while (SDL_GetTicks() < deadline && pump()) {
        clear_color(0.05f, 0.1f, 0.3f, 1.0f);
        clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DestroyContext(ctx);
    return 0;
}

/* ---------------- Vulkan ---------------- */

#define VK_CHECK(x)                                                                        \
    do {                                                                                   \
        VkResult r_ = (x);                                                                 \
        if (r_ != VK_SUCCESS) {                                                            \
            fprintf(stderr, "%s failed: %d\n", #x, r_);                                    \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

static int run_vk(SDL_Window *window, Uint64 deadline) {
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
#define IFN(name) PFN_vk##name vk##name = (PFN_vk##name)gipa(instance, "vk" #name)
    VkInstance instance = VK_NULL_HANDLE;
    IFN(CreateInstance);

    Uint32 ext_count = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app,
                                .enabledExtensionCount = ext_count,
                                .ppEnabledExtensionNames = exts};
    VK_CHECK(vkCreateInstance(&ici, NULL, &instance));

    IFN(EnumeratePhysicalDevices);
    IFN(GetPhysicalDeviceQueueFamilyProperties);
    IFN(GetPhysicalDeviceSurfaceSupportKHR);
    IFN(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(GetPhysicalDeviceSurfaceFormatsKHR);
    IFN(CreateDevice);
    IFN(GetDeviceProcAddr);
    IFN(DestroyInstance);
    IFN(DestroySurfaceKHR);

    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        fprintf(stderr, "surface: %s\n", SDL_GetError());
        return 1;
    }

    uint32_t pd_count = 8;
    VkPhysicalDevice pds[8];
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &pd_count, pds));
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t family = 0;
    for (uint32_t i = 0; i < pd_count && !pd; i++) {
        uint32_t qf_count = 16;
        VkQueueFamilyProperties qfs[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qf_count, qfs);
        for (uint32_t f = 0; f < qf_count; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], f, surface, &present);
            if ((qfs[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                pd = pds[i];
                family = f;
                break;
            }
        }
    }
    if (!pd) {
        fprintf(stderr, "no suitable device\n");
        return 1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = family,
                                   .queueCount = 1,
                                   .pQueuePriorities = &prio};
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = 1,
                              .ppEnabledExtensionNames = dev_exts};
    VkDevice device;
    VK_CHECK(vkCreateDevice(pd, &dci, NULL, &device));

#define DFN(name) PFN_vk##name vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name)
    DFN(GetDeviceQueue);
    DFN(CreateSwapchainKHR);
    DFN(DestroySwapchainKHR);
    DFN(GetSwapchainImagesKHR);
    DFN(AcquireNextImageKHR);
    DFN(QueuePresentKHR);
    DFN(CreateCommandPool);
    DFN(DestroyCommandPool);
    DFN(AllocateCommandBuffers);
    DFN(BeginCommandBuffer);
    DFN(EndCommandBuffer);
    DFN(CmdPipelineBarrier);
    DFN(CmdClearColorImage);
    DFN(QueueSubmit);
    DFN(CreateSemaphore);
    DFN(DestroySemaphore);
    DFN(CreateFence);
    DFN(DestroyFence);
    DFN(WaitForFences);
    DFN(ResetFences);
    DFN(DeviceWaitIdle);
    DFN(DestroyDevice);

    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
    uint32_t fmt_count = 128;
    VkSurfaceFormatKHR fmts[128];
    VkResult fr = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fmt_count, fmts);
    if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) {
        fprintf(stderr, "surface formats: %d\n", fr);
        return 1;
    }
    VkSurfaceFormatKHR fmt = fmts[0];
    const char *want = getenv("TEST_SRGB");
    for (uint32_t i = 0; i < fmt_count; i++) {
        VkFormat f = want ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        if (fmts[i].format == f && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fmt = fmts[i];
    }

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t image_count = 0;
    VkImage images[8];
    VkSemaphore rendered[8];
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

recreate:
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    printf("vk: swapchain format %d %ux%u\n", fmt.format, extent.width, extent.height);
    fflush(stdout);

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = caps.minImageCount + 1,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        /* only TRANSFER_DST: the layer must add COLOR_ATTACHMENT itself */
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
        .oldSwapchain = swapchain,
    };
    VkSwapchainKHR old = swapchain;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &swapchain));
    if (old) {
        vkDestroySwapchainKHR(device, old, NULL);
        for (uint32_t i = 0; i < image_count; i++) vkDestroySemaphore(device, rendered[i], NULL);
    }

    image_count = 8;
    VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, images));
    for (uint32_t i = 0; i < image_count; i++) {
        VK_CHECK(vkCreateSemaphore(device, &semi, NULL, &rendered[i]));
    }
    if (old) goto frames;

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = family};
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));

    enum { FRAMES = 2 };
    VkCommandBuffer cmds[FRAMES];
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = FRAMES};
    VK_CHECK(vkAllocateCommandBuffers(device, &cai, cmds));

    VkSemaphore acquired[FRAMES];
    VkFence fences[FRAMES];
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                            .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(device, &semi, NULL, &acquired[i]));
        VK_CHECK(vkCreateFence(device, &fi, NULL, &fences[i]));
    }
    uint32_t frame = 0;
frames:
    while (SDL_GetTicks() < deadline && pump()) {
        uint32_t f = frame++ % FRAMES;
        VK_CHECK(vkWaitForFences(device, 1, &fences[f], VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fences[f]));

        uint32_t index;
        VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[f],
                                           VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            /* fence was reset but nothing submitted: re-signal it via an empty submit */
            VK_CHECK(vkQueueSubmit(queue, 0, NULL, fences[f]));
            vkDeviceWaitIdle(device);
            goto recreate;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "acquire: %d\n", r);
            break;
        }

        VkCommandBuffer cmd = cmds[f];
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_dst = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = images[index],
                                       .subresourceRange = range};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &to_dst);
        VkClearColorValue color = {.float32 = {0.05f, 0.1f, 0.3f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &range);
        VkImageMemoryBarrier to_present = to_dst;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                             &to_present);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &acquired[f],
                           .pWaitDstStageMask = &stage,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &rendered[index]};
        VK_CHECK(vkQueueSubmit(queue, 1, &si, fences[f]));

        VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                    .waitSemaphoreCount = 1,
                                    .pWaitSemaphores = &rendered[index],
                                    .swapchainCount = 1,
                                    .pSwapchains = &swapchain,
                                    .pImageIndices = &index};
        r = vkQueuePresentKHR(queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(device);
            goto recreate;
        }
        if (r != VK_SUCCESS) {
            fprintf(stderr, "present: %d\n", r);
            break;
        }
    }

    vkDeviceWaitIdle(device);
    for (int i = 0; i < FRAMES; i++) {
        vkDestroySemaphore(device, acquired[i], NULL);
        vkDestroyFence(device, fences[i], NULL);
    }
    for (uint32_t i = 0; i < image_count; i++) {
        vkDestroySemaphore(device, rendered[i], NULL);
    }
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
    printf("vk: clean shutdown\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "gl") && strcmp(argv[1], "vk"))) {
        fprintf(stderr, "usage: %s gl|vk [seconds]\n", argv[0]);
        return 1;
    }
    bool vk = strcmp(argv[1], "vk") == 0;
    int seconds = argc > 2 ? atoi(argv[2]) : 5;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    char title[64];
    snprintf(title, sizeof(title), "tosu-overlay-test-%s", argv[1]);
    SDL_Window *window = SDL_CreateWindow(title, 800, 600,
                                          vk ? SDL_WINDOW_VULKAN : SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }

    main_window = window;
    resolve_sdl();
    if (getenv("TEST_EVENT_FILTER")) {
        p_SetEventFilter(event_filter, NULL);
    }

    printf("pid %d\n", (int)getpid());
    fflush(stdout);

    Uint64 deadline = SDL_GetTicks() + (Uint64)seconds * 1000;
    int result = vk ? run_vk(window, deadline) : run_gl(window, deadline);

    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
