/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * A frame is one command buffer: the draws, the clears, the texture uploads
 * and the window blit are recorded into it and submitted once. Two frames are
 * in flight (VK_SLOTS), each with its own command buffer, fence and staging
 * arenas, and the present is ordered by a semaphore instead of a fence wait,
 * so the FIFO walker records frame N while the GPU draws N-1. Everything that
 * really needs the device quiet (readbacks, growing an arena, releasing an
 * image a frame may still use) waits for the submission that could still
 * touch it.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */


#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <time.h>

#include "rsx_vulkan_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) X(vkFlushMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
#define VK_SLOTS 2                 /* frames in flight (see `slot` below) */

typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;   /* guest size the engine draws at */
    u32              out_width, out_height; /* display target (RSX_SCALE) */
    float            scale;           /* RSX_SCALE, 1.0 = guest size    */
    u32              scale_min;       /* RSX_SCALE_MIN                   */
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;             /* the slot being recorded (cmds[slot]) */
    VkFence          fence;           /* that slot's fence (fences[slot])    */
    /* A present recorded into the frame's own submission: the swapchain image
     * it was recorded into, and the semaphore the presentation engine waits
     * on (0 when the frame does not present). */
    u32              present_idx;
    VkSemaphore      submit_signal;

    /* Batched engine path. One command buffer, one render pass while the
     * targets do not change, and host-visible arenas for the vertices,
     * indices and constants of every draw; a frame is one submit. */
    int              batch_open;
    int              batch_pass_open;
    VkRenderPass     batch_pass;
    VkFramebuffer    batch_fb;
    u32              batch_w, batch_h;
    vk_buffer        batch_vbuf[VK_SLOTS], batch_ibuf[VK_SLOTS];
    vk_buffer        batch_ubo[VK_SLOTS],  batch_snap[VK_SLOTS];
    vk_buffer        batch_tex[VK_SLOTS];        /* texture upload staging */
    VkDeviceSize     batch_vbuf_end[VK_SLOTS], batch_ibuf_end[VK_SLOTS];
    VkDeviceSize     batch_ubo_end[VK_SLOTS],  batch_snap_end[VK_SLOTS];
    VkDeviceSize     batch_tex_end[VK_SLOTS];
    /* Two frames in flight: frame slots. Each has a command buffer, a fence
     * and its own staging arenas, so the walker records frame N while the GPU
     * still runs N-1. A present used to wait for its own frame's fence, which
     * left the CPU and the GPU taking turns (the wait alone measured 11 ms a
     * frame in play). */
    VkCommandBuffer  cmds[VK_SLOTS];
    VkFence          fences[VK_SLOTS];
    int              fence_busy[VK_SLOTS];   /* a submission is outstanding */
    u32              slot;                   /* frame slot being recorded  */
    u32              frame_index;
    int              frame_open;
    VkDescriptorPool batch_pool;
    u32              batch_sets;
    int              center_valid;
    u64              stat_flushes, stat_passes, stat_fb, stat_sets, stat_draws, stat_ubo;
    u64              stat_f_wait, stat_f_release, stat_f_arena, stat_f_upload;
    u64              stat_f_readback, stat_f_present, stat_f_snapshot;
    u64              stat_f_pipe, stat_f_fbdrop;
    u64              stat_waits;
    double           tm_stage, tm_set, tm_pass, tm_rec, tm_wait;
    vk_image         pending_img[1024];
    VkFramebuffer    pending_fb[512];
    vk_buffer        pending_buf[512];
    u32              pending_img_frame[1024];   /* the frame that queued it */
    u32              pending_fb_frame[512];
    u32              pending_buf_frame[512];
    u32              pending_imgs, pending_fbs, pending_bufs;
    u32              stat_frames;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              windowed;
    int              display_mode;    /* VK_DISP_*                      */
    int              vsync;
    u32              win_w, win_h;    /* windowed size, RSX_WINDOW      */
    int              resize_pending;
    SDL_Window*      window;
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
} vk_state;

static vk_state s_vk;

/* Set by the Graphics Settings page (rsx_draw_engine.c). */
extern volatile int g_rsx_display_reload;

#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
static int vk_load_library(void)
{
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

static void vk_destroy_image(vk_image* im)
{
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

/* Readback buffers want HOST_CACHED: coherent host memory on RADV is often
 * write-combined and uncached, so reading a 1280x720 depth image from it costs
 * tens of milliseconds per pass. Prefer a cached type (coherent when the
 * driver has one) and invalidate/flush explicitly when it is not. */
static int vk_create_host_buffer_cached(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags cc = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags c = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (vk_find_memory_type(req.memoryTypeBits, cc, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, c, &type) == 0) b->coherent = 0;
    else { pvkDestroyBuffer(s_vk.device, b->buf, NULL); b->buf = VK_NULL_HANDLE;
           return vk_create_host_buffer(b, size, usage); }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

static void vk_destroy_buffer(vk_buffer* b)
{
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host writes to non-coherent memory need a flush; reads need an
 * invalidate. The arenas and upload buffers only use coherent memory, but a
 * cached readback buffer may not be coherent, so both are explicit. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

static void vk_flush(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkFlushMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * Command submission
 *
 * The engine path records all of a frame's draws, clears and texture uploads
 * into one command buffer (`vk_batch_begin`), and it is submitted once, at
 * the present. Only the calls that genuinely need the device quiet -- a
 * readback, a target that has to be created now, an explicit query fence,
 * growing a staging arena, releasing something a frame may still use -- end
 * and submit it (`vk_batch_flush`). The older vtable path keeps its one-shot
 * `vk_begin` / `vk_submit_and_wait` behaviour because the host tests drive it
 * frame by frame.
 * -------------------------------------------------------------------------*/
#define VK_BATCH_ALIGN 256u

static void vk_batch_end_pass(void)
{
    if (!s_vk.batch_pass_open) return;
    pvkCmdEndRenderPass(s_vk.cmd);
    s_vk.batch_pass_open = 0;
    s_vk.batch_pass = VK_NULL_HANDLE;
    s_vk.batch_fb = VK_NULL_HANDLE;
}

static int vk_cmd_begin(void)
{
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    return 0;
}

static double vk_now_ms(void);      /* with the engine's stats, further down */
static void vk_pending_drain(u32 upto, int all);

/* Wait for a slot's outstanding submission, if it has one. */
static int vk_wait_fence(u32 slot)
{
    if (!s_vk.fence_busy[slot]) return 0;
    const double t0 = vk_now_ms();
    VK_CHECK(pvkWaitForFences(s_vk.device, 1, &s_vk.fences[slot], VK_TRUE, UINT64_MAX),
             "vkWaitForFences");
    s_vk.tm_wait += vk_now_ms() - t0;
    s_vk.stat_waits++;
    s_vk.fence_busy[slot] = 0;
    return 0;
}

/* Submit the frame's command buffer. `wait` is 0 for the frame's own
 * submission -- a present is ordered by its semaphore, so the walker can
 * start the next frame while the GPU still draws this one -- and 1 for the
 * synchronous operations (uploads, readbacks) that need the device quiet. */
static int vk_submit(int wait)
{
    vk_batch_end_pass();
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    /* A present recorded into this submission also takes the swapchain image
     * (it waits for the image to be free) and hands it to the presentation
     * engine (signalling the semaphore the present waits on), so a frame is
     * one submit rather than two. */
    if (s_vk.submit_signal) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_vk.sem_acquire;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_vk.submit_signal;
    }
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(pvkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence), "vkQueueSubmit");
    s_vk.fence_busy[s_vk.slot] = 1;
    s_vk.batch_open = 0;
    s_vk.stat_flushes++;
    if (!wait) return 0;
    if (vk_wait_fence(s_vk.slot)) return -1;
    vk_pending_drain(0, 1);       /* the newest submission is complete */
    return 0;
}

static int vk_submit_and_wait(void) { return vk_submit(1); }

static int vk_batch_flush(void) { return s_vk.batch_open ? vk_submit_and_wait() : 0; }

/* Images, views and framebuffers released while commands that may still use
 * them are recorded cannot be destroyed yet. Each is queued with the frame it
 * was released in, and freed once every submission that could still refer to
 * it has completed -- with two frames in flight that is two frames later, not
 * the next wait. `all` drains everything (the device is idle then).
 * `upto` is the newest frame known to have completed. */
static void vk_pending_drain(u32 upto, int all)
{
    u32 keep = 0;
    for (u32 i = 0; i < s_vk.pending_imgs; i++) {
        if (all || s_vk.pending_img_frame[i] <= upto) { vk_destroy_image(&s_vk.pending_img[i]); continue; }
        if (keep != i) {
            s_vk.pending_img[keep] = s_vk.pending_img[i];
            s_vk.pending_img_frame[keep] = s_vk.pending_img_frame[i];
        }
        keep++;
    }
    s_vk.pending_imgs = keep;
    keep = 0;
    for (u32 i = 0; i < s_vk.pending_fbs; i++) {
        if (all || s_vk.pending_fb_frame[i] <= upto) {
            pvkDestroyFramebuffer(s_vk.device, s_vk.pending_fb[i], NULL); continue;
        }
        if (keep != i) {
            s_vk.pending_fb[keep] = s_vk.pending_fb[i];
            s_vk.pending_fb_frame[keep] = s_vk.pending_fb_frame[i];
        }
        keep++;
    }
    s_vk.pending_fbs = keep;
    keep = 0;
    for (u32 i = 0; i < s_vk.pending_bufs; i++) {
        if (all || s_vk.pending_buf_frame[i] <= upto) { vk_destroy_buffer(&s_vk.pending_buf[i]); continue; }
        if (keep != i) {
            s_vk.pending_buf[keep] = s_vk.pending_buf[i];
            s_vk.pending_buf_frame[keep] = s_vk.pending_buf_frame[i];
        }
        keep++;
    }
    s_vk.pending_bufs = keep;
}

static void vk_pending_image(vk_image* im)
{
    if (!im->img && !im->view && !im->mem) return;
    if (s_vk.pending_imgs >= (u32)(sizeof s_vk.pending_img / sizeof s_vk.pending_img[0])) {
        (void)vk_batch_flush();                     /* newest submission: all done */
        vk_pending_drain(0, 1);
    }
    s_vk.pending_img_frame[s_vk.pending_imgs] = s_vk.frame_index;
    s_vk.pending_img[s_vk.pending_imgs++] = *im;
    memset(im, 0, sizeof *im);
}

static void vk_pending_buffer(vk_buffer* b)
{
    if (!b->buf && !b->mem) return;
    if (s_vk.pending_bufs >= (u32)(sizeof s_vk.pending_buf / sizeof s_vk.pending_buf[0])) {
        (void)vk_batch_flush();
        vk_pending_drain(0, 1);
    }
    s_vk.pending_buf_frame[s_vk.pending_bufs] = s_vk.frame_index;
    s_vk.pending_buf[s_vk.pending_bufs++] = *b;
    memset(b, 0, sizeof *b);
}

static void vk_pending_framebuffer(VkFramebuffer fb)
{
    if (!fb) return;
    if (s_vk.pending_fbs >= (u32)(sizeof s_vk.pending_fb / sizeof s_vk.pending_fb[0])) {
        (void)vk_batch_flush();
        vk_pending_drain(0, 1);
    }
    s_vk.pending_fb_frame[s_vk.pending_fbs] = s_vk.frame_index;
    s_vk.pending_fb[s_vk.pending_fbs++] = fb;
}

/* Start recording the next frame: pick its slot, and wait for the
 * submission that used that slot two frames ago -- a no-op when the GPU keeps
 * up, and the point where its command buffer and staging arenas become
 * reusable when it does not. */
static int vk_frame_begin(void)
{
    if (s_vk.frame_open) return 0;
    const u32 slot = s_vk.frame_index & 1u;
    s_vk.slot = slot;
    s_vk.cmd = s_vk.cmds[slot];
    s_vk.fence = s_vk.fences[slot];
    if (s_vk.fence_busy[slot]) {
        if (vk_wait_fence(slot)) return -1;
        /* Frames up to N-2 have completed; frame N-1 may still run, so what
         * it released (or frame N-2 did) is not destroyed yet. */
        vk_pending_drain(s_vk.frame_index >= 2u ? s_vk.frame_index - 2u : 0u, 0);
    }
    s_vk.batch_vbuf_end[slot] = s_vk.batch_ibuf_end[slot] = 0;
    s_vk.batch_ubo_end[slot] = s_vk.batch_snap_end[slot] = 0;
    s_vk.batch_tex_end[slot] = 0;
    s_vk.frame_open = 1;
    return 0;
}

/* A synchronous operation needs the command buffer to itself and the GPU
 * quiet: finish what is batched, wait for every slot still in flight, then
 * record on its own. Uploads, readbacks and target creation take this path;
 * in play they are rare (a warm frame uploads nothing). */
static int vk_begin(void)
{
    if (s_vk.batch_open && vk_batch_flush()) return -1;
    if (vk_frame_begin()) return -1;
    for (u32 slot = 0; slot < VK_SLOTS; slot++)
        if (s_vk.fence_busy[slot] && vk_wait_fence(slot)) return -1;
    vk_pending_drain(0, 1);
    return vk_cmd_begin();
}

static int vk_batch_begin(void)
{
    if (s_vk.batch_open) return 0;
    if (vk_frame_begin()) return -1;
    if (vk_cmd_begin()) return -1;
    s_vk.batch_open = 1;
    return 0;
}

/* Begin (or keep) one render pass. Draws to the same framebuffer share it;
 * a new target set ends the pass and puts a conservative full barrier
 * between the writes it made and whatever the next pass reads. */
static int vk_batch_begin_pass(VkRenderPass rp, VkFramebuffer fb, u32 w, u32 h)
{
    if (s_vk.batch_pass_open && s_vk.batch_pass == rp && s_vk.batch_fb == fb &&
        s_vk.batch_w == w && s_vk.batch_h == h)
        return 0;
    vk_batch_end_pass();
    if (vk_batch_begin()) return -1;
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    s_vk.batch_pass_open = 1;
    s_vk.batch_pass = rp;
    s_vk.batch_fb = fb;
    s_vk.batch_w = w;
    s_vk.batch_h = h;
    s_vk.stat_passes++;
    return 0;
}

/* Append to a host-visible arena. A flush before reuse is safe because it
 * waits, and resetting `used` is safe because only this arena moved. */
static int vk_arena_alloc(vk_buffer* b, VkDeviceSize* used, const void* src,
                          VkDeviceSize bytes, VkBufferUsageFlags usage,
                          VkDeviceSize* out_off)
{
    if (!bytes) { *out_off = 0; return 0; }
    VkDeviceSize at = (*used + VK_BATCH_ALIGN - 1u) & ~(VkDeviceSize)(VK_BATCH_ALIGN - 1u);
    if (!b->buf || at + bytes > b->size) {
        s_vk.stat_f_arena++;
        {
            static u32 dbg;
            if (dbg < 40u) {
                VK_LOG("batch: arena overflow usage=0x%x size=%llu at=%llu bytes=%llu\n",
                       (unsigned)usage, (unsigned long long)b->size,
                       (unsigned long long)at, (unsigned long long)bytes);
                dbg++;
            }
        }
        if (vk_batch_flush()) return -1;
        if (!b->buf || bytes > b->size) {
            vk_destroy_buffer(b);
            /* Constants are staged per draw, so their arena is the big one:
             * a frame's worth should fit a single buffer. */
            /* One frame's worth each: the walker resets them at every frame
             * now, so they no longer have to hold the whole session, and
             * there is a set per frame slot. */
            VkDeviceSize size = (usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
                                    ? 16u << 20
                                    : (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ? 64u << 20
                                    : (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ? 8u << 20
                                    : (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? 64u << 20
                                                                                 : 16u << 20;
            while (size < bytes) size <<= 1;
            if (vk_create_host_buffer(b, size, usage) || !b->coherent) {
                VK_LOG("batching: no host-coherent arena memory\n");
                vk_destroy_buffer(b);
                return -1;
            }
        }
        *used = 0;
        at = 0;
    }
    if (src) memcpy((u8*)b->ptr + at, src, (size_t)bytes);
    *used = at + bytes;
    *out_off = at;
    return 0;
}

/* The per-draw descriptor sets point at slices of the frame's arenas. */
static VkDescriptorSet vk_batch_descriptor_set(void);

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1 },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
/* The depth format's aspects: depth alone, or depth and stencil when the
 * chosen format carries one. The Windows and macOS backends both use a
 * depth+stencil format (D32_FLOAT_S8X24 / Depth32Float_Stencil8) and the
 * title's masked effects and shadow volumes test the stencil, so this backend
 * asks for the same thing. D32_SFLOAT_S8X24 keeps the depth aspect 32-bit
 * float, so the depth snapshots below copy out unchanged. */
static VkImageAspectFlags vk_depth_aspects(void)
{
    return (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
            s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
               ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
               : VK_IMAGE_ASPECT_DEPTH_BIT;
}
static int vk_depth_has_stencil(void) { return (vk_depth_aspects() & VK_IMAGE_ASPECT_STENCIL_BIT) != 0; }

static int vk_pick_depth_format(void)
{
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; return 0;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; return 0;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.out_width,
                                    .height = s_vk.out_height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

#define VK_BATCH_SETS 4096u
static VkDescriptorSet vk_batch_descriptor_set(void)
{
    if (!s_vk.batch_pool) {
        VkDescriptorPoolSize ps[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_BATCH_SETS * 2u },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_BATCH_SETS * RSX_MAX_TEXTURES },
            { VK_DESCRIPTOR_TYPE_SAMPLER, VK_BATCH_SETS * RSX_MAX_TEXTURES },
        };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .maxSets = VK_BATCH_SETS,
                                           .poolSizeCount = 3, .pPoolSizes = ps };
        if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.batch_pool) != VK_SUCCESS) {
            static int warned;
            if (!warned) { VK_LOG("engine: no descriptor pool for per-draw sets\n"); warned = 1; }
            return VK_NULL_HANDLE;
        }
    }
    if (s_vk.batch_sets >= VK_BATCH_SETS) {
        if (vk_batch_flush()) return VK_NULL_HANDLE;
        if (pvkResetDescriptorPool(s_vk.device, s_vk.batch_pool, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
        s_vk.batch_sets = 0;
    }
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.batch_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_g.set_layout };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) {
        if (vk_batch_flush()) return VK_NULL_HANDLE;
        if (pvkResetDescriptorPool(s_vk.device, s_vk.batch_pool, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
        s_vk.batch_sets = 0;
        if (pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    s_vk.batch_sets++;
    s_vk.stat_sets++;
    return set;
}

/* The version the instance was made with (vk_init_all): 1.1 (SPIR-V 1.3 for
 * the translated guest programs, negative viewport heights), or 1.0 from a
 * loader that knows nothing newer -- the fixed-function path alone, then. */
static u32 s_vk_api = VK_API_VERSION_1_0;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32* words = NULL;
    const u32 n = rsx_hlsl_to_spirv(s_g.hlsl, stage, &words, s_g.log, sizeof s_g.log);
    if (!n || !words) {
        VK_LOG("%s: %s\n", what, s_g.log);
        free(words);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = words };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        m = VK_NULL_HANDLE;
    }
    free(words);
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SPV_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SPV_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &p);
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    if (!s_g.hlsl) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    /* The translator writes SPIR-V 1.3, which takes Vulkan 1.1 of the
     * instance (s_vk_api) and of the device alike. */
    if (s_vk_api < VK_API_VERSION_1_1 || p.apiVersion < VK_API_VERSION_1_1) {
        VK_LOG("guest programs: off, the translated shaders need Vulkan 1.1\n");
        return 0;
    }
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        { RSX_SPV_BIND_VS_CB, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPV_BIND_FS_CB, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPV_BIND_FS_TEX, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPV_BIND_FS_SAMP, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPV_BIND_VS_CB, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPV_BIND_FS_CB, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPV_BIND_FS_TEX, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPV_BIND_FS_SAMP, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.out_width, s_vk.out_height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* D3D clip space from the decompilers, flipped by the negative height. */
    VkViewport vpt = { 0.0f, (float)s_vk.out_height, (float)s_vk.out_width, -(float)s_vk.out_height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.out_width, s_vk.out_height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 0, NULL);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.out_width, s_vk.out_height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.out_width, (float)s_vk.out_height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.out_width, s_vk.out_height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, stencil };
        VkImageSubresourceRange r = { vk_depth_aspects(), 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

/* RSX_SCALE / RSX_SCALE_MIN: the register-file engine's internal resolution.
 * Guest targets of at least scale_min in both directions are made scale times
 * larger; viewports, scissors and readbacks follow. */
enum { VK_DISP_WINDOWED = 0, VK_DISP_BORDERLESS, VK_DISP_FULLSCREEN };

static u32 vk_sc_dim(u32 v)
{
    u32 r = (u32)((float)v * s_vk.scale + 0.5f);
    return v && !r ? 1u : r;
}
static int vk_scales(u32 w, u32 h)
{
    return s_vk.scale != 1.0f && w >= s_vk.scale_min && h >= s_vk.scale_min;
}
static void vk_read_scale(void)
{
    s_vk.scale_min = 64;
    const char* m = getenv("RSX_SCALE_MIN");
    if (m && *m) s_vk.scale_min = (u32)atoi(m);
    s_vk.scale = 1.0f;
    const char* e = getenv("RSX_SCALE");
    if (e && *e) {
        const float f = (float)atof(e);
        if (f >= 0.5f && f <= 8.0f) s_vk.scale = f;
        else VK_LOG("RSX_SCALE=%s ignored (0.5 to 8)\n", e);
    }
    if (s_vk.width && s_vk.height) {
        s_vk.out_width  = vk_scales(s_vk.width, s_vk.height) ? vk_sc_dim(s_vk.width)  : s_vk.width;
        s_vk.out_height = vk_scales(s_vk.width, s_vk.height) ? vk_sc_dim(s_vk.height) : s_vk.height;
        if (s_vk.out_width > 4096u)  s_vk.out_width  = s_vk.width;
        if (s_vk.out_height > 4096u) s_vk.out_height = s_vk.height;
        if (s_vk.scale != 1.0f)
            VK_LOG("RSX_SCALE %.2f: %ux%u -> %ux%u (min %u)\n",
                   s_vk.scale, s_vk.width, s_vk.height,
                   s_vk.out_width, s_vk.out_height, s_vk.scale_min);
    }
}
static void vk_read_display(void)
{
    s_vk.display_mode = VK_DISP_WINDOWED;
    const char* dm = getenv("RSX_DISPLAY");
    if (dm && *dm) {
        if (!strcasecmp(dm, "borderless")) s_vk.display_mode = VK_DISP_BORDERLESS;
        else if (!strcasecmp(dm, "fullscreen") || !strcasecmp(dm, "exclusive")) s_vk.display_mode = VK_DISP_FULLSCREEN;
        else if (strcasecmp(dm, "windowed") && strcasecmp(dm, "window"))
            VK_LOG("RSX_DISPLAY=%s ignored (windowed, borderless or fullscreen)\n", dm);
    } else if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) {
        s_vk.display_mode = VK_DISP_BORDERLESS;
    }

    const char* ws = getenv("RSX_WINDOW");
    unsigned ww = 0, wh = 0;
    const int have_ws = ws && *ws && sscanf(ws, "%ux%u", &ww, &wh) == 2 &&
                        ww >= 320 && wh >= 240 && ww <= 16384 && wh <= 16384;
    if (ws && *ws && !have_ws) VK_LOG("RSX_WINDOW=%s ignored (want <w>x<h>)\n", ws);

    s_vk.win_w = s_vk.out_width ? s_vk.out_width : s_vk.width;
    s_vk.win_h = s_vk.out_height ? s_vk.out_height : s_vk.height;
    if (s_vk.display_mode == VK_DISP_WINDOWED) {
        if (have_ws) { s_vk.win_w = ww; s_vk.win_h = wh; }
    } else {
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
            s_vk.win_w = (u32)mode.w;
            s_vk.win_h = (u32)mode.h;
        }
    }
    const char* v = getenv("RSX_VSYNC");
    s_vk.vsync = (v && *v) ? (atoi(v) ? 1 : 0) : 1;
}

static void vk_swapchain_destroy(void)
{
    if (s_vk.device && pvkDeviceWaitIdle) {
        pvkDeviceWaitIdle(s_vk.device);
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    vk_swapchain_destroy();
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    vk_read_display();
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (s_vk.display_mode == VK_DISP_WINDOWED) {
        flags |= SDL_WINDOW_RESIZABLE;
    } else if (s_vk.display_mode == VK_DISP_BORDERLESS) {
        flags |= SDL_WINDOW_BORDERLESS;
    } else {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)s_vk.win_w, (int)s_vk.win_h, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s), %s %ux%u\n",
           SDL_GetCurrentVideoDriver(), n,
           s_vk.display_mode == VK_DISP_WINDOWED ? "windowed" :
           s_vk.display_mode == VK_DISP_BORDERLESS ? "borderless" : "fullscreen",
           s_vk.win_w, s_vk.win_h);
}

static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    /* Wayland drivers can expose more than 64 formats; the second call then
     * returns VK_INCOMPLETE because the array is smaller than the full list.
     * The first 64 are enough for the format choice. */
    {
        const VkResult fr = pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts);
        if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) {
            VK_LOG("vkGetPhysicalDeviceSurfaceFormatsKHR failed (VkResult %d)\n", (int)fr);
            return -1;
        }
    }
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        SDL_Vulkan_GetDrawableSize(s_vk.window, &w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) { VK_LOG("window has no drawable area\n"); return -1; }

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!s_vk.vsync) {
        u32 npm = 0;
        if (pvkGetPhysicalDeviceSurfacePresentModesKHR &&
            pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &npm, NULL) == VK_SUCCESS &&
            npm) {
            if (npm > 16) npm = 16;
            VkPresentModeKHR modes[16];
            if (pvkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.phys, s_vk.surface, &npm, modes) == VK_SUCCESS) {
                int imm = 0, mb = 0;
                for (u32 i = 0; i < npm; i++) {
                    if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) imm = 1;
                    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mb = 1;
                }
                present_mode = imm ? VK_PRESENT_MODE_IMMEDIATE_KHR
                             : mb  ? VK_PRESENT_MODE_MAILBOX_KHR
                                   : VK_PRESENT_MODE_FIFO_KHR;
            }
        }
    }

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = present_mode,
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d, %s\n",
           ext.width, ext.height, n, (int)fmt.format,
           present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" :
           present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return 0;
}

static void vk_window_mode_apply(void)
{
    if (!s_vk.window) return;
    switch (s_vk.display_mode) {
    case VK_DISP_WINDOWED:
        SDL_SetWindowFullscreen(s_vk.window, 0);
        SDL_SetWindowBordered(s_vk.window, SDL_TRUE);
        SDL_SetWindowSize(s_vk.window, (int)s_vk.win_w, (int)s_vk.win_h);
        SDL_SetWindowPosition(s_vk.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        break;
    case VK_DISP_BORDERLESS:
        SDL_SetWindowFullscreen(s_vk.window, 0);
        SDL_SetWindowBordered(s_vk.window, SDL_FALSE);
        SDL_SetWindowSize(s_vk.window, (int)s_vk.win_w, (int)s_vk.win_h);
        SDL_SetWindowPosition(s_vk.window, 0, 0);
        break;
    case VK_DISP_FULLSCREEN:
    default:
        SDL_SetWindowFullscreen(s_vk.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        break;
    }
    SDL_PumpEvents();
}

static int vk_swapchain_make(void)
{
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS)
        return -1;
    return vk_create_swapchain();
}

static void vk_display_reload(void)
{
    g_rsx_display_reload = 0;
    const int old_mode = s_vk.display_mode;
    const u32 old_w = s_vk.win_w, old_h = s_vk.win_h;
    const int old_vsync = s_vk.vsync;
    vk_read_display();
    if (s_vk.display_mode == old_mode && s_vk.win_w == old_w &&
        s_vk.win_h == old_h && s_vk.vsync == old_vsync)
        return;
    if (!s_vk.window) return;
    vk_swapchain_destroy();
    if (s_vk.display_mode != old_mode || s_vk.win_w != old_w || s_vk.win_h != old_h)
        vk_window_mode_apply();
    if (s_vk.device && vk_swapchain_make())
        VK_LOG("could not remake the swapchain for the new display mode\n");
    VK_LOG("display: %s %ux%u, vsync %d\n",
           s_vk.display_mode == VK_DISP_WINDOWED ? "windowed" :
           s_vk.display_mode == VK_DISP_BORDERLESS ? "borderless" : "fullscreen",
           s_vk.win_w, s_vk.win_h, s_vk.vsync);
}

static void vk_swapchain_lost(void)
{
    pvkDeviceWaitIdle(s_vk.device);
    vk_swapchain_destroy();
    if (vk_swapchain_make()) vk_window_drop("the swapchain could not be recreated");
}

/* Blit the offscreen target into the next swapchain image and present it.
 *
 * The blit is recorded into the frame's own command buffer (before it is
 * submitted), so a frame is one submission: before this it was the frame's
 * submit and wait, then a second command buffer for the blit, then a second
 * submit and wait, and only then the present -- two full CPU/GPU round trips
 * a frame, with the GPU idle in between. */
static int vk_present_record(void)
{
    if (!s_vk.windowed || !s_vk.swapchain) return -1;

    u32 idx = 0;
    VkResult r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return -1; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return -1;
    }

    vk_batch_end_pass();
    if (vk_batch_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.out_width, (int32_t)s_vk.out_height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)s_vk.sc_extent.width, (int32_t)s_vk.sc_extent.height, 1 } },
    };
    pvkCmdBlitImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    /* The submission that carries this blit also signals `submit_signal`. */
    s_vk.present_idx = idx;
    s_vk.submit_signal = s_vk.sc_done[idx];
    return 0;
}

/* Hand the image the frame's submission wrote to the presentation engine.
 * Called after the frame's fence has been waited on, so its semaphore is
 * already signalled. */
static void vk_present_submit(void)
{
    if (!s_vk.submit_signal) return;
    VkSemaphore done = s_vk.submit_signal;
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &done,
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain,
        .pImageIndices = &s_vk.present_idx,
    };
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, &pi);
    s_vk.submit_signal = VK_NULL_HANDLE;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

/* One whole present with nothing new drawn: the display target as it stands
 * (the shutdown hold). */
static void vk_present_frame(void)
{
    if (vk_present_record()) return;
    (void)vk_batch_flush();
    vk_present_submit();
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    /* Write a complete file and rename it into place: a reader of the
     * path (a test, a screen grab loop) never sees a half-written frame. */
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_vk.dump_path);
    FILE* f = fopen(tmp, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", tmp); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.out_width, s_vk.out_height);
    for (size_t i = 0; i < (size_t)s_vk.out_width * s_vk.out_height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    if (fclose(f) == 0) rename(tmp, s_vk.dump_path);
    else remove(tmp);
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;

    /* The engine batches the frame; the vtable path has already submitted
     * each operation synchronously. Record and submit only an open batch. */
    if (s_vk.windowed && s_vk.swapchain) (void)vk_present_record();
    if (s_vk.batch_open) {
        s_vk.stat_f_present++;
        if (vk_submit(0)) return;
        vk_present_submit();
    }
    s_vk.frame_open = 0;
    s_vk.frame_index++;
    s_vk.center_valid = 0;

    /* The game does not read the frame back: its tests that do go through
     * the engine's readback, and PS3RECOMP_VK_DUMP asks here. The vtable
     * path still keeps the old full-frame readback for its pixel checks. */
    if (!s_vk.eng_active || s_vk.dump_path) {
        if (vk_begin()) return;
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { s_vk.out_width, s_vk.out_height, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                                s_vk.readback.buf, 1, &region);
        VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &host, 0, NULL);
        if (vk_submit_and_wait()) return;

        vk_invalidate(&s_vk.readback);
        const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
        size_t c = ((size_t)(s_vk.out_height / 2) * s_vk.out_width + s_vk.out_width / 2) * 4;
        s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
        s_vk.center_valid = 1;
        if (s_vk.dump_path) vk_dump_ppm(px);
    }
    s_vk.presented++;
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
    s_vk.stat_frames++;
    if (getenv("RSX_VK_BATCH_STATS") && (s_vk.stat_frames % 60u) == 0) {
        VK_LOG("batch: %u frames, %.1f flushes/frame (wait %.1f release %.1f "
               "arena %.1f upload %.1f readback %.1f snapshot %.1f present %.1f), "
               "%.1f passes, %.1f sets, %.1f draws, %.2f MB UBO, pipe %.1f fbdrop %.1f\n",
               s_vk.stat_frames, (double)s_vk.stat_flushes / 60.0,
               (double)s_vk.stat_f_wait / 60.0, (double)s_vk.stat_f_release / 60.0,
               (double)s_vk.stat_f_arena / 60.0, (double)s_vk.stat_f_upload / 60.0,
               (double)s_vk.stat_f_readback / 60.0, (double)s_vk.stat_f_snapshot / 60.0,
               (double)s_vk.stat_f_present / 60.0, (double)s_vk.stat_passes / 60.0,
               (double)s_vk.stat_sets / 60.0, (double)s_vk.stat_draws / 60.0,
               (double)s_vk.stat_ubo / (60.0 * 1048576.0),
               (double)s_vk.stat_f_pipe / 60.0, (double)s_vk.stat_f_fbdrop / 60.0);
        VK_LOG("batch: draw ms/frame stage %.2f set %.2f pass %.2f record %.2f; "
               "%.1f GPU waits/frame, %.2f ms waiting\n",
               s_vk.tm_stage / 60.0, s_vk.tm_set / 60.0,
               s_vk.tm_pass / 60.0, s_vk.tm_rec / 60.0,
               (double)s_vk.stat_waits / 60.0, s_vk.tm_wait / 60.0);
        s_vk.tm_stage = s_vk.tm_set = s_vk.tm_pass = s_vk.tm_rec = 0.0;
        s_vk.tm_wait = 0.0; s_vk.stat_waits = 0;
        VK_LOG("batch: arenas vbuf %.1f/%.1f MB, ibuf %.1f/%.1f MB, ubo %.1f/%.1f MB\n",
               (double)s_vk.batch_vbuf_end[s_vk.slot] / 1048576.0,
               (double)s_vk.batch_vbuf[s_vk.slot].size / 1048576.0,
               (double)s_vk.batch_ibuf_end[s_vk.slot] / 1048576.0,
               (double)s_vk.batch_ibuf[s_vk.slot].size / 1048576.0,
               (double)s_vk.batch_ubo_end[s_vk.slot] / 1048576.0,
               (double)s_vk.batch_ubo[s_vk.slot].size / 1048576.0);
        s_vk.stat_flushes = s_vk.stat_passes = s_vk.stat_fb = 0;
        s_vk.stat_sets = s_vk.stat_draws = s_vk.stat_ubo = 0;
        s_vk.stat_f_wait = s_vk.stat_f_release = s_vk.stat_f_arena = 0;
        s_vk.stat_f_upload = s_vk.stat_f_readback = s_vk.stat_f_present = 0;
        s_vk.stat_f_snapshot = 0;
        s_vk.stat_f_pipe = s_vk.stat_f_fbdrop = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 2048          /* the engine's texture cache is 1024 */
#define VK_ENG_WRAP_MAX 4096         /* cached vertex/index buffers */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;                   /* host size (scaled)               */
    u32      gw, gh;                 /* guest size                       */
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];

/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static int vk_eng_rt_class(VkFormat f);
static int vk_eng2_init(void);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

static u32 vk_eng_alloc(void)
{
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_FREE) return i;
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Copy `rows` rows of `w` texels from host memory into one level / layer of
 * an image that is in GENERAL. */
/* Stage a level into the frame's upload arena and record the copy into the
 * frame's command buffer, so an upload costs nothing beyond the memcpy. Each
 * level used to make its own staging buffer, submit and wait for the whole
 * device: in play the engine uploads a few textures a frame (the fire, water
 * and HUD animations), and those waits were most of the CPU's time. The
 * barrier before the copy orders it after anything recorded earlier. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    const VkDeviceSize bytes = (VkDeviceSize)tight * rows;
    if (s_vk.batch_open) s_vk.stat_f_upload++;
    vk_batch_end_pass();
    if (vk_batch_begin()) return -1;
    VkDeviceSize off = 0;
    if (vk_arena_alloc(&s_vk.batch_tex[s_vk.slot], &s_vk.batch_tex_end[s_vk.slot], NULL,
                       bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &off))
        return -1;
    u8* dst = (u8*)s_vk.batch_tex[s_vk.slot].ptr + off;
    for (u32 y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    /* The arena may have ended the batch to grow (that submission waited);
     * start the next one before recording into it. */
    if (vk_batch_begin()) return -1;
    vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .bufferOffset = off,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
        .imageExtent = { w, h, 1 },
    };
    pvkCmdCopyBufferToImage(s_vk.cmd, s_vk.batch_tex[s_vk.slot].buf, img,
                            VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    return 0;
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, 1 },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    return vk_eng2_init();
}

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE) return;
    /* Drop every view of the object being released, whatever its kind. */
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
            vk_pending_image(&s_eobj[i].im);
            memset(&s_eobj[i], 0, sizeof s_eobj[i]);
        }
    if (s_eobj[handle].kind == VK_ENG_COLOR && s_eobj[handle].surface) {
        /* The colour snapshot belongs to this surface; release it and every
         * view the engine made of it before the target handle is recycled. */
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers();
    vk_pending_image(&s_eobj[handle].im);
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_buffer_release(void* user, u32 buffer);

static void vk_eng_shutdown(void* user)
{
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
    for (u32 i = 1; i < VK_ENG_WRAP_MAX; i++) vk_eng_buffer_release(user, i);
    (void)vk_batch_flush();
    vk_pending_drain(0, 1);
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;
    if (s_vk.batch_open) s_vk.stat_f_wait++;
    vk_batch_flush();
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const u32 gw = w, gh = h;
    if (vk_scales(gw, gh)) { w = vk_sc_dim(gw); h = vk_sc_dim(gh); }
    if (w > 4096u || h > 4096u) { w = gw; h = gh; }
    if (vk_eng_rt_class(vf) < 0 || !w || !h || w > 4096u || h > 4096u) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, vf, w, h,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h;
    o->gw = gw; o->gh = gh;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. The
     * initialisation stays in the frame's batch; a seed upload flushes it. */
    if (vk_batch_begin()) { vk_eng_release(user, hd); return 0; }
    vk_batch_end_pass();
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (seed && seed_row_bytes) {
        if (w != gw || h != gh) {
            u8* tmp = (u8*)malloc((size_t)w * h * bpp);
            if (tmp) {
                for (u32 yy = 0; yy < h; yy++)
                    for (u32 xx = 0; xx < w; xx++)
                        memcpy(tmp + ((size_t)yy * w + xx) * bpp,
                               (const u8*)seed + ((size_t)(yy * gh / h) * seed_row_bytes) +
                                                (size_t)(xx * gw / w) * bpp, bpp);
                vk_eng_upload(o->im.img, 0, 0, w, h, bpp, tmp, w * bpp);
                free(tmp);
            }
        } else {
            vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
        }
    }
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    const u32 gw = w, gh = h;
    if (vk_scales(gw, gh)) { w = vk_sc_dim(gw); h = vk_sc_dim(gh); }
    if (w > 4096u || h > 4096u) { w = gw; h = gh; }
    if (!w || !h || w > 4096u || h > 4096u) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image(&o->im, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        vk_depth_aspects())) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w; o->h = h;
    o->gw = gw; o->gh = gh;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_batch_begin()) { vk_eng_release(user, hd); return 0; }
    vk_batch_end_pass();
    vk_barrier_image(o->im.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { vk_depth_aspects(), 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || vk_batch_begin()) return;
    vk_batch_end_pass();
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!o) return;
    /* Clear only what was asked for: a stencil-only clear must leave the
     * depth values a shadow pass already wrote. */
    VkImageAspectFlags aspects = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & RSX_BE_CLEAR_STENCIL) && vk_depth_has_stencil()) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspects || vk_batch_begin()) return;
    vk_batch_end_pass();
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { aspects, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, aspects, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (o && !vk_batch_begin()) {
        vk_batch_end_pass();
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.out_width, (int32_t)s_vk.out_height, 1 } },
        };
        pvkCmdBlitImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                        s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                        (o->w == s_vk.out_width && o->h == s_vk.out_height) ? VK_FILTER_NEAREST
                                                                            : VK_FILTER_LINEAR);
    }
    vk_cb_present(&s_vk, 0);
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    if (s_vk.batch_open) s_vk.stat_f_readback++;
    (void)vk_batch_flush();
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->gw || y + h > o->gh) return;
    /* The engine asks in guest pixels; the target may be scale times larger. */
    u32 hx0 = (u32)((u64)x * o->w / o->gw), hx1 = (u32)((u64)(x + w) * o->w / o->gw);
    u32 hy0 = (u32)((u64)y * o->h / o->gh), hy1 = (u32)((u64)(y + h) * o->h / o->gh);
    if (hx1 > o->w) hx1 = o->w;
    if (hy1 > o->h) hy1 = o->h;
    if (hx1 <= hx0) hx1 = hx0 + 1;
    if (hy1 <= hy0) hy1 = hy0 + 1;
    const u32 hw = hx1 - hx0, hh = hy1 - hy0;
    const size_t tight = (size_t)hw * o->bpp;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * hh, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return;
    }
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)hx0, (int32_t)hy0, 0 }, .imageExtent = { hw, hh, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_submit_and_wait()) {
            vk_invalidate(&st);
            for (u32 row = 0; row < h; row++) {
                u32 sy = (u32)(((u64)row * hh + hh / 2) / h);
                if (sy >= hh) sy = hh - 1;
                const u8* src = (const u8*)st.ptr + (size_t)sy * tight;
                u8* dst = (u8*)out + (size_t)row * out_pitch;
                for (u32 col = 0; col < w; col++) {
                    u32 sx = (u32)(((u64)col * hw + hw / 2) / w);
                    if (sx >= hw) sx = hw - 1;
                    memcpy(dst + (size_t)col * o->bpp,
                           src + (size_t)sx * o->bpp, o->bpp);
                }
            }
        }
    }
    vk_destroy_buffer(&st);
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. Cube maps are not handled yet:
 * the pixel decompiler declares cube units as separate bindings, which the
 * shared set layout does not have, so faces == 6 reports 0 (placeholder). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > 4096u || h > 4096u || (bc && !s_vk.bc_ok))
        return 0;
    if (faces == 6) {
        static int warned_cube;
        if (!warned_cube) { VK_LOG("engine: cube textures not supported yet\n"); warned_cube = 1; }
        return 0;
    }
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level before the uploads arrive. */
    if (vk_batch_begin()) goto fail;
    vk_batch_end_pass();
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, 0, mip, w, h, tight, rows, src, row_bytes);
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    /* A colour snapshot is a texture, not a target, but the engine binds it
     * through the same unit crossbar; accept either kind here. */
    if (!t) t = vk_eng_get(surface, VK_ENG_TEXTURE);
    if (!t) return 0;
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
            s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format)
            return i;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->gw = t->gw; o->gh = t->gh;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    return hd;
}

/* An in-place post-process samples the colour target it is writing, which
 * is undefined on every host API. Copy the target to a sampleable twin at
 * this point in the stream and return it; the engine binds the twin with the
 * unit's crossbar. One twin per source target is reused (the engine asks
 * several times a frame), and it is released with the source. */
static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    vk_eng_obj* src = vk_eng_get(surface, VK_ENG_COLOR);
    if (!src) return 0;

    u32 snap = src->surface;
    vk_eng_obj* d = vk_eng_get(snap, VK_ENG_TEXTURE);
    int fresh = 0;
    if (!d || d->w != src->w || d->h != src->h || d->fmt != src->fmt) {
        if (d) vk_eng_release(user, snap);
        src->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        d = &s_eobj[snap];
        if (vk_create_image(&d->im, src->fmt, src->w, src->h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&d->im); memset(d, 0, sizeof *d); return 0;
        }
        d->kind = VK_ENG_TEXTURE; d->fmt = src->fmt; d->bpp = src->bpp;
        d->w = src->w; d->h = src->h; d->surface = surface;
        src = vk_eng_get(surface, VK_ENG_COLOR);
        if (!src) { vk_eng_release(user, snap); return 0; }
        src->surface = snap;
        fresh = 1;
    }

    /* The copy runs after the draws already recorded for this frame, so the
     * snapshot holds the target as it stands at this point in the stream. */
    if (vk_begin()) return 0;
    vk_barrier_image(src->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(d->im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                     fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { src->w, src->h, 1 },
    };
    pvkCmdCopyImage(s_vk.cmd, src->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    d->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (vk_submit_and_wait()) return 0;
    return snap;
}
/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    if (s_vk.batch_open) s_vk.stat_f_snapshot++;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need a 32-bit float depth, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    if (w > z->gw) w = z->gw;
    if (h > z->gh) h = z->gh;
    if (!w || !h) return 0;
    /* Snapshot the guest region at the target's resolution, so sampling the
     * snapshot with normalised guest coordinates covers the same pixels. */
    if (z->w != z->gw || z->h != z->gh) {
        w = (u32)((u64)w * z->w / z->gw);
        h = (u32)((u64)h * z->h / z->gh);
        if (w > z->w) w = z->w;
        if (h > z->h) h = z->h;
    }

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h;
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        if (vk_batch_begin()) return 0;
        vk_batch_end_pass();
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    }

    /* Record the depth -> R32 copy in the frame's batch: the draws that
     * sample the snapshot come after it, so one submission sees them in
     * order. The intermediate lives in an arena until the submission. */
    { static int n = 0; if (depth == 1 && n < 200 && ++n) {
        vk_buffer st = {0};
        if (!vk_create_host_buffer_cached(&st, (VkDeviceSize)w * h * 4u,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT) && !vk_begin()) {
            VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                      .imageExtent = { w, h, 1 } };
            vk_barrier_image(z->im.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_GENERAL);
            pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
            if (!vk_submit_and_wait()) {
                vk_invalidate(&st);
                const float* f = (const float*)st.ptr;
                float lo = 1e30f, hi = -1e30f; double sum = 0.0;
                for (u32 i = 0; i < w * h; i++) { const float d = f[i];
                    if (d < lo) lo = d; if (d > hi) hi = d; sum += d; }
                fprintf(stderr, "[rsx vulkan] f%u depth snapshot %ux%u zeta %u: min %.6f max %.6f avg %.6f first %.6f\n",
                        s_vk.frame_index, w, h, depth, lo, hi, sum / (double)(w * h), w * h ? f[0] : 0.0f);
                static int dn = -1; static unsigned ring = 0;
                if (dn < 0) { const char* e = getenv("RSX_DUMP_DEPTH"); dn = e ? atoi(e) : 0; }
                if (dn > 0) {
                    char path[160];
                    snprintf(path, sizeof path, "/tmp/opencode/shadow_ring_%u.pgm", ring % 3u);
                    FILE* fp = fopen(path, "wb");
                    if (fp) {
                        fprintf(fp, "P5\n%u %u\n65535\n", w, h);
                        for (u32 i = 0; i < w * h; i++) {
                            float d = f[i]; if (!(d > 0.0f)) d = 0.0f; if (d > 1.0f) d = 1.0f;
                            const unsigned q = (unsigned)(d * 65535.0f + 0.5f);
                            fputc((q >> 8) & 255, fp); fputc(q & 255, fp);
                        }
                        fclose(fp);
                    }
                    ring++;
                }
            }
        }
        vk_destroy_buffer(&st);
    } }
    if (vk_batch_begin()) return 0;
    vk_batch_end_pass();
    VkDeviceSize snap_off = 0;
    if (vk_arena_alloc(&s_vk.batch_snap[s_vk.slot], &s_vk.batch_snap_end[s_vk.slot], NULL,
                       (VkDeviceSize)w * h * 4u,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       &snap_off))
        return 0;
    vk_barrier_image(z->im.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy out = { .bufferOffset = snap_off,
                              .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                              .imageExtent = { w, h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.batch_snap[s_vk.slot].buf, 1, &out);
    VkBufferMemoryBarrier bb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.batch_snap[s_vk.slot].buf, .offset = snap_off,
        .size = (VkDeviceSize)w * h * 4u };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 1, &bb, 0, NULL);
    VkBufferImageCopy in = { .bufferOffset = snap_off,
                             .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { w, h, 1 } };
    pvkCmdCopyBufferToImage(s_vk.cmd, s_vk.batch_snap[s_vk.slot].buf, t->im.img,
                            VK_IMAGE_LAYOUT_GENERAL, 1, &in);
    s_vk.stat_f_snapshot++;
    return snap;
}

/* The RSX's D24S8 word as it reads through an A8R8G8B8 texture:
 * R = depth[15:8], G = depth[7:0], B = stencil (0), A = depth[23:16].
 * Vulkan cannot copy the depth aspect into a colour image, so read the
 * requested top-left crop back through a host buffer, pack it, and upload
 * the bytes. The crop is texel for texel, matching eng_zdepth_copy_size. */
static u32 vk_eng_depth_snapshot_rgba8(void* user, u32 depth, u32 w, u32 h)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: packed depth snapshots need a 32-bit float depth, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    if (w > z->gw) w = z->gw;
    if (h > z->gh) h = z->gh;
    if (!w || !h) return 0;
    if (z->w != z->gw || z->h != z->gh) {
        w = (u32)((u64)w * z->w / z->gw);
        h = (u32)((u64)h * z->h / z->gh);
        if (w > z->w) w = z->w;
        if (h > z->h) h = z->h;
    }
    if (!w || !h) return 0;

    const u32 snap = vk_eng_alloc();
    if (!snap) return 0;
    vk_eng_obj* t = &s_eobj[snap];
    if (vk_create_image(&t->im, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
    }
    t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R8G8B8A8_UNORM; t->bpp = 4; t->w = w; t->h = h;

    vk_buffer st = {0};
    if (vk_create_host_buffer_cached(&st, (VkDeviceSize)w * h * 4u,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
    }

    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        rc = vk_submit_and_wait();
    }
    if (rc) { vk_destroy_buffer(&st); vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0; }
    vk_invalidate(&st);

    const float* f = (const float*)st.ptr;
    { float lo = 1e30f, hi = -1e30f; double sum = 0.0;
      for (u32 i = 0; i < w * h; i++) {
          const float d = f[i];
          if (d < lo) lo = d;
          if (d > hi) hi = d;
          sum += d;
      }
      static int n = 0; if (n < 8 && ++n)
          fprintf(stderr, "[rsx vulkan] packed depth %ux%u: min %.6f max %.6f avg %.6f first %.6f\n",
                  w, h, lo, hi, sum / (double)(w * h), w * h ? f[0] : 0.0f); }
    u8* p = (u8*)st.ptr;
    for (u32 i = 0; i < w * h; i++) {
        float d = f[i];
        if (!(d > 0.0f)) d = 0.0f; else if (d > 1.0f) d = 1.0f;
        const u32 v = (u32)(d * 16777215.0f + 0.5f);
        p[0] = (u8)((v >> 8) & 255u);
        p[1] = (u8)(v & 255u);
        p[2] = 0;
        p[3] = (u8)((v >> 16) & 255u);
        p += 4;
    }
    { static int pn = -1; if (pn < 0) { const char* e = getenv("RSX_DUMP_PACKED"); pn = e ? atoi(e) : 0; }
      if (pn > 0) {
          char path[160];
          snprintf(path, sizeof path, "/tmp/opencode/packed_%02d.rgba", 100 - pn);
          FILE* fp = fopen(path, "wb");
          if (fp) { fwrite(st.ptr, 1, (size_t)w * h * 4u, fp); fclose(fp); }
          pn--;
      } }
    vk_flush(&st);
    rc = vk_begin();
    if (!rc) {
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&st);
    if (rc) { vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0; }
    return snap;
}

/* ---- E2: pipelines and draws --------------------------------------------- */

/*
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), FP16 and other
 * non-RGBA8 colour targets, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
#define VK_ENG_MAX_PIPE   512
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   1024

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs, fs_scaled;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
    VkPipeline          variant_scaled[VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

/* Colour targets the engine can render into. The display image is always
 * RGBA8; HDR titles bind an FP16 target first and resolve through
 * vk_eng_present's blit. */
static const VkFormat vk_eng_rt_formats[] = {
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R32G32B32A32_SFLOAT,
};
#define VK_ENG_RT_CLASSES ((u32)(sizeof vk_eng_rt_formats / sizeof vk_eng_rt_formats[0]))

static int vk_eng_rt_class(VkFormat f)
{
    for (u32 i = 0; i < VK_ENG_RT_CLASSES; i++)
        if (vk_eng_rt_formats[i] == f) return (int)i;
    return -1;
}

static struct {
    VkRenderPass rp[VK_ENG_RT_CLASSES][RSX_BE_MAX_COLOR_TARGETS + 1]; /* [format][count] */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_cb_off, ps_cb_off;
    u32          vs_bytes, ps_bytes;
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    vk_buffer    indices;
    vk_buffer    zero_constants;
    u32          warned;
} s_e2;

typedef struct vk_eng_sampler_key {
    u8 min_linear, mag_linear, mip_linear, mip_present;
    u8 wrap_s, wrap_t, wrap_r;
    u32 min_lod_bits, max_lod_bits;
} vk_eng_sampler_key;

#define VK_ENG_SAMP_CACHE 64
static struct { vk_eng_sampler_key key; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;

static int vk_eng2_init(void)
{
    if (!s_vk.device) return -1;
    memset(&s_e2, 0, sizeof s_e2);
    if (vk_create_host_buffer(&s_e2.zero_constants, 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !s_e2.zero_constants.coherent) {
        vk_destroy_buffer(&s_e2.zero_constants);
        return -1;
    }
    memset(s_e2.zero_constants.ptr, 0, 16u);
    return 0;
}

static void vk_eng_drop_framebuffers(void)
{
    for (u32 i = 0; i < s_e2.fb_count; i++)
        vk_pending_framebuffer(s_e2.fb[i].fb);
    s_e2.fb_count = 0;
}

/* The render pass for `n` colour attachments of `fmt` plus depth, LOAD/STORE
 * in GENERAL like the vtable path's: clears are separate operations. */
static VkRenderPass vk_eng_render_pass(u32 n, VkFormat fmt)
{
    if (n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    const int fc = vk_eng_rt_class(fmt);
    if (fc < 0) return VK_NULL_HANDLE;
    if (s_e2.rp[fc][n]) return s_e2.rp[fc][n];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? fmt : s_vk.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* The stencil is a real buffer here (see vk_pick_depth_format): a
             * pass keeps what earlier passes wrote to it, as the title's
             * masked effects and shadow volumes expect. */
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp[fc][n]) != VK_SUCCESS)
        s_e2.rp[fc][n] = VK_NULL_HANDLE;
    return s_e2.rp[fc][n];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

/* Vulkan asks that a device's create calls not run into each other, and on
 * this backend pipeline_create is the engine's worker-thread entry point
 * (RSX_ASYNC_SHADERS): the lock is held for the call only, never for the
 * translate and patch work around it, so workers still build in parallel. */
static pthread_mutex_t s_eng_vk_mu = PTHREAD_MUTEX_INITIALIZER;

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out)
{
    u32* words = NULL;
    char log[4096];
    log[0] = '\0';
    const u32 n = rsx_hlsl_to_spirv(hlsl, stage, &words, log, sizeof log);
    if (!n || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SPV_STAGE_VERTEX ? "vertex" : "fragment", log);
        free(words);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = words };
    pthread_mutex_lock(&s_eng_vk_mu);
    const VkResult r = pvkCreateShaderModule(s_vk.device, &ci, NULL, out);
    pthread_mutex_unlock(&s_eng_vk_mu);
    free(words);
    return r == VK_SUCCESS ? 0 : -1;
}

/* RSX_SCALE: WPOS arrives as gl_FragCoord in host pixels, but the guest
 * shader expects guest pixels. Rewrite the generated HLSL's WPOS reads to
 * divide by the scale, then compile a second fragment module for draws into
 * scaled targets. Shaders without WPOS need no variant. */
static char* vk_eng_patch_wpos(const char* src)
{
    static const char needle[] = "input.position";
    size_t count = 0;
    for (const char* p = src; (p = strstr(p, needle)) != NULL; p += sizeof needle - 1)
        count++;
    if (!count) return NULL;
    char repl[128];
    const float inv = 1.0f / s_vk.scale;
    snprintf(repl, sizeof repl,
             "(input.position * float4(%.8ff, %.8ff, 1.0f, 1.0f))", inv, inv);
    const size_t nl = sizeof needle - 1, rl = strlen(repl);
    char* out = (char*)malloc(strlen(src) + count * (rl - nl) + 1);
    if (!out) return NULL;
    char* dst = out;
    const char* p = src;
    for (;;) {
        const char* q = strstr(p, needle);
        if (!q) { strcpy(dst, p); break; }
        memcpy(dst, p, (size_t)(q - p)); dst += q - p;
        memcpy(dst, repl, rl); dst += rl;
        p = q + nl;
    }
    return out;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx, int scaled);

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 bpp = 0;
    const VkFormat vf = vk_eng_format(rt_fmt, &bpp);
    if (vk_eng_rt_class(vf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    /* Workers build in parallel: take the slot under the same lock, and
     * leave `used` set so no other build takes it. The handle is not handed
     * to the engine until this returns it. */
    u32 slot = VK_ENG_MAX_PIPE;
    pthread_mutex_lock(&s_eng_vk_mu);
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot != VK_ENG_MAX_PIPE) { memset(&s_epipe[slot], 0, sizeof s_epipe[slot]); s_epipe[slot].used = 1; }
    pthread_mutex_unlock(&s_eng_vk_mu);
    if (slot == VK_ENG_MAX_PIPE) return 0;
    { static const char* d = (const char*)1;
      if (d == (const char*)1) d = getenv("RSX_HLSL_DUMP_DIR");
      if (d) {
          char fn[512];
          snprintf(fn, sizeof fn, "%s/pipe_%u_vs.hlsl", d, slot);
          FILE* f = fopen(fn, "wb"); if (f) { fputs(vs_hlsl, f); fclose(f); }
          snprintf(fn, sizeof fn, "%s/pipe_%u_ps.hlsl", d, slot);
          f = fopen(fn, "wb"); if (f) { fputs(ps_hlsl, f); fclose(f); }
      } }
    vk_eng_pipe* P = &s_epipe[slot];
    if (vk_eng_translate(vs_hlsl, RSX_SPV_STAGE_VERTEX, &P->vs)) { P->used = 0; return 0; }
    if (vk_eng_translate(ps_hlsl, RSX_SPV_STAGE_FRAGMENT, &P->fs)) {
        pvkDestroyShaderModule(s_vk.device, P->vs, NULL); P->used = 0; return 0;
    }
    if (s_vk.scale != 1.0f) {
        char* patched = vk_eng_patch_wpos(ps_hlsl);
        if (patched) {
            if (vk_eng_translate(patched, RSX_SPV_STAGE_FRAGMENT, &P->fs_scaled))
                VK_LOG("engine: scaled WPOS fragment variant failed\n");
            free(patched);
        }
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = vf;
    /* Vulkan bakes the topology into a pipeline, which the engine's key does
     * not carry, so the first draw of a new material used to compile its
     * VkPipeline on the FIFO walker -- the 60-100 ms stalls this backend's
     * stage loads were made of. Build the common topology here instead: this
     * runs on the engine's worker thread, off the walker. */
    (void)vk_eng_variant(P, RSX_TOPOLOGY_TRIANGLES - 1u, 0);
    if (P->fs_scaled) (void)vk_eng_variant(P, RSX_TOPOLOGY_TRIANGLES - 1u, 1);
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    if (s_vk.batch_open) s_vk.stat_f_pipe++;
    (void)vk_batch_flush();
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    pthread_mutex_lock(&s_eng_vk_mu);
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++) {
        if (P->variant[t]) pvkDestroyPipeline(s_vk.device, P->variant[t], NULL);
        if (P->variant_scaled[t]) pvkDestroyPipeline(s_vk.device, P->variant_scaled[t], NULL);
    }
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    if (P->fs_scaled) pvkDestroyShaderModule(s_vk.device, P->fs_scaled, NULL);
    memset(P, 0, sizeof *P);
    pthread_mutex_unlock(&s_eng_vk_mu);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx, int scaled)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (scaled && !P->fs_scaled) scaled = 0;
    VkPipeline* slot = scaled ? &P->variant_scaled[topo_idx] : &P->variant[topo_idx];
    if (*slot) return *slot;
    /* Render passes and pipelines are device objects: fold a worker's build
     * into the walker's (see s_eng_vk_mu). A built variant never takes it. */
    pthread_mutex_lock(&s_eng_vk_mu);
    if (*slot) { pthread_mutex_unlock(&s_eng_vk_mu); return *slot; }
    VkRenderPass rp = vk_eng_render_pass(P->rt_count, P->rt_fmt);
    if (!rp || P->nslots > VK_GUEST_ATTRS) { pthread_mutex_unlock(&s_eng_vk_mu); return VK_NULL_HANDLE; }
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = scaled ? P->fs_scaled : P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back };
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Every attachment takes target A's blend and mask (see rsx_draw_engine.h). */
    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 r = 0; r < P->rt_count; r++)
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = rs->blend_enable ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = wm };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = s_g.pipe_layout, .renderPass = rp, .subpass = 0 };
    if (pvkCreateGraphicsPipelines(s_vk.device, VK_NULL_HANDLE, 1, &gci, NULL,
                                   slot) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        *slot = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&s_eng_vk_mu);
    return *slot;
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    (void)vk_batch_flush();
    vk_pending_drain(0, 1);
    for (u32 f = 0; f < VK_ENG_RT_CLASSES; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (s_e2.rp[f][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[f][n], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    vk_destroy_buffer(&s_e2.zero_constants);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Constants for a draw go into a slice of the frame's UBO arena. The
 * engine's bind calls and the draw that follows are adjacent, and every
 * slice stays valid until the frame is submitted. */
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_cb_off = 0; s_e2.vs_bytes = 0; return; }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    VkDeviceSize off = 0;
    if (vk_arena_alloc(&s_vk.batch_ubo[s_vk.slot], &s_vk.batch_ubo_end[s_vk.slot], data, bytes,
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &off)) {
        s_e2.vs_cb_off = 0; s_e2.vs_bytes = 0; return;
    }
    s_e2.vs_cb_off = (u32)off;
    s_e2.vs_bytes = bytes;
    s_vk.stat_ubo += bytes;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_cb_off = 0; s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    VkDeviceSize off = 0;
    if (vk_arena_alloc(&s_vk.batch_ubo[s_vk.slot], &s_vk.batch_ubo_end[s_vk.slot], data, bytes,
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &off)) {
        s_e2.ps_cb_off = 0; s_e2.ps_bytes = 0; return;
    }
    s_e2.ps_cb_off = (u32)off;
    s_e2.ps_bytes = bytes;
    s_vk.stat_ubo += bytes;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user; (void)textures; (void)samplers;
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures not supported yet\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffers();
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    s_vk.stat_fb++;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    (void)vk_batch_flush();
    vk_pending_drain(0, 1);
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        vk_depth_aspects()) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { vk_depth_aspects(), 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    vk_eng_sampler_key key = {
        .min_linear = d->min_linear, .mag_linear = d->mag_linear,
        .mip_linear = d->mip_linear, .mip_present = d->mip_present,
        .wrap_s = d->wrap_s, .wrap_t = d->wrap_t, .wrap_r = d->wrap_r,
    };
    memcpy(&key.min_lod_bits, &d->min_lod, sizeof key.min_lod_bits);
    memcpy(&key.max_lod_bits, &d->max_lod, sizeof key.max_lod_bits);
    for (u32 i = 0; i < s_esamp_count; i++) {
        const vk_eng_sampler_key* cached = &s_esamp[i].key;
        if (cached->min_linear == key.min_linear && cached->mag_linear == key.mag_linear &&
            cached->mip_linear == key.mip_linear && cached->mip_present == key.mip_present &&
            cached->wrap_s == key.wrap_s && cached->wrap_t == key.wrap_t &&
            cached->wrap_r == key.wrap_r && cached->min_lod_bits == key.min_lod_bits &&
            cached->max_lod_bits == key.max_lod_bits)
            return s_esamp[i].s;
    }
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].key = key;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view. */
static VkImageView vk_eng_sample_view(u32 handle)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == VK_ENG_TEXTURE || o->kind == VK_ENG_COLOR || o->kind == VK_ENG_VIEW)
               ? o->im.view : VK_NULL_HANDLE;
}

static void vk_eng_write_descriptors(VkDescriptorSet set, u32 vs_off, u32 vs_bytes,
                                     u32 ps_off, u32 ps_bytes)
{
    VkBuffer ub = s_vk.batch_ubo[s_vk.slot].buf;
    VkDescriptorBufferInfo bi[2] = {
        { vs_bytes ? ub : s_e2.zero_constants.buf, vs_bytes ? vs_off : 0,
          vs_bytes ? vs_bytes : 16u },
        { ps_bytes ? ub : s_e2.zero_constants.buf, ps_bytes ? ps_off : 0,
          ps_bytes ? ps_bytes : 16u },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u]) : VK_NULL_HANDLE;
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, v ? v : s_vk.dummy.view, VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPV_BIND_VS_CB, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPV_BIND_FS_CB, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPV_BIND_FS_TEX, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPV_BIND_FS_SAMP, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);
}

static float vk_eng_bound_scale(void)
{
    vk_eng_obj* rt = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt || !rt->gw) return 1.0f;
    return (float)rt->w / (float)rt->gw;
}

static double vk_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void vk_eng_do_draw(void* user, rsx_topology topology,
                           const void* vertices, VkBuffer vbuf, VkDeviceSize vb_off,
                           u32 vertex_count, u32 stride,
                           const u32* indices, VkBuffer ibuf, VkDeviceSize ib_off,
                           u32 index_count)
{
    (void)user;
    if (!s_e2.pipeline || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0 || rt0->fmt != P->rt_fmt) return;
    for (u32 i = 1; i < n; i++) {
        vk_eng_obj* rti = vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR);
        if (!rti || rti->fmt != P->rt_fmt) return;
    }
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    const int scaled = rt0->w != rt0->gw || rt0->h != rt0->gh;
    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u, scaled);
    VkRenderPass rp = vk_eng_render_pass(n, P->rt_fmt);
    VkFramebuffer fb = vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) return;

    const int indexed = index_count != 0;
    const double t0 = vk_now_ms();
    if (vertices) {
        /* Stage both buffers before any arena reset can invalidate an offset
         * already chosen for this draw. */
        if (indexed &&
            vk_arena_alloc(&s_vk.batch_ibuf[s_vk.slot], &s_vk.batch_ibuf_end[s_vk.slot], indices,
                           (VkDeviceSize)index_count * 4u, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &ib_off))
            return;
        if (vk_arena_alloc(&s_vk.batch_vbuf[s_vk.slot], &s_vk.batch_vbuf_end[s_vk.slot], vertices,
                           (VkDeviceSize)vertex_count * stride, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb_off))
            return;
        vbuf = s_vk.batch_vbuf[s_vk.slot].buf;
        ibuf = s_vk.batch_ibuf[s_vk.slot].buf;
    } else if (!vbuf || (indexed && !ibuf)) {
        return;
    }
    const double t1 = vk_now_ms();

    VkDescriptorSet set = vk_batch_descriptor_set();
    if (!set) return;
    s_vk.stat_draws++;
    vk_eng_write_descriptors(set, s_e2.vs_cb_off, s_e2.vs_bytes,
                              s_e2.ps_cb_off, s_e2.ps_bytes);

    const double t2 = vk_now_ms();
    if (vk_batch_begin_pass(rp, fb, w, h)) return;
    const double t3 = vk_now_ms();
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* D3D clip space from the decompilers (y up), flipped by a negative
     * viewport height: the engine's viewport has its origin at the top. */
    const float sf = vk_eng_bound_scale();
    VkViewport vp = { 0.0f, (float)h, (float)w, -(float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0] * sf; vp.y = (s_e2.vp[1] + s_e2.vp[3]) * sf;
        vp.width = s_e2.vp[2] * sf; vp.height = -s_e2.vp[3] * sf;
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 x0 = (u32)((float)s_e2.sc[0] * sf), y0 = (u32)((float)s_e2.sc[1] * sf);
        u32 x1 = (u32)((float)(s_e2.sc[0] + s_e2.sc[2]) * sf);
        u32 y1 = (u32)((float)(s_e2.sc[1] + s_e2.sc[3]) * sf);
        if (x0 > w) x0 = w;
        if (y0 > h) y0 = h;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vbuf, &vb_off);
    if (indexed) {
        pvkCmdBindIndexBuffer(s_vk.cmd, ibuf, ib_off, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    const double t4 = vk_now_ms();
    s_vk.tm_stage += t1 - t0;
    s_vk.tm_set += t2 - t1;
    s_vk.tm_pass += t3 - t2;
    s_vk.tm_rec += t4 - t3;
}

static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    vk_eng_do_draw(user, topology, vertices, VK_NULL_HANDLE, 0, vertex_count, stride,
                   indices, VK_NULL_HANDLE, 0, index_count);
}

/* ---- vertex/index buffers the engine's cache keeps across frames ---------
 * buffer_wrap takes ownership of the engine's page-aligned copy: the memory
 * is copied once into a host-visible buffer, and draw_buffer then reads it
 * in place instead of staging every draw again. */
static struct { VkBuffer buf; VkDeviceMemory mem; } s_ewrap[VK_ENG_WRAP_MAX];

static u32 vk_eng_buffer_wrap(void* user, void* data, u32 bytes)
{
    (void)user;
    if (!data || !bytes) return 0;
    u32 slot = 0;
    for (u32 i = 1; i < VK_ENG_WRAP_MAX; i++)
        if (!s_ewrap[i].buf) { slot = i; break; }
    if (!slot) return 0;
    vk_buffer b = {0};
    if (vk_create_host_buffer(&b, (VkDeviceSize)bytes,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        !b.coherent) {
        vk_destroy_buffer(&b);
        return 0;
    }
    memcpy(b.ptr, data, bytes);
    s_ewrap[slot].buf = b.buf;
    s_ewrap[slot].mem = b.mem;
    free(data);
    return slot;
}

static void vk_eng_buffer_release(void* user, u32 buffer)
{
    (void)user;
    if (buffer >= VK_ENG_WRAP_MAX || !s_ewrap[buffer].buf) return;
    vk_buffer b = { .buf = s_ewrap[buffer].buf, .mem = s_ewrap[buffer].mem };
    vk_pending_buffer(&b);
    memset(&s_ewrap[buffer], 0, sizeof s_ewrap[buffer]);
}

static void vk_eng_draw_buffer(void* user, rsx_topology topology, u32 buffer,
                               u32 vb_off, u32 vertex_count, u32 stride,
                               u32 ib_off, u32 index_count)
{
    if (buffer >= VK_ENG_WRAP_MAX || !s_ewrap[buffer].buf) return;
    vk_eng_do_draw(user, topology, NULL, s_ewrap[buffer].buf, vb_off, vertex_count, stride,
                   NULL, s_ewrap[buffer].buf, ib_off, index_count);
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = vk_eng_init,
    .shutdown             = vk_eng_shutdown,
    .submit_and_wait      = vk_eng_submit_and_wait,
    .texture_create       = vk_eng_texture_create,
    .texture_upload       = vk_eng_texture_upload,
    .texture_release      = vk_eng_release,
    .color_target_create  = vk_eng_color_target_create,
    .color_target_release = vk_eng_release,
    .surface_view         = vk_eng_surface_view,
    .depth_target_create  = vk_eng_depth_target_create,
    .depth_target_release = vk_eng_release,
    .depth_snapshot       = vk_eng_depth_snapshot,
    .depth_snapshot_rgba8 = vk_eng_depth_snapshot_rgba8,
    .color_snapshot       = vk_eng_color_snapshot,
    .pipeline_create      = vk_eng_pipeline_create,
    .pipeline_release     = vk_eng_pipeline_release,
    .bind_targets         = vk_eng_bind_targets,
    .bind_pipeline        = vk_eng_bind_pipeline,
    .bind_vs_constants    = vk_eng_bind_vs_constants,
    .bind_ps_constants    = vk_eng_bind_ps_constants,
    .bind_textures        = vk_eng_bind_textures,
    .bind_vertex_textures = vk_eng_bind_vertex_textures,
    .set_viewport         = vk_eng_set_viewport,
    .set_scissor          = vk_eng_set_scissor,
    .set_stencil_ref      = vk_eng_set_stencil_ref,
    .draw                 = vk_eng_draw,
    .buffer_wrap          = vk_eng_buffer_wrap,
    .buffer_release       = vk_eng_buffer_release,
    .draw_buffer          = vk_eng_draw_buffer,
    .clear_color          = vk_eng_clear_color,
    .clear_depth_stencil  = vk_eng_clear_depth_stencil,
    .present              = vk_eng_present,
    .readback             = vk_eng_readback,
};

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = vk_cb_track_state,
    .set_vertex_attribs = vk_cb_track_state,
    .set_depth_stencil  = vk_cb_track_state,
    .clear              = vk_cb_clear,
    .draw_arrays        = vk_cb_draw_arrays,
    .draw_indexed       = vk_cb_draw_indexed,
    .bind_texture       = vk_cb_bind_texture,
    .present            = vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    vk_read_scale();
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir == VK_ERROR_INCOMPATIBLE_DRIVER) {         /* a Vulkan 1.0 loader */
        app.apiVersion = VK_API_VERSION_1_0;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    s_vk_api = app.apiVersion;
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (!SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface)) {
            char why[256];
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    static const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    VK_CHECK(pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = VK_SLOTS };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, s_vk.cmds), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (u32 slot = 0; slot < VK_SLOTS; slot++)
        if (pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fences[slot]) != VK_SUCCESS)
            { VK_LOG("vkCreateFence failed\n"); return -1; }
    s_vk.cmd = s_vk.cmds[0];
    s_vk.fence = s_vk.fences[0];

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.out_width, s_vk.out_height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.out_width, s_vk.out_height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, vk_depth_aspects())) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.out_width * s_vk.out_height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, vk_depth_aspects(), VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { vk_depth_aspects(), 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed && vk_swapchain_make())
        vk_window_drop("the swapchain could not be created");
    return 0;
}

int rsx_vulkan_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else
        rsx_set_backend(&s_vulkan_backend);
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    VK_LOG("offscreen target %ux%u -> %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, s_vk.out_width, s_vk.out_height,
           (int)s_vk.depth_format, title ? title : "");
    return 0;
}

void rsx_vulkan_backend_shutdown(void)
{
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    (void)vk_batch_flush();
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const Uint32 end = SDL_GetTicks() + (Uint32)(s_vk.hold_seconds * 1000.0);
        while (s_vk.windowed && !SDL_TICKS_PASSED(SDL_GetTicks(), end)) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_frame();          /* FIFO: paced by the display */
        }
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        if (pvkDeviceWaitIdle) pvkDeviceWaitIdle(s_vk.device);
        if (s_vk.batch_pool) {
            pvkDestroyDescriptorPool(s_vk.device, s_vk.batch_pool, NULL);
            s_vk.batch_pool = VK_NULL_HANDLE;
            s_vk.batch_sets = 0;
        }
        vk_guest_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        for (u32 slot = 0; slot < VK_SLOTS; slot++) {
            vk_destroy_buffer(&s_vk.batch_vbuf[slot]);
            vk_destroy_buffer(&s_vk.batch_ibuf[slot]);
            vk_destroy_buffer(&s_vk.batch_ubo[slot]);
            vk_destroy_buffer(&s_vk.batch_snap[slot]);
            vk_destroy_buffer(&s_vk.batch_tex[slot]);
        }
        for (u32 slot = 0; slot < VK_SLOTS; slot++)
            if (s_vk.fences[slot]) pvkDestroyFence(s_vk.device, s_vk.fences[slot], NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
    if (s_vk.lib) dlclose(s_vk.lib);
    free(s_vk.cpu_verts);
    memset(&s_vk, 0, sizeof s_vk);
}

int rsx_vulkan_backend_pump_messages(void)
{
    if (!s_vk.windowed) return 0;
    if (g_rsx_display_reload) vk_display_reload();
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
        else if (e.type == SDL_WINDOWEVENT &&
                 (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                  e.window.event == SDL_WINDOWEVENT_RESIZED) &&
                 s_vk.display_mode == VK_DISP_WINDOWED) {
            s_vk.win_w = (u32)e.window.data1;
            s_vk.win_h = (u32)e.window.data2;
            s_vk.resize_pending = 1;
        }
    }
    if (s_vk.resize_pending) {
        s_vk.resize_pending = 0;
        if (s_vk.device && s_vk.swapchain) {
            vk_swapchain_destroy();
            if (vk_swapchain_make()) VK_LOG("could not recreate the swapchain after resize\n");
        }
    }
    return quit ? -1 : 0;
}

void rsx_vulkan_backend_present(void)
{
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

u32 rsx_vulkan_backend_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

