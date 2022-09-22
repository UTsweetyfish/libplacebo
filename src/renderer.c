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

#include <math.h>

#include "common.h"
#include "shaders.h"
#include "dispatch.h"

struct cached_frame {
    uint64_t signature;
    uint64_t params_hash; // for detecting `pl_render_params` changes
    struct pl_color_space color;
    struct pl_icc_profile profile;
    const struct pl_tex *tex;
    bool evict; // for garbage collection
};

struct sampler {
    struct pl_shader_obj *upscaler_state;
    struct pl_shader_obj *downscaler_state;
};

struct pl_renderer {
    const struct pl_gpu *gpu;
    struct pl_context *ctx;
    struct pl_dispatch *dp;

    // Texture format to use for intermediate textures
    const struct pl_fmt *fbofmt[5];

    // Cached feature checks (inverted)
    bool disable_compute;       // disable the use of compute shaders
    bool disable_sampling;      // disable use of advanced scalers
    bool disable_debanding;     // disable the use of debanding shaders
    bool disable_linear_hdr;    // disable linear scaling for HDR signals
    bool disable_linear_sdr;    // disable linear scaling for SDR signals
    bool disable_blending;      // disable blending for the target/fbofmt
    bool disable_overlay;       // disable rendering overlays
    bool disable_icc;           // disable usage of ICC profiles
    bool disable_peak_detect;   // disable peak detection shader
    bool disable_grain;         // disable AV1 grain code
    bool disable_hooks;         // disable user hooks / custom shaders
    bool disable_mixing;        // disable frame mixing

    // Shader resource objects and intermediate textures (FBOs)
    struct pl_shader_obj *peak_detect_state;
    struct pl_shader_obj *dither_state;
    struct pl_shader_obj *icc_state;
    struct pl_shader_obj *grain_state[4];
    struct pl_shader_obj *lut_state[3];
    PL_ARRAY(const struct pl_tex *) fbos;
    struct sampler sampler_main;
    struct sampler samplers_src[4];
    struct sampler samplers_dst[4];
    PL_ARRAY(struct sampler) samplers_osd;

    // Frame cache (for frame mixing / interpolation)
    PL_ARRAY(struct cached_frame) frames;
    PL_ARRAY(const struct pl_tex *) frame_fbos;
};

enum {
    // Index into `lut_state`
    LUT_IMAGE,
    LUT_TARGET,
    LUT_PARAMS,
};

static void find_fbo_format(struct pl_renderer *rr)
{
    struct {
        enum pl_fmt_type type;
        int depth;
        enum pl_fmt_caps caps;
    } configs[] = {
        // Prefer floating point formats first
        {PL_FMT_FLOAT, 16, PL_FMT_CAP_LINEAR},
        {PL_FMT_FLOAT, 16, PL_FMT_CAP_SAMPLEABLE},

        // Otherwise, fall back to unorm/snorm, preferring linearly sampleable
        {PL_FMT_UNORM, 16, PL_FMT_CAP_LINEAR},
        {PL_FMT_SNORM, 16, PL_FMT_CAP_LINEAR},
        {PL_FMT_UNORM, 16, PL_FMT_CAP_SAMPLEABLE},
        {PL_FMT_SNORM, 16, PL_FMT_CAP_SAMPLEABLE},

        // As a final fallback, allow 8-bit FBO formats (for UNORM only)
        {PL_FMT_UNORM, 8, PL_FMT_CAP_LINEAR},
        {PL_FMT_UNORM, 8, PL_FMT_CAP_SAMPLEABLE},
    };

    const struct pl_fmt *fmt = NULL;
    for (int i = 0; i < PL_ARRAY_SIZE(configs); i++) {
        fmt = pl_find_fmt(rr->gpu, configs[i].type, 4, configs[i].depth, 0,
                          configs[i].caps | PL_FMT_CAP_RENDERABLE);
        if (fmt) {
            rr->fbofmt[4] = fmt;

            // Probe the right variant for each number of channels, falling
            // back to the next biggest format
            for (int c = 1; c < 4; c++) {
                rr->fbofmt[c] = pl_find_fmt(rr->gpu, configs[i].type, c,
                                            configs[i].depth, 0, fmt->caps);
                rr->fbofmt[c] = PL_DEF(rr->fbofmt[c], rr->fbofmt[c+1]);
            }
            break;
        }
    }

    if (!fmt) {
        PL_WARN(rr, "Found no renderable FBO format! Most features disabled");
        return;
    }

    if (!(fmt->caps & PL_FMT_CAP_STORABLE)) {
        PL_INFO(rr, "Found no storable FBO format; compute shaders disabled");
        rr->disable_compute = true;
    }

    if (fmt->type != PL_FMT_FLOAT) {
        PL_INFO(rr, "Found no floating point FBO format; linear light "
                "processing disabled for HDR material");
        rr->disable_linear_hdr = true;
    }

    if (fmt->component_depth[0] < 16) {
        PL_WARN(rr, "FBO format precision low (<16 bit); linear light "
                "processing disabled");
        rr->disable_linear_sdr = true;
    }
}

struct pl_renderer *pl_renderer_create(struct pl_context *ctx,
                                       const struct pl_gpu *gpu)
{
    struct pl_renderer *rr = pl_alloc_ptr(NULL, rr);
    *rr = (struct pl_renderer) {
        .gpu  = gpu,
        .ctx = ctx,
        .dp  = pl_dispatch_create(ctx, gpu),
    };

    assert(rr->dp);
    find_fbo_format(rr);
    return rr;
}

static void sampler_destroy(struct pl_renderer *rr, struct sampler *sampler)
{
    pl_shader_obj_destroy(&sampler->upscaler_state);
    pl_shader_obj_destroy(&sampler->downscaler_state);
}

void pl_renderer_destroy(struct pl_renderer **p_rr)
{
    struct pl_renderer *rr = *p_rr;
    if (!rr)
        return;

    // Free all intermediate FBOs
    for (int i = 0; i < rr->fbos.num; i++)
        pl_tex_destroy(rr->gpu, &rr->fbos.elem[i]);
    for (int i = 0; i < rr->frames.num; i++)
        pl_tex_destroy(rr->gpu, &rr->frames.elem[i].tex);
    for (int i = 0; i < rr->frame_fbos.num; i++)
        pl_tex_destroy(rr->gpu, &rr->frame_fbos.elem[i]);

    // Free all shader resource objects
    pl_shader_obj_destroy(&rr->peak_detect_state);
    pl_shader_obj_destroy(&rr->dither_state);
    pl_shader_obj_destroy(&rr->icc_state);
    for (int i = 0; i < PL_ARRAY_SIZE(rr->lut_state); i++)
        pl_shader_obj_destroy(&rr->lut_state[i]);
    for (int i = 0; i < PL_ARRAY_SIZE(rr->grain_state); i++)
        pl_shader_obj_destroy(&rr->grain_state[i]);

    // Free all samplers
    sampler_destroy(rr, &rr->sampler_main);
    for (int i = 0; i < PL_ARRAY_SIZE(rr->samplers_src); i++)
        sampler_destroy(rr, &rr->samplers_src[i]);
    for (int i = 0; i < PL_ARRAY_SIZE(rr->samplers_dst); i++)
        sampler_destroy(rr, &rr->samplers_dst[i]);
    for (int i = 0; i < rr->samplers_osd.num; i++)
        sampler_destroy(rr, &rr->samplers_osd.elem[i]);

    pl_dispatch_destroy(&rr->dp);
    pl_free_ptr(p_rr);
}

size_t pl_renderer_save(struct pl_renderer *rr, uint8_t *out_cache)
{
    return pl_dispatch_save(rr->dp, out_cache);
}

void pl_renderer_load(struct pl_renderer *rr, const uint8_t *cache)
{
    pl_dispatch_load(rr->dp, cache);
}

void pl_renderer_flush_cache(struct pl_renderer *rr)
{
    for (int i = 0; i < rr->frames.num; i++)
        pl_tex_destroy(rr->gpu, &rr->frames.elem[i].tex);
    rr->frames.num = 0;

    pl_shader_obj_destroy(&rr->peak_detect_state);
}

const struct pl_render_params pl_render_default_params = {
    .upscaler           = &pl_filter_spline36,
    .downscaler         = &pl_filter_mitchell,
    .frame_mixer        = &pl_oversample_frame_mixer,
    .lut_entries        = 64,
    .polar_cutoff       = 0.001,

    .sigmoid_params     = &pl_sigmoid_default_params,
    .peak_detect_params = &pl_peak_detect_default_params,
    .color_map_params   = &pl_color_map_default_params,
    .dither_params      = &pl_dither_default_params,
};

const struct pl_render_params pl_render_high_quality_params = {
    .upscaler           = &pl_filter_ewa_lanczos,
    .downscaler         = &pl_filter_mitchell,
    .frame_mixer        = &pl_oversample_frame_mixer,
    .lut_entries        = 64,
    .polar_cutoff       = 0.001,

    .deband_params      = &pl_deband_default_params,
    .sigmoid_params     = &pl_sigmoid_default_params,
    .peak_detect_params = &pl_peak_detect_default_params,
    .color_map_params   = &pl_color_map_default_params,
    .dither_params      = &pl_dither_default_params,
};

const struct pl_filter_config pl_oversample_frame_mixer = {0};

const struct pl_filter_preset pl_frame_mixers[] = {
    { "none",           NULL,                       "No frame mixing" },
    { "oversample",     &pl_oversample_frame_mixer, "Oversample (AKA SmoothMotion)" },
    { "mitchell_clamp", &pl_filter_mitchell_clamp,  "Cubic spline (clamped)" },
    {0}
};

const int pl_num_frame_mixers = PL_ARRAY_SIZE(pl_frame_mixers) - 1;

#define FBOFMT(n) (params->disable_fbos ? NULL : rr->fbofmt[n])

// Represents a "in-flight" image, which is either a shader that's in the
// process of producing some sort of image, or a texture that needs to be
// sampled from
struct img {
    // Effective texture size, always set
    int w, h;

    // Recommended format (falls back to FBOFMT otherwise), only for shaders
    const struct pl_fmt *fmt;

    // Exactly *one* of these two is set:
    struct pl_shader *sh;
    const struct pl_tex *tex;

    // Current effective source area, will be sampled by the main scaler
    struct pl_rect2df rect;

    // The current effective colorspace
    struct pl_color_repr repr;
    struct pl_color_space color;
    int comps;
};

// Plane 'type', ordered by incrementing priority
enum plane_type {
    PLANE_INVALID = 0,
    PLANE_ALPHA,
    PLANE_CHROMA,
    PLANE_LUMA,
    PLANE_RGB,
    PLANE_XYZ,
};

struct pass_state {
    void *tmp;

    // Pointer back to the renderer itself, for callbacks
    struct pl_renderer *rr;

    // Represents the "current" image which we're in the process of rendering.
    // This is initially set by pass_read_image, and all of the subsequent
    // rendering steps will mutate this in-place.
    struct img img;

    // Represents the "reference rect". Canonically, this is functionallity
    // equivalent to `image.crop`, but both guaranteed to be valid, and also
    // updates as the refplane evolves (e.g. due to user hook prescalers)
    struct pl_rect2df ref_rect;

    // Integer version of `target.crop`. Semantically identical.
    struct pl_rect2d dst_rect;

    // Cached copies of the `image` / `target` for this rendering pass,
    // corrected to make sure all rects etc. are properly defaulted/inferred.
    struct pl_frame image;
    struct pl_frame target;

    // Some extra plane metadata, inferred from `planes`
    enum plane_type src_type[4];
    enum plane_type dst_type[4];
    int src_ref, dst_ref; // index into `planes`

    // Metadata for `rr->fbos`
    bool *fbos_used;
};

static const struct pl_tex *get_fbo(struct pass_state *pass, int w, int h,
                                    const struct pl_fmt *fmt, int comps)
{
    struct pl_renderer *rr = pass->rr;
    comps = PL_DEF(comps, 4);
    fmt = PL_DEF(fmt, rr->fbofmt[comps]);
    if (!fmt)
        return NULL;

    struct pl_tex_params params = {
        .w = w,
        .h = h,
        .format = fmt,
        .sampleable = true,
        .renderable = true,
        .storable   = fmt->caps & PL_FMT_CAP_STORABLE,
    };

    int best_idx = -1;
    int best_diff = 0;

    // Find the best-fitting texture out of rr->fbos
    for (int i = 0; i < rr->fbos.num; i++) {
        if (pass->fbos_used[i])
            continue;

        // Orthogonal distance, with penalty for format mismatches
        int diff = abs(rr->fbos.elem[i]->params.w - w) +
                   abs(rr->fbos.elem[i]->params.h - h) +
                   ((rr->fbos.elem[i]->params.format != fmt) ? 1000 : 0);

        if (best_idx < 0 || diff < best_diff) {
            best_idx = i;
            best_diff = diff;
        }
    }

    // No texture found at all, add a new one
    if (best_idx < 0) {
        best_idx = rr->fbos.num;
        PL_ARRAY_APPEND(rr, rr->fbos, NULL);
        pl_grow(pass->tmp, &pass->fbos_used, rr->fbos.num * sizeof(bool));
        pass->fbos_used[best_idx] = false;
    }

    if (!pl_tex_recreate(rr->gpu, &rr->fbos.elem[best_idx], &params))
        return NULL;

    pass->fbos_used[best_idx] = true;
    return rr->fbos.elem[best_idx];
}

// Forcibly convert an img to `tex`, dispatching where necessary
static const struct pl_tex *img_tex(struct pass_state *pass, struct img *img)
{
    if (img->tex) {
        pl_assert(!img->sh);
        return img->tex;
    }

    struct pl_renderer *rr = pass->rr;
    const struct pl_tex *tex = get_fbo(pass, img->w, img->h, img->fmt, img->comps);
    img->fmt = NULL;

    if (!tex) {
        PL_ERR(rr, "Failed creating FBO texture! Disabling advanced rendering..");
        memset(rr->fbofmt, 0, sizeof(rr->fbofmt));
        pl_dispatch_abort(rr->dp, &img->sh);
        return NULL;
    }

    pl_assert(img->sh);
    bool ok = pl_dispatch_finish(rr->dp, &(struct pl_dispatch_params) {
        .shader = &img->sh,
        .target = tex,
    });

    if (!ok) {
        PL_ERR(rr, "Failed dispatching intermediate pass!");
        img->sh = pl_dispatch_begin(rr->dp);
        return NULL;
    }

    img->tex = tex;
    return img->tex;
}

// Forcibly convert an img to `sh`, sampling where necessary
static struct pl_shader *img_sh(struct pass_state *pass, struct img *img)
{
    if (img->sh) {
        pl_assert(!img->tex);
        return img->sh;
    }

    pl_assert(img->tex);
    img->sh = pl_dispatch_begin(pass->rr->dp);
    pl_shader_sample_direct(img->sh, &(struct pl_sample_src) {
        .tex = img->tex,
    });

    img->tex = NULL;
    return img->sh;
}

enum sampler_type {
    SAMPLER_DIRECT,  // pick based on texture caps
    SAMPLER_NEAREST, // direct sampling, force nearest
    SAMPLER_BICUBIC, // fast bicubic scaling
    SAMPLER_COMPLEX, // complex custom filters
};

enum sampler_dir {
    SAMPLER_NOOP, // 1:1 scaling
    SAMPLER_UP,   // upscaling
    SAMPLER_DOWN, // downscaling
};

struct sampler_info {
    const struct pl_filter_config *config; // if applicable
    enum sampler_type type;
    enum sampler_dir dir;
    enum sampler_dir dir_sep[2];
};

static struct sampler_info sample_src_info(struct pl_renderer *rr,
                                           const struct pl_sample_src *src,
                                           const struct pl_render_params *params)
{
    struct sampler_info info = {0};

    float rx = src->new_w / fabs(pl_rect_w(src->rect));
    if (rx < 1.0 - 1e-6) {
        info.dir_sep[0] = SAMPLER_DOWN;
    } else if (rx > 1.0 + 1e-6) {
        info.dir_sep[0] = SAMPLER_UP;
    }

    float ry = src->new_h / fabs(pl_rect_h(src->rect));
    if (ry < 1.0 - 1e-6) {
        info.dir_sep[1] = SAMPLER_DOWN;
    } else if (ry > 1.0 + 1e-6) {
        info.dir_sep[1] = SAMPLER_UP;
    }

    // We use PL_MAX so downscaling overrides upscaling when choosing scalers
    info.dir = PL_MAX(info.dir_sep[0], info.dir_sep[1]);
    switch (info.dir) {
    case SAMPLER_DOWN:
        info.config = params->downscaler;
        break;
    case SAMPLER_UP:
        info.config = params->upscaler;
        break;
    case SAMPLER_NOOP:
        info.type = SAMPLER_NEAREST;
        return info;
    }

    int comps = PL_DEF(src->components, 4);
    if (!FBOFMT(comps) || rr->disable_sampling || !info.config) {
        info.type = SAMPLER_DIRECT;
    } else {
        info.type = SAMPLER_COMPLEX;

        // Try using faster replacements for GPU built-in scalers
        const struct pl_fmt *texfmt = src->tex ? src->tex->params.format : rr->fbofmt[comps];
        bool can_linear = texfmt->caps & PL_FMT_CAP_LINEAR;
        bool can_fast = info.dir == SAMPLER_UP || params->skip_anti_aliasing;

        if (can_fast && !params->disable_builtin_scalers) {
            if (can_linear && info.config == &pl_filter_bicubic)
                info.type = SAMPLER_BICUBIC;
            if (can_linear && info.config == &pl_filter_bilinear)
                info.type = SAMPLER_DIRECT;
            if (info.config == &pl_filter_nearest)
                info.type = can_linear ? SAMPLER_NEAREST : SAMPLER_DIRECT;
        }
    }

    return info;
}

static void dispatch_sampler(struct pass_state *pass, struct pl_shader *sh,
                             struct sampler *sampler, bool no_compute,
                             const struct pl_render_params *params,
                             const struct pl_sample_src *src)
{
    if (!sampler)
        goto fallback;

    struct pl_renderer *rr = pass->rr;
    struct sampler_info info = sample_src_info(rr, src, params);
    struct pl_shader_obj **lut = NULL;
    switch (info.dir) {
    case SAMPLER_NOOP:
        goto fallback;
    case SAMPLER_DOWN:
        lut = &sampler->downscaler_state;
        break;
    case SAMPLER_UP:
        lut = &sampler->upscaler_state;
        break;
    }

    switch (info.type) {
    case SAMPLER_DIRECT:
        goto fallback;
    case SAMPLER_NEAREST:
        pl_shader_sample_nearest(sh, src);
        return;
    case SAMPLER_BICUBIC:
        pl_shader_sample_bicubic(sh, src);
        return;
    case SAMPLER_COMPLEX:
        break; // continue below
    }

    pl_assert(lut);
    struct pl_sample_filter_params fparams = {
        .filter      = *info.config,
        .lut_entries = params->lut_entries,
        .cutoff      = params->polar_cutoff,
        .antiring    = params->antiringing_strength,
        .no_compute  = rr->disable_compute || no_compute,
        .no_widening = params->skip_anti_aliasing,
        .lut         = lut,
    };

    bool ok;
    if (info.config->polar) {
        // Polar samplers are always a single function call
        ok = pl_shader_sample_polar(sh, src, &fparams);
    } else if (info.dir_sep[0] && info.dir_sep[1]) {
        // Scaling is needed in both directions
        struct pl_shader *tsh = pl_dispatch_begin(rr->dp);
        ok = pl_shader_sample_ortho(tsh, PL_SEP_VERT, src, &fparams);
        if (!ok) {
            pl_dispatch_abort(rr->dp, &tsh);
            goto done;
        }

        struct img img = {
            .sh = tsh,
            .w  = src->tex->params.w,
            .h  = src->new_h,
            .comps = src->components,
        };

        struct pl_sample_src src2 = *src;
        src2.tex = img_tex(pass, &img);
        src2.scale = 1.0;
        ok = src2.tex && pl_shader_sample_ortho(sh, PL_SEP_HORIZ, &src2, &fparams);
    } else if (info.dir_sep[0]) {
        // Scaling is needed only in the horizontal direction
        ok = pl_shader_sample_ortho(sh, PL_SEP_HORIZ, src, &fparams);
    } else {
        // Scaling is needed only in the vertical direction
        pl_assert(info.dir_sep[1]);
        ok = pl_shader_sample_ortho(sh, PL_SEP_VERT, src, &fparams);
    }

done:
    if (!ok) {
        PL_ERR(rr, "Failed dispatching scaler.. disabling");
        rr->disable_sampling = true;
        goto fallback;
    }

    return;

fallback:
    // If all else fails, fall back to auto sampling
    pl_shader_sample_direct(sh, src);
}

static void swizzle_color(struct pl_shader *sh, int comps, const int comp_map[4])
{
    ident_t orig = sh_fresh(sh, "orig_color");
    GLSL("vec4 %s = color;   \n"
         "color = vec4(0.0); \n", orig);

    static const int def_map[4] = {0, 1, 2, 3};
    comp_map = PL_DEF(comp_map, def_map);

    for (int c = 0; c < comps; c++) {
        if (comp_map[c] >= 0)
            GLSL("color[%d] = %s[%d]; \n", c, orig, comp_map[c]);
    }
}

static void draw_overlays(struct pass_state *pass, const struct pl_tex *fbo,
                          int comps, const int comp_map[4],
                          const struct pl_overlay *overlays, int num,
                          struct pl_color_space color, struct pl_color_repr repr,
                          bool use_sigmoid, struct pl_transform2x2 *scale,
                          const struct pl_render_params *params)
{
    struct pl_renderer *rr = pass->rr;
    if (num <= 0 || rr->disable_overlay)
        return;

    enum pl_fmt_caps caps = fbo->params.format->caps;
    if (!rr->disable_blending && !(caps & PL_FMT_CAP_BLENDABLE)) {
        PL_WARN(rr, "Trying to draw an overlay to a non-blendable target. "
                "Alpha blending is disabled, results may be incorrect!");
        rr->disable_blending = true;
    }

    while (num > rr->samplers_osd.num)
        PL_ARRAY_APPEND(rr, rr->samplers_osd, (struct sampler) {0});

    for (int n = 0; n < num; n++) {
        const struct pl_overlay *ol = &overlays[n];
        const struct pl_plane *plane = &ol->plane;
        const struct pl_tex *tex = plane->texture;

        struct pl_rect2d rect = ol->rect;
        if (scale) {
            float v0[2] = { rect.x0, rect.y0 };
            float v1[2] = { rect.x1, rect.y1 };
            pl_transform2x2_apply(scale, v0);
            pl_transform2x2_apply(scale, v1);
            rect = (struct pl_rect2d) { v0[0], v0[1], v1[0], v1[1] };
        }

        struct pl_sample_src src = {
            .tex        = tex,
            .components = ol->mode == PL_OVERLAY_MONOCHROME ? 1 : plane->components,
            .new_w      = abs(pl_rect_w(rect)),
            .new_h      = abs(pl_rect_h(rect)),
            .rect = {
                -plane->shift_x,
                -plane->shift_y,
                tex->params.w - plane->shift_x,
                tex->params.h - plane->shift_y,
            },
        };

        struct sampler *sampler = &rr->samplers_osd.elem[n];
        if (params->disable_overlay_sampling)
            sampler = NULL;

        struct pl_shader *sh = pl_dispatch_begin(rr->dp);
        dispatch_sampler(pass, sh, sampler, !fbo->params.storable, params, &src);

        GLSL("vec4 osd_color;\n");
        for (int c = 0; c < src.components; c++) {
            if (plane->component_mapping[c] < 0)
                continue;
            GLSL("osd_color[%d] = color[%d];\n", plane->component_mapping[c], c);
        }

        switch (ol->mode) {
        case PL_OVERLAY_NORMAL:
            GLSL("color = osd_color;\n");
            break;
        case PL_OVERLAY_MONOCHROME:
            GLSL("color.a = osd_color[0];\n");
            GLSL("color.rgb = %s;\n", sh_var(sh, (struct pl_shader_var) {
                .var  = pl_var_vec3("base_color"),
                .data = &ol->base_color,
                .dynamic = true,
            }));
            break;
        default: abort();
        }

        struct pl_color_repr ol_repr = ol->repr;
        pl_shader_decode_color(sh, &ol_repr, NULL);
        pl_shader_color_map(sh, params->color_map_params, ol->color, color,
                            NULL, false);

        if (use_sigmoid)
            pl_shader_sigmoidize(sh, params->sigmoid_params);

        pl_shader_encode_color(sh, &repr);
        swizzle_color(sh, comps, comp_map);

        bool ok = pl_dispatch_finish(rr->dp, &(struct pl_dispatch_params) {
            .shader = &sh,
            .target = fbo,
            .rect   = rect,
            .blend_params = rr->disable_blending ? NULL : &pl_alpha_overlay,
        });

        if (!ok) {
            PL_ERR(rr, "Failed rendering overlay texture!");
            rr->disable_overlay = true;
            return;
        }
    }
}

static const struct pl_tex *get_hook_tex(void *priv, int width, int height)
{
    struct pass_state *pass = priv;

    return get_fbo(pass, width, height, NULL, 4);
}

// Returns if any hook was applied (even if there were errors)
static bool pass_hook(struct pass_state *pass, struct img *img,
                      enum pl_hook_stage stage,
                      const struct pl_render_params *params)
{
    struct pl_renderer *rr = pass->rr;
    if (!rr->fbofmt[4] || rr->disable_hooks)
        return false;

    bool ret = false;

    for (int n = 0; n < params->num_hooks; n++) {
        const struct pl_hook *hook = params->hooks[n];
        if (!(hook->stages & stage))
            continue;

        PL_TRACE(rr, "Dispatching hook %d stage 0x%x", n, stage);
        struct pl_hook_params hparams = {
            .gpu = rr->gpu,
            .dispatch = rr->dp,
            .get_tex = get_hook_tex,
            .priv = pass,
            .stage = stage,
            .rect = img->rect,
            .repr = img->repr,
            .color = img->color,
            .components = img->comps,
            .src_rect = pass->ref_rect,
            .dst_rect = pass->dst_rect,
        };

        // TODO: Add some sort of `test` API function to the hooks that allows
        // us to skip having to touch the `img` state at all for no-ops

        switch (hook->input) {
        case PL_HOOK_SIG_NONE:
            break;

        case PL_HOOK_SIG_TEX: {
            hparams.tex = img_tex(pass, img);
            if (!hparams.tex) {
                PL_ERR(rr, "Failed dispatching shader prior to hook!");
                goto error;
            }
            break;
        }

        case PL_HOOK_SIG_COLOR:
            hparams.sh = img_sh(pass, img);
            break;

        default: abort();
        }

        struct pl_hook_res res = hook->hook(hook->priv, &hparams);
        if (res.failed) {
            PL_ERR(rr, "Failed executing hook, disabling");
            goto error;
        }

        bool resizable = pl_hook_stage_resizable(stage);
        switch (res.output) {
        case PL_HOOK_SIG_NONE:
            break;

        case PL_HOOK_SIG_TEX:
            if (!resizable) {
                if (res.tex->params.w != img->w ||
                    res.tex->params.h != img->h ||
                    !pl_rect2d_eq(res.rect, img->rect))
                {
                    PL_ERR(rr, "User hook tried resizing non-resizable stage!");
                    goto error;
                }
            }

            *img = (struct img) {
                .tex = res.tex,
                .repr = res.repr,
                .color = res.color,
                .comps = res.components,
                .rect = res.rect,
                .w = res.tex->params.w,
                .h = res.tex->params.h,
            };
            break;

        case PL_HOOK_SIG_COLOR:
            if (!resizable) {
                if (res.sh->output_w != img->w ||
                    res.sh->output_h != img->h ||
                    !pl_rect2d_eq(res.rect, img->rect))
                {
                    PL_ERR(rr, "User hook tried resizing non-resizable stage!");
                    goto error;
                }
            }

            *img = (struct img) {
                .sh = res.sh,
                .repr = res.repr,
                .color = res.color,
                .comps = res.components,
                .rect = res.rect,
                .w = res.sh->output_w,
                .h = res.sh->output_h,
            };
            break;

        default: abort();
        }

        // a hook was performed successfully
        ret = true;
    }

    return ret;

error:
    rr->disable_hooks = true;

    // Make sure the state remains as valid as possible, even if the resulting
    // shaders might end up nonsensical, to prevent segfaults
    if (!img->tex && !img->sh)
        img->sh = pl_dispatch_begin(rr->dp);
    return ret;
}

// `deband_src` results
enum {
    DEBAND_NOOP = 0, // no debanding was performing
    DEBAND_NORMAL,   // debanding was performed, the plane should still be scaled
    DEBAND_SCALED,   // debanding took care of scaling as well
};

static int deband_src(struct pass_state *pass, struct pl_shader *psh,
                      const struct pl_render_params *params,
                      struct pl_sample_src *psrc)
{
    const struct pl_frame *image = &pass->image;
    struct pl_renderer *rr = pass->rr;
    if (rr->disable_debanding || !params->deband_params)
        return DEBAND_NOOP;

    if (!(psrc->tex->params.format->caps & PL_FMT_CAP_LINEAR)) {
        PL_WARN(rr, "Debanding requires uploaded textures to be linearly "
                "sampleable (params.sample_mode = PL_TEX_SAMPLE_LINEAR)! "
                "Disabling debanding..");
        rr->disable_debanding = true;
        return DEBAND_NOOP;
    }

    // The debanding shader can replace direct GPU sampling
    bool deband_scales = sample_src_info(rr, psrc, params).type == SAMPLER_DIRECT;

    struct pl_shader *sh = psh;
    struct pl_sample_src *src = psrc;
    struct pl_sample_src fixed;
    if (!deband_scales) {
        // Only sample/deband the relevant cut-out, but round it to the nearest
        // integer to avoid doing fractional scaling
        fixed = *src;
        fixed.rect.x0 = floorf(fixed.rect.x0);
        fixed.rect.y0 = floorf(fixed.rect.y0);
        fixed.rect.x1 = ceilf(fixed.rect.x1);
        fixed.rect.y1 = ceilf(fixed.rect.y1);
        fixed.new_w = pl_rect_w(fixed.rect);
        fixed.new_h = pl_rect_h(fixed.rect);
        src = &fixed;

        if (fixed.new_w == psrc->new_w &&
            fixed.new_h == psrc->new_h &&
            pl_rect2d_eq(fixed.rect, psrc->rect))
        {
            // If there's nothing left to be done (i.e. we're already rendering
            // an exact integer crop without scaling), also skip the scalers
            deband_scales = true;
        } else {
            sh = pl_dispatch_begin_ex(rr->dp, true);
        }
    }

    // Divide the deband grain scale by the effective current colorspace nominal
    // peak, to make sure the output intensity of the grain is as independent
    // of the source as possible, even though it happens this early in the
    // process (well before any linearization / output adaptation)
    struct pl_deband_params dparams = *params->deband_params;
    float scale = pl_color_transfer_nominal_peak(image->color.transfer)
                * image->color.sig_scale;
    dparams.grain /= scale;

    pl_shader_deband(sh, src, &dparams);

    if (deband_scales)
        return DEBAND_SCALED;

    struct img img = {
        .sh = sh,
        .w  = src->new_w,
        .h  = src->new_h,
        .comps = src->components,
    };

    const struct pl_tex *new = img_tex(pass, &img);
    if (!new) {
        PL_ERR(rr, "Failed dispatching debanding shader.. disabling debanding!");
        rr->disable_debanding = true;
        return DEBAND_NOOP;
    }

    // Update the original pl_sample_src to point to the new texture
    psrc->tex = new;
    psrc->rect.x0 -= src->rect.x0;
    psrc->rect.y0 -= src->rect.y0;
    psrc->rect.x1 -= src->rect.x0;
    psrc->rect.y1 -= src->rect.y0;
    psrc->scale = 1.0;
    return DEBAND_NORMAL;
}

static void hdr_update_peak(struct pass_state *pass,
                            const struct pl_render_params *params)
{
    struct pl_renderer *rr = pass->rr;
    if (!params->peak_detect_params || !pl_color_space_is_hdr(pass->img.color))
        goto cleanup;

    if (rr->disable_compute || rr->disable_peak_detect)
        goto cleanup;

    float src_peak = pass->img.color.sig_peak * pass->img.color.sig_scale;
    float dst_peak = pass->target.color.sig_peak * pass->target.color.sig_scale;
    if (src_peak <= dst_peak + 1e-6)
        goto cleanup; // no adaptation needed

    if (params->lut && params->lut_type == PL_LUT_CONVERSION)
        goto cleanup; // LUT handles tone mapping

    if (!FBOFMT(4) && !params->allow_delayed_peak_detect) {
        PL_WARN(rr, "Disabling peak detection because "
                "`allow_delayed_peak_detect` is false, but lack of FBOs "
                "forces the result to be delayed.");
        rr->disable_peak_detect = true;
        goto cleanup;
    }

    bool ok = pl_shader_detect_peak(img_sh(pass, &pass->img), pass->img.color,
                                    &rr->peak_detect_state,
                                    params->peak_detect_params);
    if (!ok) {
        PL_WARN(rr, "Failed creating HDR peak detection shader.. disabling");
        rr->disable_peak_detect = true;
        goto cleanup;
    }

    return;

cleanup:
    // No peak detection required or supported, so clean up the state to avoid
    // confusing it with later frames where peak detection is enabled again
    pl_shader_obj_destroy(&rr->peak_detect_state);
}

struct plane_state {
    enum plane_type type;
    struct pl_plane plane;
    struct img img; // for per-plane shaders
};

static const char *plane_type_names[] = {
    [PLANE_INVALID] = "invalid",
    [PLANE_ALPHA]   = "alpha",
    [PLANE_CHROMA]  = "chroma",
    [PLANE_LUMA]    = "luma",
    [PLANE_RGB]     = "rgb",
    [PLANE_XYZ]     = "xyz",
};

static void log_plane_info(struct pl_renderer *rr, const struct plane_state *st)
{
    const struct pl_plane *plane = &st->plane;
    PL_TRACE(rr, "    Type: %s", plane_type_names[st->type]);

    switch (plane->components) {
    case 0:
        PL_TRACE(rr, "    Components: (none)");
        break;
    case 1:
        PL_TRACE(rr, "    Components: {%d}",
                 plane->component_mapping[0]);
        break;
    case 2:
        PL_TRACE(rr, "    Components: {%d %d}",
                 plane->component_mapping[0],
                 plane->component_mapping[1]);
        break;
    case 3:
        PL_TRACE(rr, "    Components: {%d %d %d}",
                 plane->component_mapping[0],
                 plane->component_mapping[1],
                 plane->component_mapping[2]);
        break;
    case 4:
        PL_TRACE(rr, "    Components: {%d %d %d %d}",
                 plane->component_mapping[0],
                 plane->component_mapping[1],
                 plane->component_mapping[2],
                 plane->component_mapping[3]);
        break;
    }

    PL_TRACE(rr, "    Rect: {%f %f} -> {%f %f}",
             st->img.rect.x0, st->img.rect.y0, st->img.rect.x1, st->img.rect.y1);

    PL_TRACE(rr, "    Bits: %d (used) / %d (sampled), shift %d",
             st->img.repr.bits.color_depth,
             st->img.repr.bits.sample_depth,
             st->img.repr.bits.bit_shift);
}

// Returns true if grain was applied
static bool plane_av1_grain(struct pass_state *pass, int plane_idx,
                            struct plane_state *st,
                            const struct plane_state *ref,
                            const struct pl_frame *image,
                            const struct pl_render_params *params)
{
    struct pl_renderer *rr = pass->rr;
    if (rr->disable_grain)
        return false;

    struct img *img = &st->img;
    struct pl_plane *plane = &st->plane;
    struct pl_color_repr repr = st->img.repr;
    struct pl_av1_grain_params grain_params = {
        .data = image->av1_grain,
        .luma_tex = ref->plane.texture,
        .repr = &repr,
        .components = plane->components,
    };

    for (int c = 0; c < plane->components; c++)
        grain_params.component_mapping[c] = plane->component_mapping[c];

    for (int c = 0; c < ref->plane.components; c++) {
        if (ref->plane.component_mapping[c] == PL_CHANNEL_Y)
            grain_params.luma_comp = c;
    }

    if (!pl_needs_av1_grain(&grain_params))
        return false;

    if (!FBOFMT(plane->components)) {
        PL_ERR(rr, "AV1 grain required but no renderable format available.. "
              "disabling!");
        rr->disable_grain = true;
        return false;
    }

    grain_params.tex = img_tex(pass, img);
    if (!grain_params.tex)
        return false;

    img->sh = pl_dispatch_begin_ex(rr->dp, true);
    if (!pl_shader_av1_grain(img->sh, &rr->grain_state[plane_idx], &grain_params)) {
        pl_dispatch_abort(rr->dp, &img->sh);
        rr->disable_grain = true;
        return false;
    }

    img->tex = NULL;
    if (!img_tex(pass, img)) {
        PL_ERR(rr, "Failed applying AV1 grain.. disabling!");
        pl_dispatch_abort(rr->dp, &img->sh);
        img->tex = grain_params.tex;
        rr->disable_grain = true;
        return false;
    }

    img->repr = repr;
    return true;
}

static const enum pl_hook_stage plane_hook_stages[] = {
    [PLANE_ALPHA]   = PL_HOOK_ALPHA_INPUT,
    [PLANE_CHROMA]  = PL_HOOK_CHROMA_INPUT,
    [PLANE_LUMA]    = PL_HOOK_LUMA_INPUT,
    [PLANE_RGB]     = PL_HOOK_RGB_INPUT,
    [PLANE_XYZ]     = PL_HOOK_XYZ_INPUT,
};

static enum pl_lut_type guess_frame_lut_type(const struct pl_frame *frame,
                                             bool reversed)
{
    if (!frame->lut)
        return PL_LUT_UNKNOWN;
    if (frame->lut_type)
        return frame->lut_type;

    enum pl_color_system sys_in = frame->lut->repr_in.sys;
    enum pl_color_system sys_out = frame->lut->repr_out.sys;
    if (reversed)
        PL_SWAP(sys_in, sys_out);

    if (sys_in == PL_COLOR_SYSTEM_RGB && sys_out == sys_in)
        return PL_LUT_NORMALIZED;

    if (sys_in == frame->repr.sys && sys_out == PL_COLOR_SYSTEM_RGB)
        return PL_LUT_CONVERSION;

    // Unknown, just fall back to the default
    return PL_LUT_NATIVE;
}

static const struct pl_fmt *merge_fmt(struct pl_renderer *rr,
                                      const struct img *a, const struct img *b)
{
    const struct pl_fmt *fmta = a->tex ? a->tex->params.format : a->fmt;
    const struct pl_fmt *fmtb = b->tex->params.format;
    pl_assert(fmta && fmtb);
    if (fmta->type != fmtb->type)
        return NULL;

    int num_comps = PL_MIN(4, a->comps + b->comps);
    int min_depth = PL_MAX(a->repr.bits.sample_depth, b->repr.bits.sample_depth);

    // Only return formats that support all relevant caps of both formats
    const enum pl_fmt_caps mask = PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR;
    enum pl_fmt_caps req_caps = (fmta->caps & mask) | (fmtb->caps & mask);

    return pl_find_fmt(rr->gpu, fmta->type, num_comps, min_depth, 0, req_caps);
}

// Applies a series of rough heuristics to figure out whether we expect any
// performance gains from plane merging. This is basically a series of checks
// for operations that we *know* benefit from merged planes
static bool want_merge(struct pass_state *pass,
                       const struct plane_state *st,
                       const struct plane_state *ref,
                       const struct pl_render_params *params)
{
    const struct pl_renderer *rr = pass->rr;
    if (!rr->fbofmt[4])
        return false;

    // Debanding
    if (!rr->disable_debanding && params->deband_params)
        return true;

    // Other plane hooks, which are generally nontrivial
    enum pl_hook_stage stage = plane_hook_stages[st->type];
    for (int i = 0; i < params->num_hooks; i++) {
        if (params->hooks[i]->stages & stage)
            return true;
    }

    // Non-trivial scaling
    struct pl_sample_src src = {
        .new_w = ref->img.w,
        .new_h = ref->img.h,
        .rect = {
            .x1 = st->img.w,
            .y1 = st->img.h,
        },
    };

    struct sampler_info info = sample_src_info(pass->rr, &src, params);
    if (info.type == SAMPLER_COMPLEX)
        return true;

    // AV1 grain synthesis, can be merged for compatible channels, saving on
    // redundant sampling of the grain/offset textures
    struct pl_av1_grain_params grain_params = {
        .data = pass->image.av1_grain,
        .repr = (struct pl_color_repr *) &st->img.repr,
        .components = st->plane.components,
    };

    for (int c = 0; c < st->plane.components; c++)
        grain_params.component_mapping[c] = st->plane.component_mapping[c];

    if (!rr->disable_grain && pl_needs_av1_grain(&grain_params))
        return true;

    return false;
}

// This scales and merges all of the source images, and initializes pass->img.
static bool pass_read_image(struct pl_renderer *rr, struct pass_state *pass,
                            const struct pl_render_params *params)
{
    struct pl_frame *image = &pass->image;

    struct plane_state planes[4];
    struct plane_state *ref = &planes[pass->src_ref];

    for (int i = 0; i < image->num_planes; i++) {
        planes[i] = (struct plane_state) {
            .type = pass->src_type[i],
            .plane = image->planes[i],
            .img = {
                .w = image->planes[i].texture->params.w,
                .h = image->planes[i].texture->params.h,
                .tex = image->planes[i].texture,
                .repr = image->repr,
                .color = image->color,
                .comps = image->planes[i].components,
            },
        };
    }

    // Original ref texture, even after preprocessing
    const struct pl_tex *ref_tex = ref->plane.texture;

    // Merge all compatible planes into 'combined' shaders
    for (int i = 0; i < image->num_planes; i++) {
        struct plane_state *sti = &planes[i];
        if (!sti->type)
            continue;
        if (!want_merge(pass, sti, ref, params))
            continue;

        for (int j = i+1; j < image->num_planes; j++) {
            struct plane_state *stj = &planes[j];
            bool merge = sti->type == stj->type &&
                         sti->img.w == stj->img.w &&
                         sti->img.h == stj->img.h &&
                         sti->plane.shift_x == stj->plane.shift_x &&
                         sti->plane.shift_y == stj->plane.shift_y;
            if (!merge)
                continue;

            const struct pl_fmt *fmt = merge_fmt(rr, &sti->img, &stj->img);
            if (!fmt)
                continue;

            PL_TRACE(rr, "Merging plane %d into plane %d", j, i);
            struct pl_shader *sh = sti->img.sh;
            if (!sh) {
                sh = sti->img.sh = pl_dispatch_begin_ex(pass->rr->dp, true);
                GLSL("vec4 tmp; \n");
                pl_shader_sample_direct(sh, &(struct pl_sample_src) {
                    .tex = sti->img.tex,
                });
                sti->img.tex = NULL;
            }

            struct pl_shader *psh = pl_dispatch_begin_ex(pass->rr->dp, true);
            pl_shader_sample_direct(psh, &(struct pl_sample_src) {
                .tex = stj->img.tex,
            });

            ident_t sub = sh_subpass(sh, psh);
            pl_dispatch_abort(rr->dp, &psh);
            if (!sub)
                break; // skip merging

            GLSL("tmp = %s(); \n", sub);
            for (int jc = 0; jc < stj->img.comps; jc++) {
                int map = stj->plane.component_mapping[jc];
                if (!map)
                    continue;
                int ic = sti->img.comps++;
                pl_assert(ic < 4);
                GLSL("color[%d] = tmp[%d]; \n", ic, jc);
                sti->plane.components = sti->img.comps;
                sti->plane.component_mapping[ic] = map;
            }

            sti->img.fmt = fmt;
            *stj = (struct plane_state) {0};
        }

        if (!img_tex(pass, &sti->img)) {
            PL_ERR(rr, "Failed dispatching plane merging shader, disabling FBOs!");
            memset(rr->fbofmt, 0, sizeof(rr->fbofmt));
            return false;
        }
    }

    // Compute the sampling rc of each plane
    for (int i = 0; i < image->num_planes; i++) {
        struct plane_state *st = &planes[i];
        if (!st->type)
            continue;

        float rx = (float) ref_tex->params.w / st->plane.texture->params.w,
              ry = (float) ref_tex->params.h / st->plane.texture->params.h;

        // Only accept integer scaling ratios. This accounts for the fact that
        // fractionally subsampled planes get rounded up to the nearest integer
        // size, which we want to discard.
        float rrx = rx >= 1 ? roundf(rx) : 1.0 / roundf(1.0 / rx),
              rry = ry >= 1 ? roundf(ry) : 1.0 / roundf(1.0 / ry);

        float sx = st->plane.shift_x,
              sy = st->plane.shift_y;

        st->img.rect = (struct pl_rect2df) {
            .x0 = (image->crop.x0 - sx) / rrx,
            .y0 = (image->crop.y0 - sy) / rry,
            .x1 = (image->crop.x1 - sx) / rrx,
            .y1 = (image->crop.y1 - sy) / rry,
        };

        PL_TRACE(rr, "Plane %d:", i);
        log_plane_info(rr, st);

        // Perform AV1 grain synthesis if needed. Do this first because it
        // requires unmodified plane sizes, and also because it's closer to the
        // intent of the spec (which is to apply synthesis effectively during
        // decoding)

        if (plane_av1_grain(pass, i, st, ref, image, params)) {
            PL_TRACE(rr, "After AV1 grain:");
            log_plane_info(rr, st);
        }

        if (pass_hook(pass, &st->img, plane_hook_stages[st->type], params)) {
            PL_TRACE(rr, "After user hooks:");
            log_plane_info(rr, st);
        }

        // Update the conceptual width/height after applying plane shaders
        st->img.w = roundf(pl_rect_w(st->img.rect));
        st->img.h = roundf(pl_rect_h(st->img.rect));
    }

    struct pl_shader *sh = pl_dispatch_begin_ex(rr->dp, true);
    sh_require(sh, PL_SHADER_SIG_NONE, 0, 0);

    // Initialize the color to black
    const char *neutral = "0.0, 0.0, 0.0";
    if (pl_color_system_is_ycbcr_like(image->repr.sys))
        neutral = "0.0, 0.5, 0.5";

    GLSL("vec4 color = vec4(%s, 1.0);            \n"
         "// pass_read_image                     \n"
         "{                                      \n"
         "vec4 tmp;                              \n",
         neutral);

    // For quality reasons, explicitly drop subpixel offsets from the ref rect
    // and re-add them as part of `pass->img.rect`, always rounding towards 0.
    // Additionally, drop anamorphic subpixel mismatches.
    float off_x = ref->img.rect.x0 - truncf(ref->img.rect.x0),
          off_y = ref->img.rect.y0 - truncf(ref->img.rect.y0),
          stretch_x = roundf(pl_rect_w(ref->img.rect)) / pl_rect_w(ref->img.rect),
          stretch_y = roundf(pl_rect_h(ref->img.rect)) / pl_rect_h(ref->img.rect);

    bool has_alpha = false;
    for (int i = 0; i < image->num_planes; i++) {
        struct plane_state *st = &planes[i];
        const struct pl_plane *plane = &st->plane;
        if (!st->type)
            continue;

        float scale_x = pl_rect_w(st->img.rect) / pl_rect_w(ref->img.rect),
              scale_y = pl_rect_h(st->img.rect) / pl_rect_h(ref->img.rect),
              base_x = st->img.rect.x0 - scale_x * off_x,
              base_y = st->img.rect.y0 - scale_y * off_y;

        struct pl_sample_src src = {
            .tex        = st->img.tex,
            .components = plane->components,
            .address_mode = plane->address_mode,
            .scale      = pl_color_repr_normalize(&st->img.repr),
            .new_w      = ref->img.w,
            .new_h      = ref->img.h,
            .rect = {
                base_x,
                base_y,
                base_x + stretch_x * pl_rect_w(st->img.rect),
                base_y + stretch_y * pl_rect_h(st->img.rect),
            },
        };

        PL_TRACE(rr, "Aligning plane %d: {%f %f %f %f} -> {%f %f %f %f}",
                 i, st->img.rect.x0, st->img.rect.y0,
                 st->img.rect.x1, st->img.rect.y1,
                 src.rect.x0, src.rect.y0,
                 src.rect.x1, src.rect.y1);

        struct pl_shader *psh = pl_dispatch_begin_ex(rr->dp, true);
        if (deband_src(pass, psh, params, &src) != DEBAND_SCALED)
            dispatch_sampler(pass, psh, &rr->samplers_src[i], false, params, &src);

        ident_t sub = sh_subpass(sh, psh);
        if (!sub) {
            // Can't merge shaders, so instead force FBO indirection here
            struct img inter_img = {
                .sh = psh,
                .w = ref->img.w,
                .h = ref->img.h,
                .comps = src.components,
            };

            const struct pl_tex *inter_tex = img_tex(pass, &inter_img);
            if (!inter_tex) {
                PL_ERR(rr, "Failed dispatching subpass for plane.. disabling "
                       "all plane shaders");
                rr->disable_sampling = true;
                rr->disable_debanding = true;
                rr->disable_grain = true;
                pl_dispatch_abort(rr->dp, &sh);
                return false;
            }

            psh = pl_dispatch_begin_ex(rr->dp, true);
            pl_shader_sample_direct(psh, &(struct pl_sample_src) {
                .tex = inter_tex,
            });

            sub = sh_subpass(sh, psh);
            pl_assert(sub);
        }

        GLSL("tmp = %s();\n", sub);
        for (int c = 0; c < src.components; c++) {
            if (plane->component_mapping[c] < 0)
                continue;
            GLSL("color[%d] = tmp[%d];\n", plane->component_mapping[c], c);

            has_alpha |= plane->component_mapping[c] == PL_CHANNEL_A;
        }

        // we don't need it anymore
        pl_dispatch_abort(rr->dp, &psh);
    }

    GLSL("}\n");

    pass->img = (struct img) {
        .sh     = sh,
        .w      = ref->img.w,
        .h      = ref->img.h,
        .repr   = ref->img.repr,
        .color  = image->color,
        .comps  = has_alpha ? 4 : 3,
        .rect   = {
            off_x,
            off_y,
            off_x + pl_rect_w(ref->img.rect) / stretch_x,
            off_y + pl_rect_h(ref->img.rect) / stretch_y,
        },
    };

    // Update the reference rect to our adjusted image coordinates
    pass->ref_rect = pass->img.rect;

    pass_hook(pass, &pass->img, PL_HOOK_NATIVE,  params);

    // Apply LUT logic and colorspace conversion
    enum pl_lut_type lut_type = guess_frame_lut_type(image, false);
    sh = img_sh(pass, &pass->img);
    bool needs_conversion = true;

    if (lut_type == PL_LUT_NATIVE || lut_type == PL_LUT_CONVERSION) {
        // Fix bit depth normalization before applying LUT
        float scale = pl_color_repr_normalize(&pass->img.repr);
        GLSL("color *= vec4(%f); \n", scale);
        pl_shader_custom_lut(sh, image->lut, &rr->lut_state[LUT_IMAGE]);

        if (lut_type == PL_LUT_CONVERSION) {
            pass->img.repr.sys = PL_COLOR_SYSTEM_RGB;
            pass->img.repr.levels = PL_COLOR_LEVELS_FULL;
            needs_conversion = false;
        }
    }

    if (needs_conversion)
        pl_shader_decode_color(sh, &pass->img.repr, params->color_adjustment);
    if (lut_type == PL_LUT_NORMALIZED)
        pl_shader_custom_lut(sh, image->lut, &rr->lut_state[LUT_IMAGE]);

    pass_hook(pass, &pass->img, PL_HOOK_RGB, params);
    sh = NULL;

    // HDR peak detection, do this as early as possible
    hdr_update_peak(pass, params);
    return true;
}

static bool pass_scale_main(struct pl_renderer *rr, struct pass_state *pass,
                            const struct pl_render_params *params)
{
    if (!FBOFMT(pass->img.comps)) {
        PL_TRACE(rr, "Skipping main scaler (no FBOs)");
        return true;
    }

    struct img *img = &pass->img;
    struct pl_sample_src src = {
        .components = img->comps,
        .new_w      = abs(pl_rect_w(pass->dst_rect)),
        .new_h      = abs(pl_rect_h(pass->dst_rect)),
        .rect       = img->rect,
    };

    const struct pl_frame *image = &pass->image;
    bool need_fbo = image->num_overlays > 0;
    need_fbo |= rr->peak_detect_state && !params->allow_delayed_peak_detect;

    // Force FBO indirection if this shader is non-resizable
    int out_w, out_h;
    if (img->sh && pl_shader_output_size(img->sh, &out_w, &out_h))
        need_fbo |= out_w != src.new_w || out_h != src.new_h;

    struct sampler_info info = sample_src_info(rr, &src, params);
    bool use_sigmoid = info.dir == SAMPLER_UP && params->sigmoid_params;
    bool use_linear  = use_sigmoid || info.dir == SAMPLER_DOWN;

    // We need to enable the full rendering pipeline if there are any user
    // shaders / hooks that might depend on it.
    uint64_t scaling_hooks = PL_HOOK_PRE_OVERLAY | PL_HOOK_PRE_KERNEL |
                             PL_HOOK_POST_KERNEL;
    uint64_t linear_hooks = PL_HOOK_LINEAR | PL_HOOK_SIGMOID;

    for (int i = 0; i < params->num_hooks; i++) {
        if (params->hooks[i]->stages & (scaling_hooks | linear_hooks)) {
            need_fbo = true;
            if (params->hooks[i]->stages & linear_hooks)
                use_linear = true;
            if (params->hooks[i]->stages & PL_HOOK_SIGMOID)
                use_sigmoid = true;
        }
    }

    if (info.dir == SAMPLER_NOOP && !need_fbo) {
        pl_assert(src.new_w == img->w && src.new_h == img->h);
        PL_TRACE(rr, "Skipping main scaler (would be no-op)");
        return true;
    }

    if (info.type == SAMPLER_DIRECT && !need_fbo) {
        img->w = src.new_w;
        img->h = src.new_h;
        PL_TRACE(rr, "Skipping main scaler (free sampling)");
        return true;
    }

    // Hard-disable both sigmoidization and linearization when required
    if (params->disable_linear_scaling || rr->disable_linear_sdr)
        use_sigmoid = use_linear = false;

    // Avoid sigmoidization for HDR content because it clips to [0,1]
    if (pl_color_transfer_is_hdr(img->color.transfer)) {
        use_sigmoid = false;
        // Also disable linearization if necessary
        if (rr->disable_linear_hdr)
            use_linear = false;
    }

    if (use_linear) {
        pl_shader_linearize(img_sh(pass, img), img->color.transfer);
        img->color.transfer = PL_COLOR_TRC_LINEAR;
        pass_hook(pass, img, PL_HOOK_LINEAR, params);
    }

    if (use_sigmoid) {
        pl_shader_sigmoidize(img_sh(pass, img), params->sigmoid_params);
        pass_hook(pass, img, PL_HOOK_SIGMOID, params);
    }

    pass_hook(pass, img, PL_HOOK_PRE_OVERLAY, params);

    img->tex = img_tex(pass, img);
    if (!img->tex)
        return false;

    // Draw overlay on top of the intermediate image if needed, accounting
    // for possible stretching needed due to mismatch between the ref and src
    struct pl_transform2x2 tf = pl_transform2x2_identity;
    if (!pl_rect2d_eq(img->rect, image->crop)) {
        float rx = pl_rect_w(img->rect) / pl_rect_w(image->crop),
              ry = pl_rect_w(img->rect) / pl_rect_w(image->crop);

        tf = (struct pl_transform2x2) {
            .mat = {{{ rx, 0.0 }, { 0.0, ry }}},
            .c = {
                img->rect.x0 - image->crop.x0 * rx,
                img->rect.y0 - image->crop.y0 * ry
            },
        };
    }

    draw_overlays(pass, img->tex, img->comps, NULL, image->overlays,
                  image->num_overlays, img->color, img->repr, use_sigmoid,
                  &tf, params);

    pass_hook(pass, img, PL_HOOK_PRE_KERNEL, params);

    src.tex = img_tex(pass, img);
    struct pl_shader *sh = pl_dispatch_begin_ex(rr->dp, true);
    dispatch_sampler(pass, sh, &rr->sampler_main, false, params, &src);
    *img = (struct img) {
        .sh     = sh,
        .w      = src.new_w,
        .h      = src.new_h,
        .repr   = img->repr,
        .rect   = { 0, 0, src.new_w, src.new_h },
        .color  = img->color,
        .comps  = img->comps,
    };

    pass_hook(pass, img, PL_HOOK_POST_KERNEL, params);

    if (use_sigmoid)
        pl_shader_unsigmoidize(img_sh(pass, img), params->sigmoid_params);

    pass_hook(pass, img, PL_HOOK_SCALED, params);
    return true;
}

static bool pass_output_target(struct pl_renderer *rr, struct pass_state *pass,
                               const struct pl_render_params *params)
{
    const struct pl_frame *image = &pass->image;
    const struct pl_frame *target = &pass->target;
    struct img *img = &pass->img;
    struct pl_shader *sh = img_sh(pass, img);

    // Color management
    bool prelinearized = false;
    bool need_conversion = true;
    assert(image->color.primaries == img->color.primaries);
    assert(image->color.light == img->color.light);
    if (img->color.transfer == PL_COLOR_TRC_LINEAR)
        prelinearized = true;

    bool need_icc = (image->profile.data || target->profile.data) &&
                    !pl_icc_profile_equal(&image->profile, &target->profile);

    if (params->force_icc_lut || params->force_3dlut)
        need_icc |= !pl_color_space_equal(&image->color, &target->color);
    need_icc &= !rr->disable_icc;

    if (params->lut) {
        struct pl_color_space lut_in = params->lut->color_in;
        struct pl_color_space lut_out = params->lut->color_out;
        switch (params->lut_type) {
        case PL_LUT_UNKNOWN:
        case PL_LUT_NATIVE:
            pl_color_space_merge(&lut_in, &image->color);
            pl_color_space_merge(&lut_out, &image->color);
            break;
        case PL_LUT_CONVERSION:
            pl_color_space_merge(&lut_in, &image->color);
            pl_color_space_merge(&lut_out, &target->color);
            // Conversion LUT the highest priority
            need_icc = false;
            need_conversion = false;
            break;
        case PL_LUT_NORMALIZED:
            if (!prelinearized) {
                // PL_LUT_NORMALIZED wants linear input data
                pl_shader_linearize(sh, img->color.transfer);
                img->color.transfer = PL_COLOR_TRC_LINEAR;
                prelinearized = true;
            }
            pl_color_space_merge(&lut_in, &img->color);
            pl_color_space_merge(&lut_out, &img->color);
            break;
        }

        pl_shader_color_map(sh, params->color_map_params, image->color, lut_in,
                            NULL, prelinearized);

        if (params->lut_type == PL_LUT_NORMALIZED) {
            GLSLF("color.rgb *= vec3(1.0/%f); \n",
                  pl_color_transfer_nominal_peak(lut_in.transfer));
        }

        pl_shader_custom_lut(sh, params->lut, &rr->lut_state[LUT_PARAMS]);

        if (params->lut_type == PL_LUT_NORMALIZED) {
            GLSLF("color.rgb *= vec3(%f); \n",
                  pl_color_transfer_nominal_peak(lut_out.transfer));
        }

        if (params->lut_type != PL_LUT_CONVERSION) {
            pl_shader_color_map(sh, params->color_map_params, lut_out, img->color,
                                NULL, false);
        }
    }

#ifdef PL_HAVE_LCMS

    if (need_icc) {
        struct pl_icc_color_space src = {
            .color = image->color,
            .profile = image->profile,
        };

        struct pl_icc_color_space dst = {
            .color = target->color,
            .profile = target->profile,
        };

        struct pl_icc_result res;
        bool ok = pl_icc_update(sh, &src, &dst, &rr->icc_state, &res,
                                PL_DEF(params->icc_params, params->lut3d_params));
        if (!ok) {
            rr->disable_icc = true;
            goto fallback;
        }

        // current -> ICC in
        pl_shader_color_map(sh, params->color_map_params, image->color,
                            res.src_color, &rr->peak_detect_state, prelinearized);
        // ICC in -> ICC out
        pl_icc_apply(sh, &rr->icc_state);
        // ICC out -> target
        pl_shader_color_map(sh, params->color_map_params, res.dst_color,
                            target->color, NULL, false);

        need_conversion = false;
    }

fallback:

#else // !PL_HAVE_LCMS

    if (need_icc) {
        PL_WARN(rr, "An ICC profile was set, but libplacebo is built without "
                "support for LittleCMS! Disabling..");
        rr->disable_icc = true;
    }

#endif

    if (need_conversion) {
        // current -> target
        pl_shader_color_map(sh, params->color_map_params, image->color,
                            target->color, &rr->peak_detect_state, prelinearized);
    }

    // Apply color blindness simulation if requested
    if (params->cone_params)
        pl_shader_cone_distort(sh, target->color, params->cone_params);

    enum pl_lut_type lut_type = guess_frame_lut_type(target, true);
    if (lut_type == PL_LUT_NORMALIZED || lut_type == PL_LUT_CONVERSION)
        pl_shader_custom_lut(sh, target->lut, &rr->lut_state[LUT_TARGET]);

    // Apply the color scale separately, after encoding is done, to make sure
    // that the intermediate FBO (if any) has the correct precision.
    struct pl_color_repr repr = target->repr;
    float scale = pl_color_repr_normalize(&repr);
    if (lut_type != PL_LUT_CONVERSION)
        pl_shader_encode_color(sh, &repr);
    if (lut_type == PL_LUT_NATIVE)
        pl_shader_custom_lut(sh, target->lut, &rr->lut_state[LUT_TARGET]);
    pass_hook(pass, img, PL_HOOK_OUTPUT, params);
    sh = NULL;

    const struct pl_plane *ref = &target->planes[pass->dst_ref];
    bool flipped_x = pass->dst_rect.x1 < pass->dst_rect.x0,
         flipped_y = pass->dst_rect.y1 < pass->dst_rect.y0;

    for (int p = 0; p < target->num_planes; p++) {
        const struct pl_plane *plane = &target->planes[p];
        float rx = (float) plane->texture->params.w / ref->texture->params.w,
              ry = (float) plane->texture->params.h / ref->texture->params.h;

        // Only accept integer scaling ratios. This accounts for the fact
        // that fractionally subsampled planes get rounded up to the
        // nearest integer size, which we want to over-render.
        float rrx = rx >= 1 ? roundf(rx) : 1.0 / roundf(1.0 / rx),
              rry = ry >= 1 ? roundf(ry) : 1.0 / roundf(1.0 / ry);
        float sx = plane->shift_x, sy = plane->shift_y;

        struct pl_rect2df dst_rectf = {
            .x0 = (pass->dst_rect.x0 - sx) * rrx,
            .y0 = (pass->dst_rect.y0 - sy) * rry,
            .x1 = (pass->dst_rect.x1 - sx) * rrx,
            .y1 = (pass->dst_rect.y1 - sy) * rry,
        };

        // Normalize to make the math easier
        pl_rect2df_normalize(&dst_rectf);

        // Round the output rect
        int rx0 = floorf(dst_rectf.x0), ry0 = floorf(dst_rectf.y0),
            rx1 =  ceilf(dst_rectf.x1), ry1 =  ceilf(dst_rectf.y1);

        PL_TRACE(rr, "Subsampled target %d: {%f %f %f %f} -> {%d %d %d %d}",
                 p, dst_rectf.x0, dst_rectf.y0,
                 dst_rectf.x1, dst_rectf.y1,
                 rx0, ry0, rx1, ry1);

        if (target->num_planes > 1) {

            // Planar input, so we need to sample from an intermediate FBO
            struct pl_sample_src src = {
                .tex        = img_tex(pass, img),
                .new_w      = rx1 - rx0,
                .new_h      = ry1 - ry0,
                .rect = {
                    .x0 = (rx0 - dst_rectf.x0) / rrx,
                    .x1 = (rx1 - dst_rectf.x0) / rrx,
                    .y0 = (ry0 - dst_rectf.y0) / rry,
                    .y1 = (ry1 - dst_rectf.y0) / rry,
                },
            };

            if (!src.tex) {
                PL_ERR(rr, "Output requires multiple planes, but FBOs are "
                       "unavailable. This combination is unsupported.");
                return false;
            }

            PL_TRACE(rr, "Sampling %dx%d img aligned from {%f %f %f %f}",
                     pass->img.w, pass->img.h,
                     src.rect.x0, src.rect.y0,
                     src.rect.x1, src.rect.y1);

            for (int c = 0; c < plane->components; c++) {
                if (plane->component_mapping[c] < 0)
                    continue;
                src.component_mask |= 1 << plane->component_mapping[c];
            }

            sh = pl_dispatch_begin(rr->dp);
            dispatch_sampler(pass, sh, &rr->samplers_dst[p],
                             !plane->texture->params.storable, params, &src);

            GLSL("vec4 orig_color = color; \n");

        } else {

            // Single plane, so we can directly re-use the img shader unless
            // it's incompatible with the FBO capabilities
            bool is_comp = pl_shader_is_compute(img_sh(pass, img));
            if (is_comp && !plane->texture->params.storable) {
                if (!img_tex(pass, img)) {
                    PL_ERR(rr, "Rendering requires compute shaders, but output "
                           "is not storable, and FBOs are unavailable. This "
                           "combination is unsupported.");
                    return false;
                }
            }

            sh = img_sh(pass, img);
            img->sh = NULL;

        }

        GLSL("color *= vec4(1.0 / %f); \n", scale);
        swizzle_color(sh, plane->components, plane->component_mapping);

        if (params->dither_params) {
            // Ignore dithering for > 16-bit FBOs by default, since it makes
            // little sense to do so (and probably just adds errors)
            int depth = repr.bits.sample_depth;
            if (depth && (depth <= 16 || params->force_dither))
                pl_shader_dither(sh, depth, &rr->dither_state, params->dither_params);
        }

        bool ok = pl_dispatch_finish(rr->dp, &(struct pl_dispatch_params) {
            .shader = &sh,
            .target = plane->texture,
            .blend_params = params->blend_params,
            .rect = {
                .x0 = flipped_x ? rx1 : rx0,
                .y0 = flipped_y ? ry1 : ry0,
                .x1 = flipped_x ? rx0 : rx1,
                .y1 = flipped_y ? ry0 : ry1,
            },
        });

        if (!ok)
            return false;

        // Render any overlays, including overlays that need to be rendered
        // from the `image` itself, but which couldn't be rendered as
        // part of the intermediate scaling pass due to missing FBOs.
        if (image->num_overlays > 0 && !FBOFMT(img->comps)) {
            // The original image dimensions need to be scaled by the effective
            // end-to-end scaling ratio to compensate for the mismatch in
            // pixel coordinates between the image and target.
            float scale_x = pl_rect_w(dst_rectf) / pl_rect_w(image->crop),
                  scale_y = pl_rect_h(dst_rectf) / pl_rect_h(image->crop);

            struct pl_transform2x2 iscale = {
                .mat = {{{ scale_x, 0.0 }, { 0.0, scale_y }}},
                .c = {
                    // If the image was rendered with an offset relative to the
                    // target crop, we also need to shift the overlays.
                    dst_rectf.x0 - image->crop.x0 * scale_x,
                    dst_rectf.y0 - image->crop.y0 * scale_y,
                },
            };

            draw_overlays(pass, plane->texture, plane->components,
                          plane->component_mapping, image->overlays,
                          image->num_overlays, target->color, target->repr,
                          false, &iscale, params);
        }

        struct pl_transform2x2 tscale = {
            .mat = {{{ rrx, 0.0 }, { 0.0, rry }}},
            .c = { -sx, -sy },
        };

        draw_overlays(pass, plane->texture, plane->components,
                      plane->component_mapping, target->overlays,
                      target->num_overlays, target->color, target->repr,
                      false, &tscale, params);
    }

    *img = (struct img) {0};
    return true;
}

#define require(expr)                                                           \
  do {                                                                          \
      if (!(expr)) {                                                            \
          PL_ERR(rr, "Validation failed: %s (%s:%d)",                           \
                  #expr, __FILE__, __LINE__);                                   \
          return false;                                                         \
      }                                                                         \
  } while (0)

#define validate_plane(plane, param)                                            \
  do {                                                                          \
      require((plane).texture);                                                 \
      require((plane).texture->params.param);                                   \
      require((plane).components > 0 && (plane).components <= 4);               \
      for (int c = 0; c < (plane).components; c++) {                            \
          require((plane).component_mapping[c] >= PL_CHANNEL_NONE &&            \
                  (plane).component_mapping[c] <= PL_CHANNEL_A);                \
      }                                                                         \
  } while (0)

// Perform some basic validity checks on incoming structs to help catch invalid
// API usage. This is not an exhaustive check. In particular, enums are not
// bounds checked. This is because most functions accepting enums already
// abort() in the default case, and because it's not the intent of this check
// to catch all instances of memory corruption - just common logic bugs.
static bool validate_structs(struct pl_renderer *rr,
                             const struct pl_frame *image,
                             const struct pl_frame *target)
{
    // Rendering to/from a frame with no planes is technically allowed, but so
    // pointless that it's more likely to be a user error worth catching.
    require(image->num_planes > 0 && image->num_planes <= PL_MAX_PLANES);
    require(target->num_planes > 0 && target->num_planes <= PL_MAX_PLANES);
    for (int i = 0; i < image->num_planes; i++)
        validate_plane(image->planes[i], sampleable);
    for (int i = 0; i < target->num_planes; i++)
        validate_plane(target->planes[i], renderable);

    float src_w = pl_rect_w(image->crop), src_h = pl_rect_h(image->crop);
    float dst_w = pl_rect_w(target->crop), dst_h = pl_rect_h(target->crop);
    require(!src_w == !src_h);
    require(!dst_w == !dst_h);

    require(image->num_overlays >= 0);
    require(target->num_overlays >= 0);
    for (int i = 0; i < image->num_overlays; i++) {
        const struct pl_overlay *overlay = &image->overlays[i];
        validate_plane(overlay->plane, sampleable);
        require(pl_rect_w(overlay->rect) && pl_rect_h(overlay->rect));
    }
    for (int i = 0; i < target->num_overlays; i++) {
        const struct pl_overlay *overlay = &target->overlays[i];
        validate_plane(overlay->plane, sampleable);
        require(pl_rect_w(overlay->rect) && pl_rect_h(overlay->rect));
    }

    return true;
}

static inline enum plane_type detect_plane_type(const struct pl_plane *plane,
                                                const struct pl_color_repr *repr)
{
    if (pl_color_system_is_ycbcr_like(repr->sys)) {
        int t = PLANE_INVALID;
        for (int c = 0; c < plane->components; c++) {
            switch (plane->component_mapping[c]) {
            case PL_CHANNEL_Y: t = PL_MAX(t, PLANE_LUMA); continue;
            case PL_CHANNEL_A: t = PL_MAX(t, PLANE_ALPHA); continue;

            case PL_CHANNEL_CB:
            case PL_CHANNEL_CR:
                t = PL_MAX(t, PLANE_CHROMA);
                continue;

            default: continue;
            }
        }

        pl_assert(t);
        return t;
    }

    // Extra test for exclusive / separated alpha plane
    if (plane->components == 1 && plane->component_mapping[0] == PL_CHANNEL_A)
        return PLANE_ALPHA;

    switch (repr->sys) {
    case PL_COLOR_SYSTEM_UNKNOWN: // fall through to RGB
    case PL_COLOR_SYSTEM_RGB: return PLANE_RGB;
    case PL_COLOR_SYSTEM_XYZ: return PLANE_XYZ;
    default: abort();
    }
}

static inline void default_rect(struct pl_rect2df *rc,
                                const struct pl_rect2df *backup)
{
    if (!rc->x0 && !rc->y0 && !rc->x1 && !rc->y1)
        *rc = *backup;
}

static void fix_refs_and_rects(struct pass_state *pass, bool adjust_rects)
{
    struct pl_frame *image = &pass->image;
    struct pl_frame *target = &pass->target;

    // Find the ref planes
    for (int i = 0; i < image->num_planes; i++) {
        pass->src_type[i] = detect_plane_type(&image->planes[i], &image->repr);
        switch (pass->src_type[i]) {
        case PLANE_RGB:
        case PLANE_LUMA:
        case PLANE_XYZ:
            pass->src_ref = i;
            break;
        default: break;
        }
    }

    for (int i = 0; i < target->num_planes; i++) {
        pass->dst_type[i] = detect_plane_type(&target->planes[i], &target->repr);
        switch (pass->dst_type[i]) {
        case PLANE_RGB:
        case PLANE_LUMA:
        case PLANE_XYZ:
            pass->dst_ref = i;
            break;
        default: break;
        }
    }

    // Fix the rendering rects
    struct pl_rect2df *src = &image->crop, *dst = &target->crop;
    const struct pl_tex *src_ref = pass->image.planes[pass->src_ref].texture;
    const struct pl_tex *dst_ref = pass->target.planes[pass->dst_ref].texture;

    if ((!src->x0 && !src->x1) || (!src->y0 && !src->y1)) {
        src->x1 = src_ref->params.w;
        src->y1 = src_ref->params.h;
    };

    if ((!dst->x0 && !dst->x1) || (!dst->y0 && !dst->y1)) {
        dst->x1 = dst_ref->params.w;
        dst->y1 = dst_ref->params.h;
    }

    if (adjust_rects) {
        // Keep track of whether the end-to-end rendering is flipped
        bool flipped_x = (src->x0 > src->x1) != (dst->x0 > dst->x1),
             flipped_y = (src->y0 > src->y1) != (dst->y0 > dst->y1);

        // Normalize both rects to make the math easier
        pl_rect2df_normalize(src);
        pl_rect2df_normalize(dst);

        // Round the output rect and clip it to the framebuffer dimensions
        float rx0 = roundf(PL_MAX(dst->x0, 0.0)),
              ry0 = roundf(PL_MAX(dst->y0, 0.0)),
              rx1 = roundf(PL_MIN(dst->x1, dst_ref->params.w)),
              ry1 = roundf(PL_MIN(dst->y1, dst_ref->params.h));

        // Adjust the src rect corresponding to the rounded crop
        float scale_x = pl_rect_w(*src) / pl_rect_w(*dst),
              scale_y = pl_rect_h(*src) / pl_rect_h(*dst),
              base_x = src->x0,
              base_y = src->y0;

        src->x0 = base_x + (rx0 - dst->x0) * scale_x;
        src->x1 = base_x + (rx1 - dst->x0) * scale_x;
        src->y0 = base_y + (ry0 - dst->y0) * scale_y;
        src->y1 = base_y + (ry1 - dst->y0) * scale_y;

        // Update dst_rect to the rounded values and re-apply flip if needed. We
        // always do this in the `dst` rather than the `src`` because this allows
        // e.g. polar sampling compute shaders to work.
        *dst = (struct pl_rect2df) {
            .x0 = flipped_x ? rx1 : rx0,
            .y0 = flipped_y ? ry1 : ry0,
            .x1 = flipped_x ? rx0 : rx1,
            .y1 = flipped_y ? ry0 : ry1,
        };
    }

    // Copies of the above, for convenience
    pass->ref_rect = *src;
    pass->dst_rect = (struct pl_rect2d) {
        dst->x0, dst->y0, dst->x1, dst->y1,
    };
}

static const struct pl_tex *frame_ref(const struct pl_frame *frame)
{
    pl_assert(frame->num_planes);
    for (int i = 0; i < frame->num_planes; i++) {
        switch (detect_plane_type(&frame->planes[i], &frame->repr)) {
        case PLANE_RGB:
        case PLANE_LUMA:
        case PLANE_XYZ:
            return frame->planes[i].texture;
        default: continue;
        }
    }

    return frame->planes[0].texture;
}

static void fix_color_space(struct pl_frame *frame)
{
    const struct pl_tex *tex = frame_ref(frame);

    // If the primaries are not known, guess them based on the resolution
    if (!frame->color.primaries)
        frame->color.primaries = pl_color_primaries_guess(tex->params.w, tex->params.h);

    pl_color_space_infer(&frame->color);

    // For UNORM formats, we can infer the sampled bit depth from the texture
    // itself. This is ignored for other format types, because the logic
    // doesn't really work out for them anyways, and it's best not to do
    // anything too crazy unless the user provides explicit details.
    struct pl_bit_encoding *bits = &frame->repr.bits;
    if (!bits->sample_depth && tex->params.format->type == PL_FMT_UNORM) {
        // Just assume the first component's depth is canonical. This works in
        // practice, since for cases like rgb565 we want to use the lower depth
        // anyway. Plus, every format has at least one component.
        bits->sample_depth = tex->params.format->component_depth[0];

        // If we don't know the color depth, assume it spans the full range of
        // the texture. Otherwise, clamp it to the texture depth.
        bits->color_depth = PL_DEF(bits->color_depth, bits->sample_depth);
        bits->color_depth = PL_MIN(bits->color_depth, bits->sample_depth);

        // If the texture depth is higher than the known color depth, assume
        // the colors were left-shifted.
        bits->bit_shift += bits->sample_depth - bits->color_depth;
    }
}

static bool pass_infer_state(struct pass_state *pass, bool adjust_rects)
{
    // Backwards compatibility hacks
    struct pl_frame *image = &pass->image;
    struct pl_frame *target = &pass->target;
    default_rect(&image->crop, &image->src_rect);
    default_rect(&target->crop, &target->dst_rect);

    if (!target->num_planes && target->fbo) {
        target->num_planes = 1;
        target->planes[0] = (struct pl_plane) {
            .texture = target->fbo,
            .components = target->fbo->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        };
    }

    if (!validate_structs(pass->rr, image, target))
        return false;

    fix_refs_and_rects(pass, adjust_rects);
    fix_color_space(image);

    // Infer the target color space info based on the image's
    target->color.primaries = PL_DEF(target->color.primaries, image->color.primaries);
    target->color.transfer = PL_DEF(target->color.transfer, image->color.transfer);
    fix_color_space(target);
    return true;
}

bool pl_render_image(struct pl_renderer *rr, const struct pl_frame *pimage,
                     const struct pl_frame *ptarget,
                     const struct pl_render_params *params)
{
    params = PL_DEF(params, &pl_render_default_params);

    struct pass_state pass = {
        .rr = rr,
        .image = *pimage,
        .target = *ptarget,
    };

    if (!pass_infer_state(&pass, true))
        return false;

    pass.tmp = pl_tmp(NULL),
    pass.fbos_used = pl_calloc(pass.tmp, rr->fbos.num, sizeof(bool));

    // TODO: output caching
    pl_dispatch_reset_frame(rr->dp);

    for (int i = 0; i < params->num_hooks; i++) {
        if (params->hooks[i]->reset)
            params->hooks[i]->reset(params->hooks[i]->priv);
    }

    if (!pass_read_image(rr, &pass, params))
        goto error;

    if (!pass_scale_main(rr, &pass, params))
        goto error;

    if (!pass_output_target(rr, &pass, params))
        goto error;

    pl_free(pass.tmp);
    return true;

error:
    pl_dispatch_abort(rr->dp, &pass.img.sh);
    pl_free(pass.tmp);
    PL_ERR(rr, "Failed rendering image!");
    return false;
}

static uint64_t render_params_hash(const struct pl_render_params *params_orig)
{
    struct pl_render_params params = *params_orig;
    uint64_t hash = 0;

#define HASH_PTR(ptr)                                                           \
    do {                                                                        \
        if (ptr) {                                                              \
            pl_hash_merge(&hash, pl_mem_hash(ptr, sizeof(*ptr)));               \
            ptr = NULL;                                                         \
        }                                                                       \
    } while (0)

#define HASH_FILTER(scaler)                                                     \
    do {                                                                        \
        if (scaler) {                                                           \
            struct pl_filter_config filter = *scaler;                           \
            HASH_PTR(filter.kernel);                                            \
            HASH_PTR(filter.window);                                            \
            pl_hash_merge(&hash, pl_mem_hash(&filter, sizeof(filter)));         \
            scaler = NULL;                                                      \
        }                                                                       \
    } while (0)

    HASH_FILTER(params.upscaler);
    HASH_FILTER(params.downscaler);
    HASH_FILTER(params.frame_mixer);

    HASH_PTR(params.deband_params);
    HASH_PTR(params.sigmoid_params);
    HASH_PTR(params.color_adjustment);
    HASH_PTR(params.peak_detect_params);
    HASH_PTR(params.color_map_params);
    HASH_PTR(params.dither_params);
    HASH_PTR(params.cone_params);
    HASH_PTR(params.blend_params);

#ifdef PL_HAVE_LCMS
    HASH_PTR(params.icc_params);
    HASH_PTR(params.lut3d_params);
#endif

    // Hash all hooks
    for (int i = 0; i < params.num_hooks; i++)
        pl_hash_merge(&hash, pl_mem_hash(&params.hooks[i], sizeof(params.hooks[i])));
    params.hooks = NULL;

    // Hash the LUT by only looking at the signature
    if (params.lut) {
        pl_hash_merge(&hash, params.lut->signature);
        params.lut = NULL;
    }

    pl_hash_merge(&hash, pl_mem_hash(&params, sizeof(params)));
    return hash;
}

#define MAX_MIX_FRAMES 16

bool pl_render_image_mix(struct pl_renderer *rr, const struct pl_frame_mix *images,
                         const struct pl_frame *ptarget,
                         const struct pl_render_params *params)
{
    params = PL_DEF(params, &pl_render_default_params);
    uint64_t params_hash = render_params_hash(params);

    require(images->num_frames >= 1);
    for (int i = 0; i < images->num_frames - 1; i++)
        require(images->timestamps[i] <= images->timestamps[i+1]);

    struct pass_state pass = {
        .rr = rr,
        .image = *images->frames[0],
        .target = *ptarget,
    };

    // As the canonical reference, find the nearest frame that would be
    // currently visible on an idealized zero-order-hold display.
    for (int i = 1; i < images->num_frames; i++) {
        if (images->timestamps[i] <= 0.0)
            pass.image = *images->frames[i];
    }

    if (!params->frame_mixer || rr->disable_mixing || !FBOFMT(4))
        goto fallback;

    if (!pass_infer_state(&pass, false))
        return false;

    int out_w = abs(pl_rect_w(pass.dst_rect)),
        out_h = abs(pl_rect_h(pass.dst_rect));

    // The color space to mix the frames in. We arbitrarily choose to use the
    // "current" frame's color space, but converted to RGB.
    //
    // TODO: Maybe mix in linear light instead of the native colorspace?
    const struct pl_color_space mix_color = pass.image.color;
    static const struct pl_color_repr mix_repr = {
        .sys = PL_COLOR_SYSTEM_RGB,
        .levels = PL_COLOR_LEVELS_PC,
        .alpha = PL_ALPHA_PREMULTIPLIED,
    };

    int fidx = 0;
    struct cached_frame frames[MAX_MIX_FRAMES];
    float weights[MAX_MIX_FRAMES];
    float wsum = 0.0;

    // Garbage collect the cache by evicting all frames from the cache that are
    // not determined to still be required
    for (int i = 0; i < rr->frames.num; i++)
        rr->frames.elem[i].evict = true;

    // Traverse the input frames and determine/prepare the ones we need
    for (int i = 0; i < images->num_frames; i++) {
        uint64_t sig = images->signatures[i];
        float pts = images->timestamps[i];
        PL_TRACE(rr, "Considering image with signature 0x%llx, pts %f",
                 (unsigned long long) sig, pts);

        float weight;
        if (params->frame_mixer->kernel) {

            float radius = params->frame_mixer->kernel->radius;
            if (fabs(pts) >= radius) {
                PL_TRACE(rr, "  -> Skipping: outside filter radius (%f)", radius);
                continue;
            }

            // Weight is directly sampled from the filter
            weight = pl_filter_sample(params->frame_mixer, pts);
            PL_TRACE(rr, "  -> Filter offset %f = weight %f", pts, weight);

        } else {

            // Compute the visible interval [pts, end] of this frame
            float end = i+1 < images->num_frames ? images->timestamps[i+1] : INFINITY;
            if (pts > images->vsync_duration || end < 0.0) {
                PL_TRACE(rr, "  -> Skipping: no intersection with vsync");
                continue;
            } else {
                pts = PL_MAX(pts, 0.0);
                end = PL_MIN(end, images->vsync_duration);
                pl_assert(end >= pts);
            }

            // Weight is the fraction of vsync interval that frame is visible
            weight = (end - pts) / images->vsync_duration;
            PL_TRACE(rr, "  -> Frame [%f, %f] intersects [%f, %f] = weight %f",
                     pts, end, 0.0, images->vsync_duration, weight);

        }

        struct cached_frame *f = NULL;
        for (int j = 0; j < rr->frames.num; j++) {
            if (rr->frames.elem[j].signature == sig) {
                f = &rr->frames.elem[j];
                f->evict = false;
                break;
            }
        }

        // Skip frames with negligible contributions. Do this after the loop
        // above to make sure these frames don't get evicted just yet.
        const float cutoff = 1e-3;
        if (fabs(weight) <= cutoff) {
            PL_TRACE(rr, "   -> Skipping: weight (%f) below threshold (%f)",
                     weight, cutoff);
            continue;
        }

        if (!f) {
            // Signature does not exist in the cache at all yet,
            // so grow the cache by this entry.
            PL_ARRAY_GROW(rr, rr->frames);
            f = &rr->frames.elem[rr->frames.num++];
            *f = (struct cached_frame) {
                .signature = sig,
                .color = images->frames[i]->color,
                .profile = images->frames[i]->profile,
            };
        }

        // Check to see if we can blindly reuse this cache entry. This is the
        // case if either the params are compatible, or the user doesn't care
        bool can_reuse = f->tex;
        if (can_reuse && !params->preserve_mixing_cache) {
            can_reuse = f->tex->params.w == out_w &&
                        f->tex->params.h == out_h &&
                        f->params_hash == params_hash;
        }

        if (!can_reuse) {
            // If we can't reuse the entry, we need to render to this
            // texture first
            PL_TRACE(rr, "  -> Cached texture missing or invalid.. (re)creating");
            if (!f->tex) {
                if (PL_ARRAY_POP(rr->frame_fbos, &f->tex))
                    pl_tex_invalidate(rr->gpu, f->tex);
            }
            bool ok = pl_tex_recreate(rr->gpu, &f->tex, &(struct pl_tex_params) {
                .w = out_w,
                .h = out_h,
                .format = rr->fbofmt[4],
                .sampleable = true,
                .renderable = true,
                .storable = rr->fbofmt[4]->caps & PL_FMT_CAP_STORABLE,
            });

            if (!ok) {
                PL_ERR(rr, "Could not create intermediate texture for "
                       "frame mixing.. disabling!");
                rr->disable_mixing = true;
                goto fallback;
            }

            // In the intermediate frame cache, we store all images as RGB, but
            // in their native colorspaces. Preserving the original colorspace
            // avoids precision loss due to unnecessary color space roundtrips.
            // We also explicitly clear the ICC profile, see below for why.
            struct pl_frame image = *images->frames[i];
            image.profile = (struct pl_icc_profile) {0};

            struct pl_frame inter_target = {
                .num_planes = 1,
                .planes[0] = {
                    .texture = f->tex,
                    .components = rr->fbofmt[4]->num_components,
                    .component_mapping = {0, 1, 2, 3},
                },
                .color = f->color,
                .repr = mix_repr,
            };

            if (!pl_render_image(rr, &image, &inter_target, params)) {
                PL_ERR(rr, "Could not render image for frame mixing.. disabling!");
                rr->disable_mixing = true;
                goto fallback;
            }

            f->params_hash = params_hash;
        }

        pl_assert(fidx < MAX_MIX_FRAMES);
        frames[fidx] = *f;
        weights[fidx] = weight;
        wsum += weight;
        fidx++;
    }

    // Evict the frames we *don't* need
    for (int i = 0; i < rr->frames.num; ) {
        if (rr->frames.elem[i].evict) {
            PL_TRACE(rr, "Evicting frame with signature %llx from cache",
                     (unsigned long long) rr->frames.elem[i].signature);
            PL_ARRAY_APPEND(rr, rr->frame_fbos, rr->frames.elem[i].tex);
            PL_ARRAY_REMOVE_AT(rr->frames, i);
            continue;
        } else {
            i++;
        }
    }

    // Sample and mix the output color
    struct pl_shader *sh = pl_dispatch_begin(rr->dp);
    sh->res.output = PL_SHADER_SIG_COLOR;
    sh->output_w = out_w;
    sh->output_h = out_h;

    GLSL("vec4 color;                   \n"
         "// pl_render_image_mix        \n"
         "{                             \n"
         "vec4 mix_color = vec4(0.0);   \n");

    for (int i = 0; i < fidx; i++) {
        const struct pl_tex_params *tpars = &frames[i].tex->params;

        // Use linear sampling if desired and possible
        enum pl_tex_sample_mode sample_mode = PL_TEX_SAMPLE_NEAREST;
        if ((tpars->w != out_w || tpars->h != out_h) &&
            (tpars->format->caps & PL_FMT_CAP_LINEAR))
        {
            sample_mode = PL_TEX_SAMPLE_LINEAR;
        }

        ident_t pos, tex = sh_bind(sh, frames[i].tex, PL_TEX_ADDRESS_CLAMP,
                                   sample_mode, "frame", NULL, &pos, NULL, NULL);

        GLSL("color = %s(%s, %s); \n", sh_tex_fn(sh, *tpars), tex, pos);

        // Note: This ignores differences in ICC profile, which we decide to
        // just simply not care about. Doing that properly would require
        // converting between different image profiles, and the headache of
        // finagling that state is just not worth it because this is an
        // exceptionally unlikely hypothetical.
        pl_shader_color_map(sh, NULL, frames[i].color, mix_color, NULL, false);

        ident_t weight = sh_var(sh, (struct pl_shader_var) {
            .var = pl_var_float("weight"),
            .data = &(float){ weights[i] / wsum },
            .dynamic = true,
        });

        GLSL("mix_color += %s * color; \n", weight);
    }

    GLSL("color = mix_color; \n"
         "}                  \n");

    // Dispatch this to the destination
    pass.tmp = pl_tmp(NULL),
    pass.fbos_used = pl_calloc(pass.tmp, rr->fbos.num, sizeof(bool));
    pass.img = (struct img) {
        .sh = sh,
        .w = out_w,
        .h = out_h,
        .comps = 4,
        .color = mix_color,
        .repr = mix_repr,
    };

    if (!pass_output_target(rr, &pass, params)) {
        pl_free(pass.tmp);
        goto fallback;
    }

    pl_free(pass.tmp);
    return true;

fallback:
    return pl_render_image(rr, &pass.image, ptarget, params);
}

void pl_frame_set_chroma_location(struct pl_frame *frame,
                                  enum pl_chroma_location chroma_loc)
{
    const struct pl_tex *ref = frame_ref(frame);

    if (ref) {
        // Texture dimensions are already known, so apply the chroma location
        // only to subsampled planes
        int ref_w = ref->params.w, ref_h = ref->params.h;

        for (int i = 0; i < frame->num_planes; i++) {
            struct pl_plane *plane = &frame->planes[i];
            const struct pl_tex *tex = plane->texture;
            bool subsampled = tex->params.w < ref_w || tex->params.h < ref_h;
            if (subsampled)
                pl_chroma_location_offset(chroma_loc, &plane->shift_x, &plane->shift_y);
        }
    } else {
        // Texture dimensions are not yet known, so apply the chroma location
        // to all chroma planes, regardless of subsampling
        for (int i = 0; i < frame->num_planes; i++) {
            struct pl_plane *plane = &frame->planes[i];
            if (detect_plane_type(plane, &frame->repr) == PLANE_CHROMA)
                pl_chroma_location_offset(chroma_loc, &plane->shift_x, &plane->shift_y);
        }
    }
}

void pl_frame_from_swapchain(struct pl_frame *out_frame,
                             const struct pl_swapchain_frame *frame)
{
    const struct pl_tex *fbo = frame->fbo;
    *out_frame = (struct pl_frame) {
        .num_planes = 1,
        .planes = {{
            .texture = fbo,
            .components = fbo->params.format->num_components,
            .component_mapping = {0, 1, 2, 3},
        }},
        .crop = { 0, 0, fbo->params.w, fbo->params.h },
        .repr = frame->color_repr,
        .color = frame->color_space,
    };

    if (frame->flipped)
        PL_SWAP(out_frame->crop.y0, out_frame->crop.y1);
}

bool pl_frame_is_cropped(const struct pl_frame *frame)
{
    int x0 = roundf(PL_MIN(frame->crop.x0, frame->crop.x1)),
        y0 = roundf(PL_MIN(frame->crop.y0, frame->crop.y1)),
        x1 = roundf(PL_MAX(frame->crop.x0, frame->crop.x1)),
        y1 = roundf(PL_MAX(frame->crop.y0, frame->crop.y1));

    const struct pl_tex *ref = frame_ref(frame);
    pl_assert(ref);

    if (!x0 && !x1)
        x1 = ref->params.w;
    if (!y0 && !y1)
        y1 = ref->params.h;

    return x0 > 0 || y0 > 0 || x1 < ref->params.w || y1 < ref->params.h;
}

void pl_frame_clear(const struct pl_gpu *gpu, const struct pl_frame *frame,
                    const float rgb[3])
{
    struct pl_color_repr repr = frame->repr;
    struct pl_transform3x3 tr = pl_color_repr_decode(&repr, NULL);
    pl_transform3x3_invert(&tr);

    float encoded[3] = { rgb[0], rgb[1], rgb[2] };
    pl_transform3x3_apply(&tr, encoded);

    for (int p = 0; p < frame->num_planes; p++) {
        const struct pl_plane *plane =  &frame->planes[p];
        float clear[4] = { 0.0, 0.0, 0.0, 1.0 };
        for (int c = 0; c < plane->components; c++) {
            if (plane->component_mapping[c] >= 0)
                clear[c] = encoded[plane->component_mapping[c]];
        }

        pl_tex_clear(gpu, plane->texture, clear);
    }
}
