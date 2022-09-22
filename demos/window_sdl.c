// License: CC0 / Public Domain

#if !defined(USE_GL) && !defined(USE_VK) || defined(USE_GL) && defined(USE_VK)
#error Specify exactly one of -DUSE_GL or -DUSE_VK when compiling!
#endif

#include "common.h"
#include "window.h"

#include <SDL.h>

#ifdef USE_VK
#include <libplacebo/vulkan.h>
#include <SDL_vulkan.h>
#define WINFLAG_API SDL_WINDOW_VULKAN
#define IMPL win_impl_sdl_vk
#define IMPL_NAME "SDL2 (vulkan)"
#endif

#ifdef USE_GL
#include <libplacebo/opengl.h>
#define WINFLAG_API SDL_WINDOW_OPENGL
#define IMPL win_impl_sdl_gl
#define IMPL_NAME "SDL2 (opengl)"
#endif

#ifdef NDEBUG
#define DEBUG false
#else
#define DEBUG true
#endif

const struct window_impl IMPL;

struct priv {
    struct window w;
    SDL_Window *win;

#ifdef USE_VK
    VkSurfaceKHR surf;
    const struct pl_vulkan *vk;
    const struct pl_vk_inst *vk_inst;
#endif

#ifdef USE_GL
    SDL_GLContext gl_ctx;
    const struct pl_opengl *gl;
#endif

    int scroll_dx, scroll_dy;
    char **files;
    size_t files_num;
    size_t files_size;
    bool file_seen;
};

static struct window *sdl_create(struct pl_context *ctx, const char *title,
                                 int width, int height, enum winflags flags)
{
    struct priv *p = calloc(1, sizeof(struct priv));
    if (!p)
        return NULL;

    p->w.impl = &IMPL;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL2: Failed initializing: %s\n", SDL_GetError());
        goto error;
    }

    uint32_t sdl_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | WINFLAG_API;
    p->win = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              width, height, sdl_flags);
    if (!p->win) {
        fprintf(stderr, "SDL2: Failed creating window: %s\n", SDL_GetError());
        goto error;
    }

#ifdef USE_VK
    struct pl_vk_inst_params iparams = pl_vk_inst_default_params;
    iparams.debug = DEBUG;

    unsigned int num = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(p->win, &num, NULL)) {
        fprintf(stderr, "SDL2: Failed enumerating vulkan extensions: %s\n", SDL_GetError());
        goto error;
    }

    const char **exts = malloc(num * sizeof(const char *));
    SDL_Vulkan_GetInstanceExtensions(p->win, &num, exts);
    iparams.extensions = exts;
    iparams.num_extensions = num;

    p->vk_inst = pl_vk_inst_create(ctx, &iparams);
    free(exts);
    if (!p->vk_inst) {
        fprintf(stderr, "libplacebo: Failed creating vulkan instance!\n");
        goto error;
    }

    if (!SDL_Vulkan_CreateSurface(p->win, p->vk_inst->instance, &p->surf)) {
        fprintf(stderr, "SDL2: Failed creating surface: %s\n", SDL_GetError());
        goto error;
    }

    struct pl_vulkan_params params = pl_vulkan_default_params;
    params.instance = p->vk_inst->instance;
    params.surface = p->surf;
    params.allow_software = true;
    p->vk = pl_vulkan_create(ctx, &params);
    if (!p->vk) {
        fprintf(stderr, "libplacebo: Failed creating vulkan device\n");
        goto error;
    }

    p->w.swapchain = pl_vulkan_create_swapchain(p->vk, &(struct pl_vulkan_swapchain_params) {
        .surface = p->surf,
        .present_mode = VK_PRESENT_MODE_FIFO_KHR,
        .prefer_hdr = (flags & WIN_HDR),
    });

    if (!p->w.swapchain) {
        fprintf(stderr, "libplacebo: Failed creating vulkan swapchain\n");
        goto error;
    }

    p->w.gpu = p->vk->gpu;
#endif // USE_VK

#ifdef USE_GL
    p->gl_ctx = SDL_GL_CreateContext(p->win);
    if (!p->gl_ctx) {
        fprintf(stderr, "SDL2: Failed creating GL context: %s\n", SDL_GetError());
        goto error;
    }

    SDL_GL_MakeCurrent(p->win, p->gl_ctx);

    struct pl_opengl_params params = pl_opengl_default_params;
    params.allow_software = true;
    params.debug = DEBUG;
    p->gl = pl_opengl_create(ctx, &params);
    if (!p->gl) {
        fprintf(stderr, "libplacebo: Failed creating opengl device\n");
        goto error;
    }

    p->w.swapchain = pl_opengl_create_swapchain(p->gl, &(struct pl_opengl_swapchain_params) {
        .swap_buffers = (void (*)(void *)) SDL_GL_SwapWindow,
        .priv = p->win,
    });

    if (!p->w.swapchain) {
        fprintf(stderr, "libplacebo: Failed creating opengl swapchain\n");
        goto error;
    }

    if (!pl_swapchain_resize(p->w.swapchain, &width, &height)) {
        fprintf(stderr, "libplacebo: Failed initializing swapchain\n");
        goto error;
    }

    p->w.gpu = p->gl->gpu;
#endif // USE_GL

    return &p->w;

error:
    window_destroy((struct window **) &p);
    return NULL;
}

static void sdl_destroy(struct window **window)
{
    struct priv *p = (struct priv *) *window;
    if (!p)
        return;

    pl_swapchain_destroy(&p->w.swapchain);

#ifdef USE_VK
    pl_vulkan_destroy(&p->vk);
    if (p->surf)
        vkDestroySurfaceKHR(p->vk_inst->instance, p->surf, NULL);
    pl_vk_inst_destroy(&p->vk_inst);
#endif

#ifdef USE_GL
    pl_opengl_destroy(&p->gl);
    SDL_GL_DeleteContext(p->gl_ctx);
#endif

    for (int i = 0; i < p->files_num; i++)
        SDL_free(p->files[i]);
    free(p->files);

    SDL_DestroyWindow(p->win);
    SDL_Quit();
    free(p);
    *window = NULL;
}

static inline void handle_event(struct priv *p, SDL_Event *event)
{
    switch (event->type) {
    case SDL_QUIT:
        p->w.window_lost = true;
        return;

    case SDL_WINDOWEVENT:
        if (event->window.windowID != SDL_GetWindowID(p->win))
            return;

        if (event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int width = event->window.data1, height = event->window.data2;
            if (!pl_swapchain_resize(p->w.swapchain, &width, &height)) {
                fprintf(stderr, "libplacebo: Failed resizing swapchain? Exiting...\n");
                p->w.window_lost = true;
            }
        }
        return;

    case SDL_MOUSEWHEEL:
        p->scroll_dx += event->wheel.x;
        p->scroll_dy += event->wheel.y;
        return;

    case SDL_DROPFILE:
        if (p->files_num == p->files_size) {
            size_t new_size = p->files_size ? p->files_size * 2 : 16;
            char **new_files = realloc(p->files, new_size * sizeof(char *));
            if (!new_files)
                return;
            p->files = new_files;
            p->files_size = new_size;
        }

        p->files[p->files_num++] = event->drop.file;
        return;
    }
}

static void sdl_poll(struct window *window, bool block)
{
    struct priv *p = (struct priv *) window;
    SDL_Event event;
    int ret;

    do {
        ret = block ? SDL_WaitEvent(&event) : SDL_PollEvent(&event);
        if (ret)
            handle_event(p, &event);

        // Only block on the first iteration
        block = false;
    } while (ret);
}

static void sdl_get_cursor(const struct window *window, int *x, int *y)
{
    SDL_GetMouseState(x, y);
}

static bool sdl_get_button(const struct window *window, enum button btn)
{
    static const uint32_t button_mask[] = {
        [BTN_LEFT] = SDL_BUTTON_LMASK,
        [BTN_RIGHT] = SDL_BUTTON_RMASK,
        [BTN_MIDDLE] = SDL_BUTTON_MMASK,
    };

    return SDL_GetMouseState(NULL, NULL) & button_mask[btn];
}

static void sdl_get_scroll(const struct window *window, float *dx, float *dy)
{
    struct priv *p = (struct priv *) window;
    *dx = p->scroll_dx;
    *dy = p->scroll_dy;
    p->scroll_dx = p->scroll_dy = 0;
}

static char *sdl_get_file(const struct window *window)
{
    struct priv *p = (struct priv *) window;
    if (p->file_seen) {
        assert(p->files_num);
        SDL_free(p->files[0]);
        memmove(&p->files[0], &p->files[1], --p->files_num * sizeof(char *));
        p->file_seen = false;
    }

    if (!p->files_num)
        return NULL;

    p->file_seen = true;
    return p->files[0];
}

const struct window_impl IMPL = {
    .name = IMPL_NAME,
    .create = sdl_create,
    .destroy = sdl_destroy,
    .poll = sdl_poll,
    .get_cursor = sdl_get_cursor,
    .get_button = sdl_get_button,
    .get_scroll = sdl_get_scroll,
    .get_file = sdl_get_file,
};
