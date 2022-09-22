/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "command.h"
#include "formats.h"
#include "utils.h"
#include "gpu.h"
#include "swapchain.h"

struct sem_pair {
    VkSemaphore in;
    VkSemaphore out;
};

struct priv {
    struct vk_ctx *vk;
    VkSurfaceKHR surf;

    // current swapchain and metadata:
    struct pl_vulkan_swapchain_params params;
    VkSwapchainCreateInfoKHR protoInfo; // partially filled-in prototype
    VkSwapchainKHR swapchain;
    VkSwapchainKHR old_swapchain;
    int cur_width, cur_height;
    int swapchain_depth;
    int frames_in_flight;   // number of frames currently queued
    bool suboptimal;        // true once VK_SUBOPTIMAL_KHR is returned
    struct pl_color_repr color_repr;
    struct pl_color_space color_space;
    struct pl_hdr_metadata hdr_metadata;

    // state of the images:
    PL_ARRAY(const struct pl_tex *) images; // pl_tex wrappers for the VkImages
    PL_ARRAY(struct sem_pair) sems;         // pool of semaphores used to synchronize images
    int idx_sems;                           // index of next free semaphore pair
    int last_imgidx;                        // the image index last acquired (for submit)
};

static struct pl_sw_fns vulkan_swapchain;

static bool vk_map_color_space(VkColorSpaceKHR space, struct pl_color_space *out)
{
    switch (space) {
    // Note: This is technically against the spec, but more often than not
    // it's the correct result since `SRGB_NONLINEAR` is just a catch-all
    // for any sort of typical SDR curve, which is better approximated by
    // `pl_color_space_monitor`.
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        *out = pl_color_space_monitor;
        return true;

#ifdef VK_EXT_swapchain_colorspace
    case VK_COLOR_SPACE_BT709_NONLINEAR_EXT:
        *out = pl_color_space_monitor;
        return true;
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_DISPLAY_P3,
            .transfer  = PL_COLOR_TRC_BT_1886,
        };
        return true;
    case VK_COLOR_SPACE_DCI_P3_LINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_DCI_P3,
            .transfer  = PL_COLOR_TRC_LINEAR,
        };
        return true;
    case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_DCI_P3,
            .transfer  = PL_COLOR_TRC_BT_1886,
        };
        return true;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
    case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT:
        // TODO
        return false;
    case VK_COLOR_SPACE_BT709_LINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_DCI_P3,
            .transfer  = PL_COLOR_TRC_LINEAR,
        };
        return true;
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_BT_2020,
            .transfer  = PL_COLOR_TRC_LINEAR,
        };
        return true;
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_BT_2020,
            .transfer  = PL_COLOR_TRC_PQ,
        };
        return true;
    case VK_COLOR_SPACE_DOLBYVISION_EXT:
        // Unlikely to ever be implemented
        return false;
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_BT_2020,
            .transfer  = PL_COLOR_TRC_HLG,
        };
        return true;
    case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_ADOBE,
            .transfer  = PL_COLOR_TRC_LINEAR,
        };
        return true;
    case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT:
        *out = (struct pl_color_space) {
            .primaries = PL_COLOR_PRIM_ADOBE,
            .transfer  = PL_COLOR_TRC_GAMMA22,
        };
        return true;
    case VK_COLOR_SPACE_PASS_THROUGH_EXT:
        *out = pl_color_space_unknown;
        return true;
#endif

#ifdef VK_AMD_display_native_hdr
    case VK_COLOR_SPACE_DISPLAY_NATIVE_AMD:
        // TODO
        return false;
#endif

    default: return false;
    }
}

static bool pick_surf_format(const struct pl_gpu *gpu, const struct vk_ctx *vk,
                             VkSurfaceKHR surf, bool prefer_hdr,
                             VkSurfaceFormatKHR *out_format,
                             struct pl_color_space *out_space)
{
    int best_score = 0, best_id;
    VkSurfaceFormatKHR *formats = NULL;
    int num = 0;

    // Specific format requested by user
    if (out_format->format) {
        if (vk_map_color_space(out_format->colorSpace, out_space)) {
            PL_INFO(gpu, "Using user-supplied surface configuration: %s + %s",
                    vk_fmt_name(out_format->format),
                    vk_csp_name(out_format->colorSpace));
            return true;
        } else {
            PL_ERR(gpu, "User-supplied surface format unsupported: %s",
                   vk_fmt_name(out_format->format));
        }
    }

    VK(vk->GetPhysicalDeviceSurfaceFormatsKHR(vk->physd, surf, &num, NULL));
    formats = pl_calloc_ptr(NULL, num, formats);
    VK(vk->GetPhysicalDeviceSurfaceFormatsKHR(vk->physd, surf, &num, formats));

    PL_INFO(gpu, "Available surface configurations:");
    for (int i = 0; i < num; i++) {
        PL_INFO(gpu, "    %d: %-40s %s", i,
                vk_fmt_name(formats[i].format),
                vk_csp_name(formats[i].colorSpace));
    }

    for (int i = 0; i < num; i++) {
        // A value of VK_FORMAT_UNDEFINED means we can pick anything we want
        if (formats[i].format == VK_FORMAT_UNDEFINED) {
            *out_format = (VkSurfaceFormatKHR) {
                .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
            };
            pl_free(formats);
            return true;
        }

        // Color space / format whitelist
        struct pl_color_space space;
        if (!vk_map_color_space(formats[i].colorSpace, &space))
            continue;

        switch (formats[i].format) {
        // Only accept floating point formats for linear curves
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R64G64B64_SFLOAT:
        case VK_FORMAT_R64G64B64A64_SFLOAT:
            if (space.transfer == PL_COLOR_TRC_LINEAR)
                break; // accept
            continue;

        // Only accept 8 bit for non-HDR curves
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_B8G8R8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
            if (!pl_color_transfer_is_hdr(space.transfer))
                break; // accept
            continue;

        // Only accept 10 bit formats for non-linear curves
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            if (space.transfer != PL_COLOR_TRC_LINEAR)
                break; // accept
            continue;

        // Accept 16-bit formats for everything
        case VK_FORMAT_R16G16B16_UNORM:
        case VK_FORMAT_R16G16B16A16_UNORM:
             break; // accept

        default: continue;
        }

        // Make sure we can wrap this format to a meaningful, valid pl_fmt
        for (int n = 0; n < gpu->num_formats; n++) {
            const struct pl_fmt *plfmt = gpu->formats[n];
            const struct vk_format **pvkfmt = PL_PRIV(plfmt);
            if ((*pvkfmt)->tfmt != formats[i].format)
                continue;

            enum pl_fmt_caps render_caps = 0;
            render_caps |= PL_FMT_CAP_RENDERABLE;
            render_caps |= PL_FMT_CAP_BLITTABLE;
            if ((plfmt->caps & render_caps) != render_caps)
                continue;

            // format valid, use it if it has a higher score
            int score = 0;
            for (int c = 0; c < 3; c++)
                score += plfmt->component_depth[c];
            if (pl_color_transfer_is_hdr(space.transfer) == prefer_hdr)
                score += 10000;

            switch (plfmt->type) {
            case PL_FMT_UNORM: score += 3000; break;
            case PL_FMT_SNORM: score += 2000; break;
            case PL_FMT_FLOAT: score += 1000; break;
            default: break;
            };

            if (score > best_score) {
                *out_format = formats[i];
                *out_space = space;
                best_score = score;
                best_id = i;
                break;
            }
        }
    }

    if (best_score)
        PL_INFO(gpu, "Picked surface configuration %d", best_id);

    // fall through
error:
    if (!best_score)
        PL_FATAL(vk, "Failed picking any valid, renderable surface format!");
    pl_free(formats);
    return best_score > 0;
}

const struct pl_swapchain *pl_vulkan_create_swapchain(const struct pl_vulkan *plvk,
                              const struct pl_vulkan_swapchain_params *params)
{
    struct vk_ctx *vk = PL_PRIV(plvk);
    const struct pl_gpu *gpu = plvk->gpu;

    if (!vk->CreateSwapchainKHR) {
        PL_ERR(gpu, VK_KHR_SWAPCHAIN_EXTENSION_NAME " not enabled!");
        return NULL;
    }

    VkSurfaceFormatKHR sfmt = params->surface_format;
    struct pl_color_space csp;
    if (!pick_surf_format(gpu, vk, params->surface, params->prefer_hdr, &sfmt, &csp))
        return NULL;

    struct pl_swapchain *sw = pl_zalloc_priv(NULL, struct pl_swapchain, struct priv);
    sw->impl = &vulkan_swapchain;
    sw->ctx = vk->ctx;
    sw->gpu = gpu;

    struct priv *p = PL_PRIV(sw);
    p->params = *params;
    p->vk = vk;
    p->surf = params->surface;
    p->swapchain_depth = PL_DEF(params->swapchain_depth, 3);
    pl_assert(p->swapchain_depth > 0);
    p->protoInfo = (VkSwapchainCreateInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = p->surf,
        .imageFormat = sfmt.format,
        .imageColorSpace = sfmt.colorSpace,
        .imageArrayLayers = 1, // non-stereoscopic
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .minImageCount = p->swapchain_depth + 1, // +1 for the FB
        .presentMode = params->present_mode,
        .clipped = true,
    };

    p->color_space = csp;
    p->color_repr = (struct pl_color_repr) {
        .sys    = PL_COLOR_SYSTEM_RGB,
        .levels = PL_COLOR_LEVELS_FULL,
        .alpha  = PL_ALPHA_UNKNOWN, // will be updated by vk_sw_recreate
    };

    // Make sure the swapchain present mode is supported
    VkPresentModeKHR *modes = NULL;
    int num_modes = 0;
    VK(vk->GetPhysicalDeviceSurfacePresentModesKHR(vk->physd, p->surf, &num_modes, NULL));
    modes = pl_calloc_ptr(NULL, num_modes, modes);
    VK(vk->GetPhysicalDeviceSurfacePresentModesKHR(vk->physd, p->surf, &num_modes, modes));

    bool supported = false;
    for (int i = 0; i < num_modes; i++)
        supported |= (modes[i] == p->protoInfo.presentMode);
    pl_free_ptr(&modes);

    if (!supported) {
        PL_WARN(vk, "Requested swap mode unsupported by this device, falling "
                "back to VK_PRESENT_MODE_FIFO_KHR");
        p->protoInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }

    return sw;

error:
    pl_free(modes);
    pl_free(sw);
    return NULL;
}

static void vk_sw_destroy(const struct pl_swapchain *sw)
{
    const struct pl_gpu *gpu = sw->gpu;
    struct priv *p = PL_PRIV(sw);
    struct vk_ctx *vk = p->vk;

    pl_gpu_flush(gpu);
    vk_wait_idle(vk);
    for (int i = 0; i < p->images.num; i++)
        pl_tex_destroy(gpu, &p->images.elem[i]);
    for (int i = 0; i < p->sems.num; i++) {
        vk->DestroySemaphore(vk->dev, p->sems.elem[i].in, PL_VK_ALLOC);
        vk->DestroySemaphore(vk->dev, p->sems.elem[i].out, PL_VK_ALLOC);
    }

    vk->DestroySwapchainKHR(vk->dev, p->swapchain, PL_VK_ALLOC);
    pl_free((void *) sw);
}

static int vk_sw_latency(const struct pl_swapchain *sw)
{
    struct priv *p = PL_PRIV(sw);
    return p->swapchain_depth;
}

static bool update_swapchain_info(struct priv *p, VkSwapchainCreateInfoKHR *info,
                                  int w, int h)
{
    struct vk_ctx *vk = p->vk;

    // Query the supported capabilities and update this struct as needed
    VkSurfaceCapabilitiesKHR caps = {0};
    VK(vk->GetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physd, p->surf, &caps));

    // Check for hidden/invisible window
    if (!caps.currentExtent.width || !caps.currentExtent.height) {
        PL_DEBUG(vk, "maxImageExtent reported as 0x0, hidden window? skipping");
        return false;
    }

    // Sorted by preference
    static const struct { VkCompositeAlphaFlagsKHR vk_mode;
                          enum pl_alpha_mode pl_mode;
                        } alphaModes[] = {
        {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,  PL_ALPHA_PREMULTIPLIED},
        {VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, PL_ALPHA_INDEPENDENT},
        {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,         PL_ALPHA_UNKNOWN},
        {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,          PL_ALPHA_UNKNOWN},
    };

    for (int i = 0; i < PL_ARRAY_SIZE(alphaModes); i++) {
        if (caps.supportedCompositeAlpha & alphaModes[i].vk_mode) {
            info->compositeAlpha = alphaModes[i].vk_mode;
            p->color_repr.alpha = alphaModes[i].pl_mode;
            PL_DEBUG(vk, "Requested alpha compositing mode: %s",
                     vk_alpha_mode(info->compositeAlpha));
            break;
        }
    }

    if (!info->compositeAlpha) {
        PL_ERR(vk, "Failed picking alpha compositing mode (caps: 0x%x)",
               caps.supportedCompositeAlpha);
        goto error;
    }

    // Note: We could probably also allow picking a surface transform that
    // flips the framebuffer and set `pl_swapchain_frame.flipped`, but this
    // doesn't appear to be necessary for any vulkan implementations.
    static const VkSurfaceTransformFlagsKHR rotModes[] = {
        VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR,
    };

    for (int i = 0; i < PL_ARRAY_SIZE(rotModes); i++) {
        if (caps.supportedTransforms & rotModes[i]) {
            info->preTransform = rotModes[i];
            PL_DEBUG(vk, "Requested surface transform: %s",
                     vk_surface_transform(info->preTransform));
            break;
        }
    }

    if (!info->preTransform) {
        PL_ERR(vk, "Failed picking surface transform mode (caps: 0x%x)",
               caps.supportedTransforms);
        goto error;
    }

    // Image count as required
    PL_DEBUG(vk, "Requested image count: %d (min %d max %d)",
             (int) info->minImageCount, (int) caps.minImageCount,
             (int) caps.maxImageCount);

    info->minImageCount = PL_MAX(info->minImageCount, caps.minImageCount);
    if (caps.maxImageCount)
        info->minImageCount = PL_MIN(info->minImageCount, caps.maxImageCount);

    PL_DEBUG(vk, "Requested image size: %dx%d (min %dx%d < cur %dx%d < max %dx%d)",
             w, h, caps.minImageExtent.width, caps.minImageExtent.height,
             caps.currentExtent.width, caps.currentExtent.height,
             caps.maxImageExtent.width, caps.maxImageExtent.height);

    // Default the requested size based on the reported extent
    if (caps.currentExtent.width != 0xFFFFFFFF)
        w = PL_DEF(w, caps.currentExtent.width);
    if (caps.currentExtent.height != 0xFFFFFFFF)
        h = PL_DEF(h, caps.currentExtent.height);

    // Otherwise, re-use the existing size if available
    w = PL_DEF(w, info->imageExtent.width);
    h = PL_DEF(h, info->imageExtent.height);

    if (!w || !h) {
        PL_ERR(vk, "Failed resizing swapchain: unknown size?");
        goto error;
    }

    // Clamp the extent based on the supported limits
    w = PL_CLAMP(w, caps.minImageExtent.width,  caps.maxImageExtent.width);
    h = PL_CLAMP(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    info->imageExtent = (VkExtent2D) { w, h };

    // We just request whatever makes sense, and let the pl_vk decide what
    // pl_tex_params that translates to. That said, we still need to intersect
    // the swapchain usage flags with the format usage flags
    VkImageUsageFlags req_flags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags opt_flags = VK_IMAGE_USAGE_STORAGE_BIT;

    info->imageUsage = caps.supportedUsageFlags & (req_flags | opt_flags);
    VkFormatProperties fmtprop = {0};
    vk->GetPhysicalDeviceFormatProperties(vk->physd, info->imageFormat, &fmtprop);

#define CHECK(usage, feature) \
    if (!((fmtprop.optimalTilingFeatures & VK_FORMAT_FEATURE_##feature##_BIT))) \
        info->imageUsage &= ~VK_IMAGE_USAGE_##usage##_BIT

    CHECK(COLOR_ATTACHMENT, COLOR_ATTACHMENT);
    CHECK(TRANSFER_DST, TRANSFER_DST);
    CHECK(STORAGE, STORAGE_IMAGE);

    if ((info->imageUsage & req_flags) != req_flags) {
        PL_ERR(vk, "The swapchain doesn't support rendering and blitting!");
        goto error;
    }

    return true;

error:
    return false;
}

static void destroy_swapchain(struct vk_ctx *vk, struct priv *p)
{
    assert(p->old_swapchain);
    vk->DestroySwapchainKHR(vk->dev, p->old_swapchain, PL_VK_ALLOC);
    p->old_swapchain = VK_NULL_HANDLE;
}

static bool vk_sw_recreate(const struct pl_swapchain *sw, int w, int h)
{
    const struct pl_gpu *gpu = sw->gpu;
    struct priv *p = PL_PRIV(sw);
    struct vk_ctx *vk = p->vk;

    VkImage *vkimages = NULL;
    int num_images = 0;

    // It's invalid to trigger another swapchain recreation while there's more
    // than one swapchain already active, so we need to flush any pending
    // asynchronous swapchain release operations that may be ongoing
    while (p->old_swapchain) {
        vk_flush_commands(vk);
        vk_poll_commands(vk, UINT64_MAX);
    }

    VkSwapchainCreateInfoKHR sinfo = p->protoInfo;
    sinfo.oldSwapchain = p->swapchain;

    if (!update_swapchain_info(p, &sinfo, w, h))
        return false;

    PL_INFO(sw, "(Re)creating swapchain of size %dx%d",
            sinfo.imageExtent.width,
            sinfo.imageExtent.height);

    VK(vk->CreateSwapchainKHR(vk->dev, &sinfo, PL_VK_ALLOC, &p->swapchain));

    p->suboptimal = false;
    p->cur_width = sinfo.imageExtent.width;
    p->cur_height = sinfo.imageExtent.height;

    // Freeing the old swapchain while it's still in use is an error, so do it
    // asynchronously once the device is idle
    if (sinfo.oldSwapchain) {
        p->old_swapchain = sinfo.oldSwapchain;
        vk_dev_callback(vk, (vk_cb) destroy_swapchain, vk, p);
    }

    // Get the new swapchain images
    VK(vk->GetSwapchainImagesKHR(vk->dev, p->swapchain, &num_images, NULL));
    vkimages = pl_calloc_ptr(NULL, num_images, vkimages);
    VK(vk->GetSwapchainImagesKHR(vk->dev, p->swapchain, &num_images, vkimages));

    for (int i = 0; i < num_images; i++)
        PL_VK_NAME(IMAGE, vkimages[i], "swapchain");

    // If needed, allocate some more semaphores
    while (num_images > p->sems.num) {
        VkSemaphore sem_in = NULL, sem_out = NULL;
        static const VkSemaphoreCreateInfo seminfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VK(vk->CreateSemaphore(vk->dev, &seminfo, PL_VK_ALLOC, &sem_in));
        VK(vk->CreateSemaphore(vk->dev, &seminfo, PL_VK_ALLOC, &sem_out));
        PL_VK_NAME(SEMAPHORE, sem_in, "swapchain in");
        PL_VK_NAME(SEMAPHORE, sem_out, "swapchain out");

        PL_ARRAY_APPEND(sw, p->sems, (struct sem_pair) {
            .in = sem_in,
            .out = sem_out,
        });
    }

    // Recreate the pl_tex wrappers
    for (int i = 0; i < p->images.num; i++)
        pl_tex_destroy(gpu, &p->images.elem[i]);
    p->images.num = 0;

    for (int i = 0; i < num_images; i++) {
        const VkExtent2D *ext = &sinfo.imageExtent;
        const struct pl_tex *tex = pl_vulkan_wrap(gpu, &(struct pl_vulkan_wrap_params) {
            .image = vkimages[i],
            .width = ext->width,
            .height = ext->height,
            .format = sinfo.imageFormat,
            .usage = sinfo.imageUsage,
        });
        if (!tex)
            goto error;
        PL_ARRAY_APPEND(sw, p->images, tex);
    }

    pl_assert(num_images > 0);
    int bits = 0;

    // The channel with the most bits is probably the most authoritative about
    // the actual color information (consider e.g. a2bgr10). Slight downside
    // in that it results in rounding r/b for e.g. rgb565, but we don't pick
    // surfaces with fewer than 8 bits anyway, so let's not care for now.
    const struct pl_fmt *fmt = p->images.elem[0]->params.format;
    for (int i = 0; i < fmt->num_components; i++)
        bits = PL_MAX(bits, fmt->component_depth[i]);

    p->color_repr.bits.sample_depth = bits;
    p->color_repr.bits.color_depth = bits;

    // Re-set the HDR metadata
    pl_swapchain_hdr_metadata(sw, &p->hdr_metadata);

    pl_free(vkimages);
    return true;

error:
    PL_ERR(vk, "Failed (re)creating swapchain!");
    pl_free(vkimages);
    if (p->swapchain != sinfo.oldSwapchain) {
        vk->DestroySwapchainKHR(vk->dev, p->swapchain, PL_VK_ALLOC);
        p->swapchain = VK_NULL_HANDLE;
        p->cur_width = p->cur_height = 0;
        p->suboptimal = false;
    }
    return false;
}

static bool vk_sw_start_frame(const struct pl_swapchain *sw,
                              struct pl_swapchain_frame *out_frame)
{
    struct priv *p = PL_PRIV(sw);
    struct vk_ctx *vk = p->vk;
    if (!p->swapchain && !vk_sw_recreate(sw, 0, 0))
        return false;

    if (p->suboptimal && !p->params.allow_suboptimal) {
        PL_TRACE(vk, "Swapchain is suboptimal, recreating...");
        if (!vk_sw_recreate(sw, 0, 0))
            return false;
    }

    VkSemaphore sem_in = p->sems.elem[p->idx_sems].in;
    PL_TRACE(vk, "vkAcquireNextImageKHR signals %p", (void *) sem_in);

    for (int attempts = 0; attempts < 2; attempts++) {
        uint32_t imgidx = 0;
        VkResult res = vk->AcquireNextImageKHR(vk->dev, p->swapchain, UINT64_MAX,
                                               sem_in, VK_NULL_HANDLE, &imgidx);

        switch (res) {
        case VK_SUBOPTIMAL_KHR:
            p->suboptimal = true;
            // fall through
        case VK_SUCCESS:
            p->last_imgidx = imgidx;
            pl_vulkan_release(sw->gpu, p->images.elem[imgidx],
                              VK_IMAGE_LAYOUT_UNDEFINED, 0, sem_in);
            *out_frame = (struct pl_swapchain_frame) {
                .fbo = p->images.elem[imgidx],
                .flipped = false,
                .color_repr = p->color_repr,
                .color_space = p->color_space,
            };
            return true;

        case VK_ERROR_OUT_OF_DATE_KHR: {
            // In these cases try recreating the swapchain
            if (!vk_sw_recreate(sw, 0, 0))
                return false;
            continue;
        }

        default:
            PL_ERR(vk, "Failed acquiring swapchain image: %s", vk_res_str(res));
            return false;
        }
    }

    // If we've exhausted the number of attempts to recreate the swapchain,
    // just give up silently and let the user retry some time later.
    return false;
}

static void present_cb(struct priv *p, void *arg)
{
    p->frames_in_flight--;
}

static bool vk_sw_submit_frame(const struct pl_swapchain *sw)
{
    const struct pl_gpu *gpu = sw->gpu;
    struct priv *p = PL_PRIV(sw);
    struct vk_ctx *vk = p->vk;
    if (!p->swapchain)
        return false;

    VkSemaphore sem_out = p->sems.elem[p->idx_sems++].out;
    p->idx_sems %= p->sems.num;

    bool held = pl_vulkan_hold(gpu, p->images.elem[p->last_imgidx],
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                               VK_ACCESS_MEMORY_READ_BIT, sem_out);
    if (!held) {
        PL_ERR(gpu, "Failed holding swapchain image for presentation");
        return false;
    }

    struct vk_cmd *cmd = pl_vk_steal_cmd(gpu);
    if (!cmd)
        return false;

    p->frames_in_flight++;
    vk_cmd_callback(cmd, (vk_cb) present_cb, p, NULL);

    vk_cmd_queue(vk, &cmd);
    if (!vk_flush_commands(vk))
        return false;

    struct vk_cmdpool *pool = vk->pool_graphics;
    VkQueue queue = pool->queues[pool->idx_queues];

    vk_rotate_queues(p->vk);

    VkPresentInfoKHR pinfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &sem_out,
        .swapchainCount = 1,
        .pSwapchains = &p->swapchain,
        .pImageIndices = &p->last_imgidx,
    };

    PL_TRACE(vk, "vkQueuePresentKHR waits on %p", (void *) sem_out);
    VkResult res = vk->QueuePresentKHR(queue, &pinfo);
    switch (res) {
    case VK_SUBOPTIMAL_KHR:
        p->suboptimal = true;
        // fall through
    case VK_SUCCESS:
        return true;

    case VK_ERROR_OUT_OF_DATE_KHR:
        // We can silently ignore this error, since the next start_frame will
        // recreate the swapchain automatically.
        return true;

    default:
        PL_ERR(vk, "Failed presenting to queue %p: %s", (void *) queue,
               vk_res_str(res));
        return false;
    }
}

static void vk_sw_swap_buffers(const struct pl_swapchain *sw)
{
    struct priv *p = PL_PRIV(sw);

    while (p->frames_in_flight >= p->swapchain_depth)
        vk_poll_commands(p->vk, UINT64_MAX);
}

static bool vk_sw_resize(const struct pl_swapchain *sw, int *width, int *height)
{
    struct priv *p = PL_PRIV(sw);
    bool ok = true;

    bool width_changed = *width && *width != p->cur_width,
         height_changed = *height && *height != p->cur_height;

    if (p->suboptimal || width_changed || height_changed)
        ok = vk_sw_recreate(sw, *width, *height);

    *width = p->cur_width;
    *height = p->cur_height;
    return ok;
}

static bool vk_sw_hdr_metadata(const struct pl_swapchain *sw,
                               const struct pl_hdr_metadata *metadata)
{
    struct priv *p = PL_PRIV(sw);
    struct vk_ctx *vk = p->vk;
    if (!vk->SetHdrMetadataEXT)
        return false;

    if (!pl_color_transfer_is_hdr(p->color_space.transfer))
        return false;

    if (!p->swapchain && !vk_sw_recreate(sw, 0, 0))
        return false;

    if (!metadata)
        return true;

    // Remember the metadata so we can re-apply it after swapchain recreation
    p->hdr_metadata = *metadata;

    vk->SetHdrMetadataEXT(vk->dev, 1, &p->swapchain, &(VkHdrMetadataEXT) {
        .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
        .displayPrimaryRed   = { metadata->prim.red.x,   metadata->prim.red.y },
        .displayPrimaryGreen = { metadata->prim.green.x, metadata->prim.green.y },
        .displayPrimaryBlue  = { metadata->prim.blue.x,  metadata->prim.blue.y },
        .whitePoint = { metadata->prim.white.x, metadata->prim.white.y },
        .maxLuminance = metadata->max_luma,
        .minLuminance = metadata->min_luma,
        .maxContentLightLevel = metadata->max_cll,
        .maxFrameAverageLightLevel = metadata->max_fall,
    });

    return true;
}

bool pl_vulkan_swapchain_suboptimal(const struct pl_swapchain *sw)
{
    struct priv *p = PL_PRIV(sw);
    return p->suboptimal;
}

static struct pl_sw_fns vulkan_swapchain = {
    .destroy      = vk_sw_destroy,
    .latency      = vk_sw_latency,
    .resize       = vk_sw_resize,
    .hdr_metadata = vk_sw_hdr_metadata,
    .start_frame  = vk_sw_start_frame,
    .submit_frame = vk_sw_submit_frame,
    .swap_buffers = vk_sw_swap_buffers,
};
