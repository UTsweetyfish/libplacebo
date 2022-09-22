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
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "context.h"
#include "shaders.h"
#include "dispatch.h"
#include "gpu.h"

// Maximum number of passes to keep around at once. If full, passes older than
// MIN_AGE are evicted to make room. (Failing that, the cache size doubles)
#define MAX_PASSES 100
#define MIN_AGE 10

enum {
    TMP_PRELUDE,   // GLSL version, global definitions, etc.
    TMP_MAIN,      // main GLSL shader body
    TMP_VERT_HEAD, // vertex shader inputs/outputs
    TMP_VERT_BODY, // vertex shader body
    TMP_COUNT,
};

struct pl_dispatch {
    struct pl_context *ctx;
    const struct pl_gpu *gpu;
    uint8_t current_ident;
    uint8_t current_index;
    int max_passes;

    PL_ARRAY(struct pl_shader *) shaders;       // to avoid re-allocations
    PL_ARRAY(struct pass *) passes;             // compiled passes
    PL_ARRAY(struct cached_pass) cached_passes; // not-yet-compiled passes

    // temporary buffers to help avoid re_allocations during pass creation
    pl_str tmp[TMP_COUNT];
};

enum pass_var_type {
    PASS_VAR_NONE = 0,
    PASS_VAR_GLOBAL, // regular/global uniforms (PL_GPU_CAP_INPUT_VARIABLES)
    PASS_VAR_UBO,    // uniform buffers
    PASS_VAR_PUSHC   // push constants
};

// Cached metadata about a variable's effective placement / update method
struct pass_var {
    int index; // for pl_var_update
    enum pass_var_type type;
    struct pl_var_layout layout;
    void *cached_data;
};

struct pass {
    uint64_t signature; // as returned by pl_shader_signature
    const struct pl_pass *pass;
    int last_index;

    // contains cached data and update metadata, same order as pl_shader
    struct pass_var *vars;

    // for uniform buffer updates
    const struct pl_buf *ubo;
    struct pl_shader_desc ubo_desc; // temporary

    // Cached pl_pass_run_params. This will also contain mutable allocations
    // for the push constants, descriptor bindings (including the binding for
    // the UBO pre-filled), vertex array and variable updates
    struct pl_pass_run_params run_params;
};

struct cached_pass {
    uint64_t signature;
    const uint8_t *cached_program;
    size_t cached_program_len;
};

static void pass_destroy(struct pl_dispatch *dp, struct pass *pass)
{
    if (!pass)
        return;

    pl_buf_destroy(dp->gpu, &pass->ubo);
    pl_pass_destroy(dp->gpu, &pass->pass);
    pl_free(pass);
}

struct pl_dispatch *pl_dispatch_create(struct pl_context *ctx,
                                       const struct pl_gpu *gpu)
{
    pl_assert(ctx);
    struct pl_dispatch *dp = pl_zalloc_ptr(ctx, dp);
    dp->ctx = ctx;
    dp->gpu = gpu;
    dp->max_passes = MAX_PASSES;

    return dp;
}

void pl_dispatch_destroy(struct pl_dispatch **ptr)
{
    struct pl_dispatch *dp = *ptr;
    if (!dp)
        return;

    for (int i = 0; i < dp->passes.num; i++)
        pass_destroy(dp, dp->passes.elem[i]);
    for (int i = 0; i < dp->shaders.num; i++)
        pl_shader_free(&dp->shaders.elem[i]);

    pl_free(dp);
    *ptr = NULL;
}

struct pl_shader *pl_dispatch_begin_ex(struct pl_dispatch *dp, bool unique)
{
    struct pl_shader_params params = {
        .id = unique ? dp->current_ident++ : 0,
        .gpu = dp->gpu,
        .index = dp->current_index,
    };

    struct pl_shader *sh;
    if (PL_ARRAY_POP(dp->shaders, &sh)) {
        pl_shader_reset(sh, &params);
        return sh;
    }

    return pl_shader_alloc(dp->ctx, &params);
}

void pl_dispatch_reset_frame(struct pl_dispatch *dp)
{
    dp->current_ident = 0;
    dp->current_index++;
}

struct pl_shader *pl_dispatch_begin(struct pl_dispatch *dp)
{
    return pl_dispatch_begin_ex(dp, false);
}

static bool add_pass_var(struct pl_dispatch *dp, void *tmp, struct pass *pass,
                         struct pl_pass_params *params,
                         const struct pl_shader_var *sv, struct pass_var *pv,
                         bool greedy)
{
    const struct pl_gpu *gpu = dp->gpu;
    if (pv->type)
        return true;

    // Try not to use push constants for "large" values like matrices in the
    // first pass, since this is likely to exceed the VGPR/pushc size budgets
    bool try_pushc = greedy || (sv->var.dim_m == 1 && sv->var.dim_a == 1) || sv->dynamic;
    if (try_pushc && gpu->glsl.vulkan && gpu->limits.max_pushc_size) {
        pv->layout = pl_std430_layout(params->push_constants_size, &sv->var);
        size_t new_size = pv->layout.offset + pv->layout.size;
        if (new_size <= gpu->limits.max_pushc_size) {
            params->push_constants_size = new_size;
            pv->type = PASS_VAR_PUSHC;
            return true;
        }
    }

    // If we haven't placed all PCs yet, don't place anything else, since
    // we want to try and fit more stuff into PCs before "giving up"
    if (!greedy)
        return true;

    // Attempt using uniform buffer next. The GLSL version 440 check is due
    // to explicit offsets on UBO entries. In theory we could leave away
    // the offsets and support UBOs for older GL as well, but this is a nice
    // safety net for driver bugs (and also rules out potentially buggy drivers)
    // Also avoid UBOs for highly dynamic stuff since that requires synchronizing
    // the UBO writes every frame
    bool try_ubo = !(gpu->caps & PL_GPU_CAP_INPUT_VARIABLES) || !sv->dynamic;
    if (try_ubo && gpu->glsl.version >= 440 && gpu->limits.max_ubo_size) {
        if (sh_buf_desc_append(tmp, gpu, &pass->ubo_desc, &pv->layout, sv->var)) {
            pv->type = PASS_VAR_UBO;
            return true;
        }
    }

    // Otherwise, use global uniforms
    if (gpu->caps & PL_GPU_CAP_INPUT_VARIABLES) {
        pv->type = PASS_VAR_GLOBAL;
        pv->index = params->num_variables;
        pv->layout = pl_var_host_layout(0, &sv->var);
        PL_ARRAY_APPEND_RAW(tmp, params->variables, params->num_variables, sv->var);
        return true;
    }

    // Ran out of variable binding methods. The most likely scenario in which
    // this can happen is if we're using a GPU that does not support global
    // input vars and we've exhausted the UBO size limits.
    PL_ERR(dp, "Unable to add input variable '%s': possibly exhausted "
           "UBO size limits?", sv->var.name);
    return false;
}

#define ADD(x, ...) pl_str_append_asprintf_c(dp, (x), __VA_ARGS__)
#define ADD_STR(x, s) pl_str_append(dp, (x), (s))

static void add_var(struct pl_dispatch *dp, pl_str *body,
                    const struct pl_var *var)
{
    ADD(body, "%s %s", pl_var_glsl_type_name(*var), var->name);

    if (var->dim_a > 1) {
        ADD(body, "[%d];\n", var->dim_a);
    } else {
        ADD(body, ";\n");
    }
}

static int cmp_buffer_var(const void *pa, const void *pb)
{
    const struct pl_buffer_var * const *a = pa, * const *b = pb;
    return PL_CMP((*a)->layout.offset, (*b)->layout.offset);
}

static void add_buffer_vars(struct pl_dispatch *dp, pl_str *body,
                            const struct pl_buffer_var *vars, int num,
                            void *tmp)
{
    // Sort buffer vars
    const struct pl_buffer_var **sorted_vars = pl_calloc_ptr(tmp, num, sorted_vars);
    for (int i = 0; i < num; i++)
        sorted_vars[i] = &vars[i];
    qsort(sorted_vars, num, sizeof(sorted_vars[0]), cmp_buffer_var);

    ADD(body, "{\n");
    for (int i = 0; i < num; i++) {
        // Add an explicit offset wherever possible
        if (dp->gpu->glsl.version >= 440)
            ADD(body, "    layout(offset=%zu) ", sorted_vars[i]->layout.offset);
        add_var(dp, body, &sorted_vars[i]->var);
    }
    ADD(body, "};\n");
}

static ident_t sh_var_from_va(struct pl_shader *sh, const char *name,
                              const struct pl_vertex_attrib *va,
                              const void *data)
{
    return sh_var(sh, (struct pl_shader_var) {
        .var  = pl_var_from_fmt(va->fmt, name),
        .data = data,
    });
}

static inline struct pl_desc_binding sd_binding(const struct pl_shader_desc sd)
{
    // For backwards compatbility with the deprecated field sd.object
    struct pl_desc_binding binding = sd.binding;
    binding.object = PL_DEF(binding.object, sd.object);
    return binding;
}

static void generate_shaders(struct pl_dispatch *dp, struct pass *pass,
                             struct pl_pass_params *params, struct pl_shader *sh,
                             ident_t vert_pos, ident_t out_proj, void *tmp)
{
    const struct pl_gpu *gpu = dp->gpu;
    const struct pl_shader_res *res = pl_shader_finalize(sh);

    pl_str *pre = &dp->tmp[TMP_PRELUDE];
    ADD(pre, "#version %d%s\n", gpu->glsl.version,
        (gpu->glsl.gles && gpu->glsl.version > 100) ? " es" : "");
    if (params->type == PL_PASS_COMPUTE)
        ADD(pre, "#extension GL_ARB_compute_shader : enable\n");

    // Enable this unconditionally if the GPU supports it, since we have no way
    // of knowing whether subgroups are being used or not
    if (gpu->caps & PL_GPU_CAP_SUBGROUPS) {
        ADD(pre, "#extension GL_KHR_shader_subgroup_basic : enable \n"
                 "#extension GL_KHR_shader_subgroup_vote : enable \n"
                 "#extension GL_KHR_shader_subgroup_arithmetic : enable \n"
                 "#extension GL_KHR_shader_subgroup_ballot : enable \n"
                 "#extension GL_KHR_shader_subgroup_shuffle : enable \n");
    }

    // Enable all extensions needed for different types of input
    bool has_ssbo = false, has_ubo = false, has_img = false, has_texel = false,
         has_ext = false, has_nofmt = false;
    for (int i = 0; i < sh->descs.num; i++) {
        switch (sh->descs.elem[i].desc.type) {
        case PL_DESC_BUF_UNIFORM: has_ubo = true; break;
        case PL_DESC_BUF_STORAGE: has_ssbo = true; break;
        case PL_DESC_BUF_TEXEL_UNIFORM: has_texel = true; break;
        case PL_DESC_BUF_TEXEL_STORAGE: {
            const struct pl_buf *buf = sd_binding(res->descriptors[i]).object;
            has_nofmt |= !buf->params.format->glsl_format;
            has_texel = true;
            break;
        }
        case PL_DESC_STORAGE_IMG: {
            const struct pl_tex *tex = sd_binding(res->descriptors[i]).object;
            has_nofmt |= !tex->params.format->glsl_format;
            has_img = true;
            break;
        }
        case PL_DESC_SAMPLED_TEX: {
            const struct pl_tex *tex = sd_binding(res->descriptors[i]).object;
            switch (tex->sampler_type) {
            case PL_SAMPLER_NORMAL: break;
            case PL_SAMPLER_RECT: break;
            case PL_SAMPLER_EXTERNAL: has_ext = true; break;
            default: abort();
            }
            break;
        }
        default: break;
        }
    }

    if (has_img)
        ADD(pre, "#extension GL_ARB_shader_image_load_store : enable\n");
    if (has_ubo)
        ADD(pre, "#extension GL_ARB_uniform_buffer_object : enable\n");
    if (has_ssbo)
        ADD(pre, "#extension GL_ARB_shader_storage_buffer_object : enable\n");
    if (has_texel)
        ADD(pre, "#extension GL_ARB_texture_buffer_object : enable\n");
    if (has_ext)
        ADD(pre, "#extension GL_OES_EGL_image_external : enable\n");
    if (has_nofmt)
        ADD(pre, "#extension GL_EXT_shader_image_load_formatted : enable\n");

    if (gpu->glsl.gles) {
        // Use 32-bit precision for floats if possible
        ADD(pre, "#ifdef GL_FRAGMENT_PRECISION_HIGH \n"
                 "precision highp float;            \n"
                 "#else                             \n"
                 "precision mediump float;          \n"
                 "#endif                            \n");

        // Always use 16-bit precision for samplers
        ADD(pre, "precision mediump sampler2D; \n");
        if (gpu->limits.max_tex_1d_dim)
            ADD(pre, "precision mediump sampler1D; \n");
        if (gpu->limits.max_tex_3d_dim && gpu->glsl.version > 100)
            ADD(pre, "precision mediump sampler3D; \n");
    }

    // Add all of the push constants as their own element
    if (params->push_constants_size) {
        // We re-use add_buffer_vars to make sure variables are sorted, this
        // is important because the push constants can be out-of-order in
        // `pass->vars`
        PL_ARRAY(struct pl_buffer_var) pc_bvars = {0};
        for (int i = 0; i < res->num_variables; i++) {
            if (pass->vars[i].type != PASS_VAR_PUSHC)
                continue;

            PL_ARRAY_APPEND(tmp, pc_bvars, (struct pl_buffer_var) {
                .var = res->variables[i].var,
                .layout = pass->vars[i].layout,
            });
        }

        ADD(pre, "layout(std430, push_constant) uniform PushC ");
        add_buffer_vars(dp, pre, pc_bvars.elem, pc_bvars.num, tmp);
    }

    // Add all of the required descriptors
    for (int i = 0; i < res->num_descriptors; i++) {
        const struct pl_shader_desc *sd = &res->descriptors[i];
        const struct pl_desc *desc = &params->descriptors[i];

        switch (desc->type) {
        case PL_DESC_SAMPLED_TEX: {
            static const char *types[][4] = {
                [PL_SAMPLER_NORMAL][1]  = "sampler1D",
                [PL_SAMPLER_NORMAL][2]  = "sampler2D",
                [PL_SAMPLER_NORMAL][3]  = "sampler3D",
                [PL_SAMPLER_RECT][2]    = "sampler2DRect",
                [PL_SAMPLER_EXTERNAL][2] = "samplerExternalOES",
            };

            const struct pl_tex *tex = sd_binding(*sd).object;
            int dims = pl_tex_params_dimension(tex->params);
            const char *type = types[tex->sampler_type][dims];
            pl_assert(type);

            static const char prefixes[PL_FMT_TYPE_COUNT] = {
                [PL_FMT_FLOAT]  = ' ',
                [PL_FMT_UNORM]  = ' ',
                [PL_FMT_SNORM]  = ' ',
                [PL_FMT_UINT]   = 'u',
                [PL_FMT_SINT]   = 'i',
            };

            char prefix = prefixes[tex->params.format->type];
            pl_assert(prefix);

            const char *prec = "";
            if (prefix != ' ' && gpu->glsl.gles)
                prec = "highp ";

            // Vulkan requires explicit bindings; GL always sets the
            // bindings manually to avoid relying on the user doing so
            if (gpu->glsl.vulkan)
                ADD(pre, "layout(binding=%d) ", desc->binding);

            pl_assert(type && prefix);
            ADD(pre, "uniform %s%c%s %s;\n", prec, prefix, type, desc->name);
            break;
        }

        case PL_DESC_STORAGE_IMG: {
            static const char *types[] = {
                [1] = "image1D",
                [2] = "image2D",
                [3] = "image3D",
            };

            // For better compatibility, we have to explicitly label the
            // type of data we will be reading/writing to this image.
            const struct pl_tex *tex = sd_binding(*sd).object;
            const char *format = tex->params.format->glsl_format;
            const char *access = pl_desc_access_glsl_name(desc->access);
            int dims = pl_tex_params_dimension(tex->params);
            if (gpu->glsl.vulkan) {
                if (format) {
                    ADD(pre, "layout(binding=%d, %s) ", desc->binding, format);
                } else {
                    ADD(pre, "layout(binding=%d) ", desc->binding);
                }
            } else if (gpu->glsl.version >= 130 && format) {
                ADD(pre, "layout(%s) ", format);
            }

            ADD(pre, "%s%s%s restrict uniform %s %s;\n", access,
                (sd->memory & PL_MEMORY_COHERENT) ? " coherent" : "",
                (sd->memory & PL_MEMORY_VOLATILE) ? " volatile" : "",
                types[dims], desc->name);
            break;
        }

        case PL_DESC_BUF_UNIFORM:
            if (gpu->glsl.vulkan) {
                ADD(pre, "layout(std140, binding=%d) ", desc->binding);
            } else {
                ADD(pre, "layout(std140) ");
            }
            ADD(pre, "uniform %s ", desc->name);
            add_buffer_vars(dp, pre, sd->buffer_vars, sd->num_buffer_vars, tmp);
            break;

        case PL_DESC_BUF_STORAGE:
            if (gpu->glsl.vulkan) {
                ADD(pre, "layout(std430, binding=%d) ", desc->binding);
            } else if (gpu->glsl.version >= 140) {
                ADD(pre, "layout(std430) ");
            }
            ADD(pre, "%s%s%s restrict buffer %s ",
                pl_desc_access_glsl_name(desc->access),
                (sd->memory & PL_MEMORY_COHERENT) ? " coherent" : "",
                (sd->memory & PL_MEMORY_VOLATILE) ? " volatile" : "",
                desc->name);
            add_buffer_vars(dp, pre, sd->buffer_vars, sd->num_buffer_vars, tmp);
            break;

        case PL_DESC_BUF_TEXEL_UNIFORM:
            if (gpu->glsl.vulkan)
                ADD(pre, "layout(binding=%d) ", desc->binding);
            ADD(pre, "uniform samplerBuffer %s;\n", desc->name);
            break;

        case PL_DESC_BUF_TEXEL_STORAGE: {
            const struct pl_buf *buf = sd_binding(*sd).object;
            const char *format = buf->params.format->glsl_format;
            const char *access = pl_desc_access_glsl_name(desc->access);
            if (gpu->glsl.vulkan) {
                if (format) {
                    ADD(pre, "layout(binding=%d, %s) ", desc->binding, format);
                } else {
                    ADD(pre, "layout(binding=%d) ", desc->binding);
                }
            } else if (format) {
                ADD(pre, "layout(%s) ", format);
            }

            ADD(pre, "%s%s%s restrict uniform imageBuffer %s;\n", access,
                (sd->memory & PL_MEMORY_COHERENT) ? " coherent" : "",
                (sd->memory & PL_MEMORY_VOLATILE) ? " volatile" : "",
                desc->name);
            break;
        }
        default: abort();
        }
    }

    // Add all of the remaining variables
    for (int i = 0; i < res->num_variables; i++) {
        const struct pl_var *var = &res->variables[i].var;
        const struct pass_var *pv = &pass->vars[i];
        if (pv->type != PASS_VAR_GLOBAL)
            continue;
        ADD(pre, "uniform ");
        add_var(dp, pre, var);
    }

    char *vert_in  = gpu->glsl.version >= 130 ? "in" : "attribute";
    char *vert_out = gpu->glsl.version >= 130 ? "out" : "varying";
    char *frag_in  = gpu->glsl.version >= 130 ? "in" : "varying";

    pl_str *glsl = &dp->tmp[TMP_MAIN];
    ADD_STR(glsl, *pre);

    const char *out_color = "gl_FragColor";
    switch(params->type) {
    case PL_PASS_RASTER: {
        pl_assert(vert_pos);
        pl_str *vert_head = &dp->tmp[TMP_VERT_HEAD];
        pl_str *vert_body = &dp->tmp[TMP_VERT_BODY];

        // Set up a trivial vertex shader
        ADD_STR(vert_head, *pre);
        ADD(vert_body, "void main() {\n");
        for (int i = 0; i < sh->vas.num; i++) {
            const struct pl_vertex_attrib *va = &params->vertex_attribs[i];
            const struct pl_shader_va *sva = &sh->vas.elem[i];
            const char *type = va->fmt->glsl_type;

            // Use the pl_shader_va for the name in the fragment shader since
            // the pl_vertex_attrib is already mangled for the vertex shader
            const char *name = sva->attr.name;

            char loc[32];
            snprintf(loc, sizeof(loc), "layout(location=%d)", va->location);
            // Older GLSL doesn't support the use of explicit locations
            if (gpu->glsl.version < 430)
                loc[0] = '\0';
            ADD(vert_head, "%s %s %s %s;\n", loc, vert_in, type, va->name);

            if (strcmp(name, vert_pos) == 0) {
                pl_assert(va->fmt->num_components == 2);
                if (out_proj) {
                    ADD(vert_body, "gl_Position = vec4((%s * vec3(%s, 1.0)).xy, 0.0, 1.0); \n",
                        out_proj, va->name);
                } else {
                    ADD(vert_body, "gl_Position = vec4(%s, 0.0, 1.0);\n", va->name);
                }
            } else {
                // Everything else is just blindly passed through
                ADD(vert_head, "%s %s %s %s;\n", loc, vert_out, type, name);
                ADD(vert_body, "%s = %s;\n", name, va->name);
                ADD(glsl, "%s %s %s %s;\n", loc, frag_in, type, name);
            }
        }

        ADD(vert_body, "}");
        ADD_STR(vert_head, *vert_body);
        params->vertex_shader = vert_head->buf;

        // GLSL 130+ doesn't use the magic gl_FragColor
        if (gpu->glsl.version >= 130) {
            out_color = "out_color";
            ADD(glsl, "%s out vec4 %s;\n",
                gpu->glsl.version >= 430 ? "layout(location=0) " : "",
                out_color);
        }
        break;
    }
    case PL_PASS_COMPUTE:
        ADD(glsl, "layout (local_size_x = %d, local_size_y = %d) in;\n",
            res->compute_group_size[0], res->compute_group_size[1]);
        break;
    default: abort();
    }

    // Set up the main shader body
    ADD(glsl, "%s", res->glsl);
    ADD(glsl, "void main() {\n");

    pl_assert(res->input == PL_SHADER_SIG_NONE);
    switch (params->type) {
    case PL_PASS_RASTER:
        pl_assert(res->output == PL_SHADER_SIG_COLOR);
        ADD(glsl, "%s = %s();\n", out_color, res->name);
        break;
    case PL_PASS_COMPUTE:
        ADD(glsl, "%s();\n", res->name);
        break;
    default: abort();
    }

    ADD(glsl, "}");
    params->glsl_shader = glsl->buf;
}

#undef ADD
#undef ADD_STR

static bool blend_equal(const struct pl_blend_params *a,
                        const struct pl_blend_params *b)
{
    if (!a && !b)
        return true;
    if (!a || !b)
        return false;

    return a->src_rgb == b->src_rgb && a->dst_rgb == b->dst_rgb &&
           a->src_alpha == b->src_alpha && a->dst_alpha == b->dst_alpha;
}

#define pass_age(pass) (dp->current_index - (pass)->last_index)

static int cmp_pass_age(const void *ptra, const void *ptrb)
{
    const struct pass *a = *(const struct pass **) ptra;
    const struct pass *b = *(const struct pass **) ptrb;
    return b->last_index - a->last_index;
}

static void garbage_collect_passes(struct pl_dispatch *dp)
{
    if (dp->passes.num <= dp->max_passes)
        return;

    // Garbage collect oldest passes, starting at the middle
    qsort(dp->passes.elem, dp->passes.num, sizeof(struct pass *), cmp_pass_age);
    int idx = dp->passes.num / 2;
    while (idx < dp->passes.num && pass_age(dp->passes.elem[idx]) < MIN_AGE)
        idx++;

    for (int i = idx; i < dp->passes.num; i++)
        pass_destroy(dp, dp->passes.elem[i]);

    int num_evicted = dp->passes.num - idx;
    dp->passes.num = idx;

    if (num_evicted) {
        PL_DEBUG(dp, "Evicted %d passes from dispatch cache, consider "
                 "using more dynamic shaders", num_evicted);
    } else {
        dp->max_passes *= 2;
    }
}

static struct pass *find_pass(struct pl_dispatch *dp, struct pl_shader *sh,
                              const struct pl_tex *target, ident_t vert_pos,
                              const struct pl_blend_params *blend, bool load,
                              const struct pl_dispatch_vertex_params *vparams,
                              ident_t out_proj)
{
    uint64_t sig = pl_shader_signature(sh);

    for (int i = 0; i < dp->passes.num; i++) {
        struct pass *p = dp->passes.elem[i];
        if (p->signature != sig)
            continue;

        // Failed shader, no additional checks needed
        if (!p->pass) {
            p->last_index = dp->current_index;
            return p;
        }

        if (pl_shader_is_compute(sh)) {
            // no special requirements besides the signature
            p->last_index = dp->current_index;
            return p;
        } else {
            pl_assert(target);
            const struct pl_fmt *tfmt;
            tfmt = p->pass->params.target_dummy.params.format;
            bool raster_ok = target->params.format == tfmt;
            raster_ok &= blend_equal(p->pass->params.blend_params, blend);
            raster_ok &= load == p->pass->params.load_target;
            if (vparams) {
                raster_ok &= p->pass->params.vertex_type == vparams->vertex_type;
                raster_ok &= p->pass->params.vertex_stride == vparams->vertex_stride;
            }
            if (raster_ok) {
                p->last_index = dp->current_index;
                return p;
            }
        }
    }

    void *tmp = pl_tmp(NULL); // for resources attached to `params`

    struct pass *pass = pl_alloc_ptr(dp, pass);
    *pass = (struct pass) {
        .signature = sig,
        .last_index = dp->current_index,
        .ubo_desc = {
            .desc = {
                .name = "UBO",
                .type = PL_DESC_BUF_UNIFORM,
            },
        },
    };

    struct pl_pass_run_params *rparams = &pass->run_params;
    struct pl_pass_params params = {
        .type = pl_shader_is_compute(sh) ? PL_PASS_COMPUTE : PL_PASS_RASTER,
        .num_descriptors = sh->descs.num,
        .blend_params = blend, // set this for all pass types (for caching)
        .vertex_type = vparams ? vparams->vertex_type : PL_PRIM_TRIANGLE_STRIP,
        .vertex_stride = vparams ? vparams->vertex_stride : 0,
    };

    // Find and attach the cached program, if any
    for (int i = 0; i < dp->cached_passes.num; i++) {
        if (dp->cached_passes.elem[i].signature == sig) {
            PL_DEBUG(dp, "Re-using cached program with signature 0x%llx",
                     (unsigned long long) sig);

            params.cached_program = dp->cached_passes.elem[i].cached_program;
            params.cached_program_len = dp->cached_passes.elem[i].cached_program_len;
            PL_ARRAY_REMOVE_AT(dp->cached_passes, i);
            break;
        }
    }

    if (params.type == PL_PASS_RASTER) {
        assert(target);
        params.target_dummy = *target;
        params.load_target = load;

        // Fill in the vertex attributes array
        params.num_vertex_attribs = sh->vas.num;
        params.vertex_attribs = pl_calloc_ptr(tmp, sh->vas.num, params.vertex_attribs);

        int va_loc = 0;
        for (int i = 0; i < sh->vas.num; i++) {
            struct pl_vertex_attrib *va = &params.vertex_attribs[i];
            *va = sh->vas.elem[i].attr;

            // Mangle the name to make sure it doesn't conflict with the
            // fragment shader input
            va->name = pl_asprintf(tmp, "%s_v", va->name);

            // Place the vertex attribute
            va->location = va_loc;
            if (!vparams) {
                va->offset = params.vertex_stride;
                params.vertex_stride += va->fmt->texel_size;
            }

            // The number of vertex attribute locations consumed by a vertex
            // attribute is the number of vec4s it consumes, rounded up
            const size_t va_loc_size = sizeof(float[4]);
            va_loc += (va->fmt->texel_size + va_loc_size - 1) / va_loc_size;
        }

        if (!vparams) {
            // Generate the vertex array placeholder
            rparams->vertex_count = 4; // single quad
            size_t vert_size = rparams->vertex_count * params.vertex_stride;
            rparams->vertex_data = pl_zalloc(pass, vert_size);
        }
    }

    // Place all the variables; these will dynamically end up in different
    // locations based on what the underlying GPU supports (UBOs, pushc, etc.)
    //
    // We go through the list twice, once to place stuff that we definitely
    // want inside PCs, and then a second time to opportunistically place the rest.
    pass->vars = pl_calloc_ptr(pass, sh->vars.num, pass->vars);
    for (int i = 0; i < sh->vars.num; i++) {
        if (!add_pass_var(dp, tmp, pass, &params, &sh->vars.elem[i], &pass->vars[i], false))
            goto error;
    }
    for (int i = 0; i < sh->vars.num; i++) {
        if (!add_pass_var(dp, tmp, pass, &params, &sh->vars.elem[i], &pass->vars[i], true))
            goto error;
    }

    // Create and attach the UBO if necessary
    int ubo_index = -1;
    size_t ubo_size = sh_buf_desc_size(&pass->ubo_desc);
    if (ubo_size) {
        pass->ubo = pl_buf_create(dp->gpu, &(struct pl_buf_params) {
            .size = ubo_size,
            .uniform = true,
            .host_writable = true,
        });

        if (!pass->ubo) {
            PL_ERR(dp, "Failed creating uniform buffer for dispatch");
            goto error;
        }

        ubo_index = sh->descs.num;
        pass->ubo_desc.binding.object = pass->ubo;
        sh_desc(sh, pass->ubo_desc);
    }

    // Place and fill in the descriptors
    int num = sh->descs.num;
    int binding[PL_DESC_TYPE_COUNT] = {0};
    params.num_descriptors = num;
    params.descriptors = pl_calloc_ptr(tmp, num, params.descriptors);
    rparams->desc_bindings = pl_calloc_ptr(pass, num, rparams->desc_bindings);
    for (int i = 0; i < num; i++) {
        struct pl_desc *desc = &params.descriptors[i];
        *desc = sh->descs.elem[i].desc;
        desc->binding = binding[pl_desc_namespace(dp->gpu, desc->type)]++;
    }

    // Pre-fill the desc_binding for the UBO
    if (pass->ubo) {
        pl_assert(ubo_index >= 0);
        rparams->desc_bindings[ubo_index].object = pass->ubo;
    }

    // Create the push constants region
    params.push_constants_size = PL_ALIGN2(params.push_constants_size, 4);
    rparams->push_constants = pl_zalloc(pass, params.push_constants_size);

    // Finally, finalize the shaders and create the pass itself
    generate_shaders(dp, pass, &params, sh, vert_pos, out_proj, tmp);
    pass->pass = rparams->pass = pl_pass_create(dp->gpu, &params);
    if (!pass->pass) {
        PL_ERR(dp, "Failed creating render pass for dispatch");
        goto error;
    }

    // fall through
error:
    pass->ubo_desc = (struct pl_shader_desc) {0}; // contains temporary pointers
    pl_free(tmp);
    garbage_collect_passes(dp);
    PL_ARRAY_APPEND(dp, dp->passes, pass);
    return pass;
}

static void update_pass_var(struct pl_dispatch *dp, struct pass *pass,
                            const struct pl_shader_var *sv, struct pass_var *pv)
{
    struct pl_var_layout host_layout = pl_var_host_layout(0, &sv->var);
    pl_assert(host_layout.size);

    // Use the cache to skip updates if possible
    if (pv->cached_data && !memcmp(sv->data, pv->cached_data, host_layout.size))
        return;
    if (!pv->cached_data)
        pv->cached_data = pl_alloc(pass, host_layout.size);
    memcpy(pv->cached_data, sv->data, host_layout.size);

    struct pl_pass_run_params *rparams = &pass->run_params;
    switch (pv->type) {
    case PASS_VAR_NONE:
        abort();
    case PASS_VAR_GLOBAL: {
        struct pl_var_update vu = {
            .index = pv->index,
            .data  = sv->data,
        };
        PL_ARRAY_APPEND_RAW(pass, rparams->var_updates, rparams->num_var_updates, vu);
        break;
    }
    case PASS_VAR_UBO: {
        pl_assert(pass->ubo);
        const size_t offset = pv->layout.offset;
        if (host_layout.stride == pv->layout.stride) {
            pl_assert(host_layout.size == pv->layout.size);
            pl_buf_write(dp->gpu, pass->ubo, offset, sv->data, host_layout.size);
        } else {
            // Coalesce strided UBO write into a single pl_buf_write to avoid
            // unnecessary synchronization overhead by assembling the correctly
            // strided upload in RAM
            pl_grow(dp, &dp->tmp[0].buf, pv->layout.size);
            uint8_t * const tmp = dp->tmp[0].buf;
            const uint8_t *src = sv->data;
            const uint8_t *end = src + host_layout.size;
            uint8_t *dst = tmp;
            while (src < end) {
                memcpy(dst, src, host_layout.stride);
                src += host_layout.stride;
                dst += pv->layout.stride;
            }
            pl_buf_write(dp->gpu, pass->ubo, offset, tmp, pv->layout.size);
        }
        break;
    }
    case PASS_VAR_PUSHC:
        pl_assert(rparams->push_constants);
        memcpy_layout(rparams->push_constants, pv->layout, sv->data, host_layout);
        break;
    };
}

static void compute_vertex_attribs(struct pl_dispatch *dp, struct pl_shader *sh,
                                   int width, int height, ident_t *out_scale)
{
    // Simulate vertex attributes using global definitions
    *out_scale = sh_var(sh, (struct pl_shader_var) {
        .var     = pl_var_vec2("out_scale"),
        .data    = &(float[2]){ 1.0 / width, 1.0 / height },
        .dynamic = true,
    });

    GLSLP("#define frag_pos(id) (vec2(id) + vec2(0.5)) \n"
          "#define frag_map(id) (%s * frag_pos(id))    \n"
          "#define gl_FragCoord vec4(frag_pos(gl_GlobalInvocationID), 0.0, 1.0) \n",
          *out_scale);

    for (int n = 0; n < sh->vas.num; n++) {
        const struct pl_shader_va *sva = &sh->vas.elem[n];

        ident_t points[4];
        for (int i = 0; i < PL_ARRAY_SIZE(points); i++) {
            char name[4];
            snprintf(name, sizeof(name), "p%d", i);
            points[i] = sh_var_from_va(sh, name, &sva->attr, sva->data[i]);
        }

        GLSLP("#define %s_map(id) "
             "(mix(mix(%s, %s, frag_map(id).x), "
             "     mix(%s, %s, frag_map(id).x), "
             "frag_map(id).y))\n"
             "#define %s (%s_map(gl_GlobalInvocationID))\n",
             sva->attr.name,
             points[0], points[1], points[2], points[3],
             sva->attr.name, sva->attr.name);
    }
}

static void translate_compute_shader(struct pl_dispatch *dp,
                                     struct pl_shader *sh,
                                     const struct pl_rect2d *rc,
                                     const struct pl_dispatch_params *params)
{
    int width = abs(pl_rect_w(*rc)), height = abs(pl_rect_h(*rc));
    ident_t out_scale;
    compute_vertex_attribs(dp, sh, width, height, &out_scale);

    // Simulate a framebuffer using storage images
    pl_assert(params->target->params.storable);
    pl_assert(sh->res.output == PL_SHADER_SIG_COLOR);
    ident_t fbo = sh_desc(sh, (struct pl_shader_desc) {
        .binding.object = params->target,
        .desc = {
            .name    = "out_image",
            .type    = PL_DESC_STORAGE_IMG,
            .access  = params->blend_params ? PL_DESC_ACCESS_READWRITE
                                            : PL_DESC_ACCESS_WRITEONLY,
        },
    });

    ident_t base = sh_var(sh, (struct pl_shader_var) {
        .data    = &(int[2]){ rc->x0, rc->y0 },
        .dynamic = true,
        .var     = {
            .name  = "base",
            .type  = PL_VAR_SINT,
            .dim_v = 2,
            .dim_m = 1,
            .dim_a = 1,
        },
    });

    int dx = rc->x0 > rc->x1 ? -1 : 1, dy = rc->y0 > rc->y1 ? -1 : 1;
    GLSL("ivec2 dir = ivec2(%d, %d);\n", dx, dy); // hard-code, not worth var
    GLSL("ivec2 pos = %s + dir * ivec2(gl_GlobalInvocationID);\n", base);
    GLSL("vec2 fpos = %s * vec2(gl_GlobalInvocationID);\n", out_scale);
    GLSL("if (max(fpos.x, fpos.y) < 1.0) {\n");
    if (params->blend_params) {
        GLSL("vec4 orig = imageLoad(%s, pos);\n", fbo);

        static const char *modes[] = {
            [PL_BLEND_ZERO] = "0.0",
            [PL_BLEND_ONE]  = "1.0",
            [PL_BLEND_SRC_ALPHA] = "color.a",
            [PL_BLEND_ONE_MINUS_SRC_ALPHA] = "(1.0 - color.a)",
        };

        GLSL("color = vec4(color.rgb * vec3(%s), color.a * %s) \n"
             "      + vec4(orig.rgb  * vec3(%s), orig.a  * %s);\n",
             modes[params->blend_params->src_rgb],
             modes[params->blend_params->src_alpha],
             modes[params->blend_params->dst_rgb],
             modes[params->blend_params->dst_alpha]);
    }
    GLSL("imageStore(%s, pos, color);\n", fbo);
    GLSL("}\n");
    sh->res.output = PL_SHADER_SIG_NONE;
}

bool pl_dispatch_finish(struct pl_dispatch *dp, const struct pl_dispatch_params *params)
{
    struct pl_shader *sh = *params->shader;
    const struct pl_shader_res *res = &sh->res;
    bool ret = false;

    if (sh->failed) {
        PL_ERR(sh, "Trying to dispatch a failed shader.");
        goto error;
    }

    if (!sh->mutable) {
        PL_ERR(dp, "Trying to dispatch non-mutable shader?");
        goto error;
    }

    if (res->input != PL_SHADER_SIG_NONE || res->output != PL_SHADER_SIG_COLOR) {
        PL_ERR(dp, "Trying to dispatch shader with incompatible signature!");
        goto error;
    }

    const struct pl_tex_params *tpars = &params->target->params;
    if (pl_tex_params_dimension(*tpars) != 2 || !tpars->renderable) {
        PL_ERR(dp, "Trying to dispatch a shader using an invalid target "
               "texture. The target must be a renderable 2D texture.");
        goto error;
    }

    if (pl_shader_is_compute(sh) && !tpars->storable) {
        PL_ERR(dp, "Trying to dispatch using a compute shader with a "
               "non-storable target texture.");
        goto error;
    } else if (tpars->storable && (dp->gpu->caps & PL_GPU_CAP_PARALLEL_COMPUTE)) {
        if (sh_try_compute(sh, 16, 16, true, 0))
            PL_TRACE(dp, "Upgrading fragment shader to compute shader.");
    }

    struct pl_rect2d rc = params->rect;
    if (!pl_rect_w(rc)) {
        rc.x0 = 0;
        rc.x1 = tpars->w;
    }
    if (!pl_rect_h(rc)) {
        rc.y0 = 0;
        rc.y1 = tpars->h;
    }

    int w, h, tw = abs(pl_rect_w(rc)), th = abs(pl_rect_h(rc));
    if (pl_shader_output_size(sh, &w, &h) && (w != tw || h != th))
    {
        PL_ERR(dp, "Trying to dispatch a shader with explicit output size "
               "requirements %dx%d using a target rect of size %dx%d.",
               w, h, tw, th);
        goto error;
    }

    ident_t vert_pos = NULL;

    if (pl_shader_is_compute(sh)) {
        // Translate the compute shader to simulate vertices etc.
        translate_compute_shader(dp, sh, &rc, params);
    } else {
        // Add the vertex information encoding the position
        vert_pos = sh_attr_vec2(sh, "position", &(const struct pl_rect2df) {
            .x0 = 2.0 * rc.x0 / tpars->w - 1.0,
            .y0 = 2.0 * rc.y0 / tpars->h - 1.0,
            .x1 = 2.0 * rc.x1 / tpars->w - 1.0,
            .y1 = 2.0 * rc.y1 / tpars->h - 1.0,
        });
    }

    // We need to set pl_pass_params.load_target when either blending is
    // enabled or we're drawing to some scissored sub-rect of the texture
    struct pl_rect2d full = { 0, 0, tpars->w, tpars->h };
    struct pl_rect2d rc_norm = rc;
    pl_rect2d_normalize(&rc_norm);
    rc_norm.x0 = PL_MAX(rc_norm.x0, 0);
    rc_norm.y0 = PL_MAX(rc_norm.y0, 0);
    rc_norm.x1 = PL_MIN(rc_norm.x1, tpars->w);
    rc_norm.y1 = PL_MIN(rc_norm.y1, tpars->h);
    bool load = params->blend_params || !pl_rect2d_eq(rc_norm, full);

    struct pass *pass = find_pass(dp, sh, params->target, vert_pos,
                                  params->blend_params, load, NULL, NULL);

    // Silently return on failed passes
    if (!pass->pass)
        goto error;

    struct pl_pass_run_params *rparams = &pass->run_params;

    // Update the descriptor bindings
    for (int i = 0; i < sh->descs.num; i++)
        rparams->desc_bindings[i] = sd_binding(sh->descs.elem[i]);

    // Update all of the variables (if needed)
    rparams->num_var_updates = 0;
    for (int i = 0; i < sh->vars.num; i++)
        update_pass_var(dp, pass, &sh->vars.elem[i], &pass->vars[i]);

    // Update the vertex data
    if (rparams->vertex_data) {
        uintptr_t vert_base = (uintptr_t) rparams->vertex_data;
        size_t stride = rparams->pass->params.vertex_stride;
        for (int i = 0; i < sh->vas.num; i++) {
            const struct pl_shader_va *sva = &sh->vas.elem[i];
            struct pl_vertex_attrib *va = &rparams->pass->params.vertex_attribs[i];

            size_t size = sva->attr.fmt->texel_size;
            uintptr_t va_base = vert_base + va->offset; // use placed offset
            for (int n = 0; n < 4; n++)
                memcpy((void *) (va_base + n * stride), sva->data[n], size);
        }
    }

    // For compute shaders: also update the dispatch dimensions
    if (pl_shader_is_compute(sh)) {
        // Round up to make sure we don-t leave off a part of the target
        int width = abs(pl_rect_w(rc)),
            height = abs(pl_rect_h(rc)),
            block_w = res->compute_group_size[0],
            block_h = res->compute_group_size[1],
            num_x   = (width  + block_w - 1) / block_w,
            num_y   = (height + block_h - 1) / block_h;

        rparams->compute_groups[0] = num_x;
        rparams->compute_groups[1] = num_y;
        rparams->compute_groups[2] = 1;
    } else {
        // Update the scissors for performance
        rparams->scissors = rc_norm;
    }

    // Dispatch the actual shader
    rparams->target = params->target;
    rparams->timer = params->timer;
    pl_pass_run(dp->gpu, &pass->run_params);
    ret = true;

error:
    // Reset the temporary buffers which we use to build the shader
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        dp->tmp[i].len = 0;

    pl_dispatch_abort(dp, params->shader);
    return ret;
}

bool pl_dispatch_compute(struct pl_dispatch *dp,
                         const struct pl_dispatch_compute_params *params)
{
    struct pl_shader *sh = *params->shader;
    const struct pl_shader_res *res = &sh->res;
    bool ret = false;

    if (sh->failed) {
        PL_ERR(sh, "Trying to dispatch a failed shader.");
        goto error;
    }

    if (!sh->mutable) {
        PL_ERR(dp, "Trying to dispatch non-mutable shader?");
        goto error;
    }

    if (res->input != PL_SHADER_SIG_NONE) {
        PL_ERR(dp, "Trying to dispatch shader with incompatible signature!");
        goto error;
    }

    if (!pl_shader_is_compute(sh)) {
        PL_ERR(dp, "Trying to dispatch a non-compute shader using "
               "`pl_dispatch_compute`!");
        goto error;
    }

    if (sh->vas.num) {
        if (!params->width || !params->height) {
            PL_ERR(dp, "Trying to dispatch a targetless compute shader that "
                   "uses vertex attributes, this requires specifying the size "
                   "of the effective rendering area!");
            goto error;
        }

        compute_vertex_attribs(dp, sh, params->width, params->height,
                               &(ident_t){0});
    }

    struct pass *pass = find_pass(dp, sh, NULL, NULL, NULL, false, NULL, NULL);

    // Silently return on failed passes
    if (!pass->pass)
        goto error;

    struct pl_pass_run_params *rparams = &pass->run_params;

    // Update the descriptor bindings
    for (int i = 0; i < sh->descs.num; i++)
        rparams->desc_bindings[i] = sd_binding(sh->descs.elem[i]);

    // Update all of the variables (if needed)
    rparams->num_var_updates = 0;
    for (int i = 0; i < sh->vars.num; i++)
        update_pass_var(dp, pass, &sh->vars.elem[i], &pass->vars[i]);

    // Update the dispatch size
    int groups = 1;
    for (int i = 0; i < 3; i++) {
        groups *= params->dispatch_size[i];
        rparams->compute_groups[i] = params->dispatch_size[i];
    }

    if (!groups) {
        pl_assert(params->width && params->height);
        int block_w = res->compute_group_size[0],
            block_h = res->compute_group_size[1],
            num_x   = (params->width  + block_w - 1) / block_w,
            num_y   = (params->height + block_h - 1) / block_h;

        rparams->compute_groups[0] = num_x;
        rparams->compute_groups[1] = num_y;
        rparams->compute_groups[2] = 1;
    }

    // Dispatch the actual shader
    rparams->timer = params->timer;
    pl_pass_run(dp->gpu, &pass->run_params);
    ret = true;

error:
    // Reset the temporary buffers which we use to build the shader
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        dp->tmp[i].len = 0;

    pl_dispatch_abort(dp, params->shader);
    return ret;
}

bool pl_dispatch_vertex(struct pl_dispatch *dp,
                        const struct pl_dispatch_vertex_params *params)
{
    struct pl_shader *sh = *params->shader;
    const struct pl_shader_res *res = &sh->res;
    bool ret = false;

    if (sh->failed) {
        PL_ERR(sh, "Trying to dispatch a failed shader.");
        goto error;
    }

    if (!sh->mutable) {
        PL_ERR(dp, "Trying to dispatch non-mutable shader?");
        goto error;
    }

    if (res->input != PL_SHADER_SIG_NONE || res->output != PL_SHADER_SIG_COLOR) {
        PL_ERR(dp, "Trying to dispatch shader with incompatible signature!");
        goto error;
    }

    const struct pl_tex_params *tpars = &params->target->params;
    if (pl_tex_params_dimension(*tpars) != 2 || !tpars->renderable) {
        PL_ERR(dp, "Trying to dispatch a shader using an invalid target "
               "texture. The target must be a renderable 2D texture.");
        goto error;
    }

    if (pl_shader_is_compute(sh)) {
        PL_ERR(dp, "Trying to dispatch a compute shader using pl_dispatch_vertex.");
        goto error;
    }

    if (sh->vas.num) {
        PL_ERR(dp, "Trying to dispatch a custom vertex shader with already "
               "attached vertex attributes.");
        goto error;
    }

    int pos_idx = params->vertex_position_idx;
    if (pos_idx < 0 || pos_idx >= params->num_vertex_attribs) {
        PL_ERR(dp, "Vertex position index out of range?");
        goto error;
    }

    // Attach all of the vertex attributes to the shader manually
    sh->vas.num = params->num_vertex_attribs;
    PL_ARRAY_RESIZE(sh, sh->vas, sh->vas.num);
    for (int i = 0; i < params->num_vertex_attribs; i++)
        sh->vas.elem[i].attr = params->vertex_attribs[i];

    // Compute the coordinate projection matrix
    struct pl_transform2x2 proj = pl_transform2x2_identity;
    switch (params->vertex_coords) {
    case PL_COORDS_ABSOLUTE:
        proj.mat.m[0][0] /= tpars->w;
        proj.mat.m[1][1] /= tpars->h;
        // fall through
    case PL_COORDS_RELATIVE:
        proj.mat.m[0][0] *= 2.0;
        proj.mat.m[1][1] *= 2.0;
        proj.c[0] -= 1.0;
        proj.c[1] -= 1.0;
        // fall through
    case PL_COORDS_NORMALIZED:
        if (params->vertex_flipped) {
            proj.mat.m[1][1] = -proj.mat.m[1][1];
            proj.c[1] += 2.0;
        }
        break;
    }

    ident_t out_proj = NULL;
    if (memcmp(&proj, &pl_transform2x2_identity, sizeof(proj)) != 0) {
        struct pl_matrix3x3 mat = {{
            {proj.mat.m[0][0], proj.mat.m[0][1], proj.c[0]},
            {proj.mat.m[1][0], proj.mat.m[1][1], proj.c[1]},
            {0.0, 0.0, 1.0},
        }};
        out_proj = sh_var(sh, (struct pl_shader_var) {
            .var = pl_var_mat3("proj"),
            .data = PL_TRANSPOSE_3X3(mat.m),
        });
    }

    ident_t vert_pos = params->vertex_attribs[pos_idx].name;
    struct pass *pass = find_pass(dp, sh, params->target, vert_pos,
                                  params->blend_params, true, params, out_proj);

    // Silently return on failed passes
    if (!pass->pass)
        goto error;

    struct pl_pass_run_params *rparams = &pass->run_params;

    // Update the descriptor bindings
    for (int i = 0; i < sh->descs.num; i++)
        rparams->desc_bindings[i] = sd_binding(sh->descs.elem[i]);

    // Update all of the variables (if needed)
    rparams->num_var_updates = 0;
    for (int i = 0; i < sh->vars.num; i++)
        update_pass_var(dp, pass, &sh->vars.elem[i], &pass->vars[i]);

    // Update the scissors
    rparams->scissors = params->scissors;
    if (params->vertex_flipped) {
        rparams->scissors.y0 = tpars->h - rparams->scissors.y0;
        rparams->scissors.y1 = tpars->h - rparams->scissors.y1;
    }
    pl_rect2d_normalize(&rparams->scissors);

    // Dispatch the actual shader
    rparams->target = params->target;
    rparams->vertex_count = params->vertex_count;
    rparams->vertex_data = params->vertex_data;
    rparams->vertex_buf = params->vertex_buf;
    rparams->buf_offset = params->buf_offset;
    rparams->index_data = params->index_data;
    rparams->index_buf = params->index_buf;
    rparams->index_offset = params->index_offset;
    rparams->timer = params->timer;
    pl_pass_run(dp->gpu, &pass->run_params);
    ret = true;

error:
    // Reset the temporary buffers which we use to build the shader
    for (int i = 0; i < PL_ARRAY_SIZE(dp->tmp); i++)
        dp->tmp[i].len = 0;

    pl_dispatch_abort(dp, params->shader);
    return ret;
}

void pl_dispatch_abort(struct pl_dispatch *dp, struct pl_shader **psh)
{
    struct pl_shader *sh = *psh;
    if (!sh)
        return;

    // Re-add the shader to the internal pool of shaders
    PL_ARRAY_APPEND(dp, dp->shaders, sh);
    *psh = NULL;
}

// Stuff related to caching
static const char cache_magic[] = {'P', 'L', 'D', 'P'};
static const uint32_t cache_version = 1;

static void write_buf(uint8_t *buf, size_t *pos, const void *src, size_t size)
{
    assert(size);
    if (buf)
        memcpy(&buf[*pos], src, size);
    *pos += size;
}

#define WRITE(type, var) write_buf(out, &size, &(type){ var }, sizeof(type))
#define LOAD(var)                           \
  do {                                      \
      memcpy(&(var), cache, sizeof(var));   \
      cache += sizeof(var);                 \
  } while (0)

size_t pl_dispatch_save(struct pl_dispatch *dp, uint8_t *out)
{
    size_t size = 0;

    write_buf(out, &size, cache_magic, sizeof(cache_magic));
    WRITE(uint32_t, cache_version);
    WRITE(uint32_t, dp->passes.num + dp->cached_passes.num);

    // Save the cached programs for all compiled passes
    for (int i = 0; i < dp->passes.num; i++) {
        const struct pass *pass = dp->passes.elem[i];
        if (!pass->pass)
            continue;

        const struct pl_pass_params *params = &pass->pass->params;
        if (!params->cached_program_len)
            continue;

        if (out) {
            PL_DEBUG(dp, "Saving %zu bytes of cached program with signature 0x%llx",
                     params->cached_program_len, (unsigned long long) pass->signature);
        }

        WRITE(uint64_t, pass->signature);
        WRITE(uint64_t, params->cached_program_len);
        write_buf(out, &size, params->cached_program, params->cached_program_len);
    }

    // Re-save the cached programs for all previously loaded (but not yet
    // comppiled) passes. This is simply to make `pl_dispatch_load` followed
    // by `pl_dispatch_save` return the same cache as was previously loaded.
    for (int i = 0; i < dp->cached_passes.num; i++) {
        const struct cached_pass *pass = &dp->cached_passes.elem[i];
        if (out) {
            PL_DEBUG(dp, "Saving %zu bytes of cached program with signature 0x%llx",
                     pass->cached_program_len, (unsigned long long) pass->signature);
        }

        WRITE(uint64_t, pass->signature);
        WRITE(uint64_t, pass->cached_program_len);
        write_buf(out, &size, pass->cached_program, pass->cached_program_len);
    }

    return size;
}

void pl_dispatch_load(struct pl_dispatch *dp, const uint8_t *cache)
{
    char magic[4];
    LOAD(magic);
    if (memcmp(magic, cache_magic, sizeof(magic)) != 0) {
        PL_ERR(dp, "Failed loading dispatch cache: invalid magic bytes");
        return;
    }

    uint32_t version;
    LOAD(version);
    if (version != cache_version) {
        PL_WARN(dp, "Failed loading dispatch cache: wrong version");
        return;
    }

    uint32_t num;
    LOAD(num);

    for (int i = 0; i < num; i++) {
        uint64_t sig, size;
        LOAD(sig);
        LOAD(size);
        if (!size)
            continue;

        // Skip passes that are already compiled
        for (int n = 0; n < dp->passes.num; n++) {
            if (dp->passes.elem[n]->signature == sig) {
                PL_DEBUG(dp, "Skipping already compiled pass with signature %llx",
                         (unsigned long long) sig);
                cache += size;
                continue;
            }
        }

        // Find a cached_pass entry with this signature, if any
        struct cached_pass *pass = NULL;
        for (int n = 0; n < dp->cached_passes.num; n++) {
            if (dp->cached_passes.elem[n].signature == sig) {
                pass = &dp->cached_passes.elem[n];
                break;
            }
        }

        if (!pass) {
            // None found, add a new entry
            PL_ARRAY_GROW(dp, dp->cached_passes);
            pass = &dp->cached_passes.elem[dp->cached_passes.num++];
            *pass = (struct cached_pass) { .signature = sig };
        }

        PL_DEBUG(dp, "Loading %zu bytes of cached program with signature 0x%llx",
                 (size_t) size, (unsigned long long) sig);

        pl_free((void *) pass->cached_program);
        pass->cached_program = pl_memdup(dp, cache, size);
        pass->cached_program_len = size;
        cache += size;
    }
}
