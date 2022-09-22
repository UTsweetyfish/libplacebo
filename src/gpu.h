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

#pragma once

#include "common.h"
#include "context.h"

// To avoid having to include drm_fourcc.h
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR   UINT64_C(0x0)
#define DRM_FORMAT_MOD_INVALID  ((UINT64_C(1) << 56) - 1)
#endif

// This struct must be the first member of the gpu's priv struct. The `pl_gpu`
// helpers will cast the priv struct to this struct!

#define GPU_PFN(name) __typeof__(pl_##name) *name
struct pl_gpu_fns {
    // Destructors: These also free the corresponding objects, but they
    // must not be called on NULL. (The NULL checks are done by the pl_*_destroy
    // wrappers)
    void (*destroy)(const struct pl_gpu *gpu);
    void (*tex_destroy)(const struct pl_gpu *, const struct pl_tex *);
    void (*buf_destroy)(const struct pl_gpu *, const struct pl_buf *);
    void (*pass_destroy)(const struct pl_gpu *, const struct pl_pass *);
    void (*sync_destroy)(const struct pl_gpu *, const struct pl_sync *);
    void (*timer_destroy)(const struct pl_gpu *, struct pl_timer *);

    GPU_PFN(tex_create);
    GPU_PFN(tex_invalidate); // optional
    GPU_PFN(tex_clear); // optional if no blittable formats
    GPU_PFN(tex_blit); // optional if no blittable formats
    GPU_PFN(tex_upload);
    GPU_PFN(tex_download);
    GPU_PFN(tex_poll); // optional: if NULL, textures are always free to use
    GPU_PFN(buf_create);
    GPU_PFN(buf_write);
    GPU_PFN(buf_read);
    GPU_PFN(buf_copy);
    GPU_PFN(buf_export); // optional if !gpu->export_caps.buf
    GPU_PFN(buf_poll); // optional: if NULL, buffers are always free to use
    GPU_PFN(desc_namespace);
    GPU_PFN(pass_create);
    GPU_PFN(pass_run);
    GPU_PFN(sync_create); // optional if !gpu->export_caps.sync
    GPU_PFN(tex_export); // optional if !gpu->export_caps.sync
    GPU_PFN(timer_create); // optional
    GPU_PFN(timer_query); // optional
    GPU_PFN(gpu_flush); // optional
    GPU_PFN(gpu_finish);
    GPU_PFN(gpu_is_failed); // optional
};
#undef GPU_PFN

// All resources such as textures and buffers allocated from the GPU must be
// destroyed before calling pl_destroy.
void pl_gpu_destroy(const struct pl_gpu *gpu);

// Returns true if the device supports interop. This is considered to be
// the case if at least one of `gpu->export/import_caps` is nonzero.
static inline bool pl_gpu_supports_interop(const struct pl_gpu *gpu)
{
    return gpu->export_caps.tex ||
           gpu->import_caps.tex ||
           gpu->export_caps.buf ||
           gpu->import_caps.buf ||
           gpu->export_caps.sync ||
           gpu->import_caps.sync;
}

// GPU-internal helpers: these should not be used outside of GPU implementations

// Log some metadata about the created GPU, and perform verification
void pl_gpu_print_info(const struct pl_gpu *gpu);

// Sort the pl_fmt list into an optimal order. This tries to prefer formats
// supporting more capabilities, while also trying to maintain a sane order in
// terms of bit depth / component index.
void pl_gpu_sort_formats(struct pl_gpu *gpu);

// Look up the right GLSL image format qualifier from a partially filled-in
// pl_fmt, or NULL if the format does not have a legal matching GLSL name.
//
// `components` may differ from fmt->num_components (for emulated formats)
const char *pl_fmt_glsl_format(const struct pl_fmt *fmt, int components);

// Look up the right fourcc from a partially filled-in pl_fmt, or 0 if the
// format does not have a legal matching fourcc format.
uint32_t pl_fmt_fourcc(const struct pl_fmt *fmt);

// Compute the total size (in bytes) of a texture transfer operation
size_t pl_tex_transfer_size(const struct pl_tex_transfer_params *par);

// Helper that wraps pl_tex_upload/download using texture upload buffers to
// ensure that params->buf is always set.
bool pl_tex_upload_pbo(const struct pl_gpu *gpu,
                       const struct pl_tex_transfer_params *params);
bool pl_tex_download_pbo(const struct pl_gpu *gpu,
                         const struct pl_tex_transfer_params *params);

// This requires that params.buf has been set and is of type PL_BUF_TEXEL_*
bool pl_tex_upload_texel(const struct pl_gpu *gpu, struct pl_dispatch *dp,
                         const struct pl_tex_transfer_params *params);
bool pl_tex_download_texel(const struct pl_gpu *gpu, struct pl_dispatch *dp,
                           const struct pl_tex_transfer_params *params);

void pl_pass_run_vbo(const struct pl_gpu *gpu,
                     const struct pl_pass_run_params *params);

// Make a deep-copy of the pass params. Note: cached_program etc. are not
// copied, but cleared explicitly.
struct pl_pass_params pl_pass_params_copy(void *alloc,
                                          const struct pl_pass_params *params);

// Utility function for pretty-printing UUIDs
#define UUID_SIZE 16
#define PRINT_UUID(uuid) (print_uuid((char[3 * UUID_SIZE]){0}, (uuid)))
const char *print_uuid(char buf[3 * UUID_SIZE], const uint8_t uuid[UUID_SIZE]);

// Helper to pretty-print fourcc codes
#define PRINT_FOURCC(fcc)       \
    (!(fcc) ? "" : (char[5]) {  \
        (fcc) & 0xFF,           \
        ((fcc) >> 8) & 0xFF,    \
        ((fcc) >> 16) & 0xFF,   \
        ((fcc) >> 24) & 0xFF    \
    })

#define DRM_MOD_SIZE 26
#define PRINT_DRM_MOD(mod) (print_drm_mod((char[DRM_MOD_SIZE]){0}, (mod)))
const char *print_drm_mod(char buf[DRM_MOD_SIZE], uint64_t mod);
