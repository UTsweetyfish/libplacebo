// License: CC0 / Public Domain

#if !defined(USE_GL) && !defined(USE_VK) || defined(USE_GL) && defined(USE_VK)
#error Specify exactly one of -DUSE_GL or -DUSE_VK when compiling!
#endif

#include <string.h>

#include "common.h"
#include "window.h"

#ifdef USE_VK
#include <libplacebo/vulkan.h>
#define GLFW_INCLUDE_VULKAN
#define IMPL win_impl_glfw_vk
#define IMPL_NAME "GLFW (vulkan)"
#endif

#ifdef USE_GL
#include <libplacebo/opengl.h>
#define IMPL win_impl_glfw_gl
#define IMPL_NAME "GLFW (opengl)"
#endif

#include <GLFW/glfw3.h>

#ifdef NDEBUG
#define DEBUG false
#else
#define DEBUG true
#endif

const struct window_impl IMPL;

struct priv {
    struct window w;
    GLFWwindow *win;

#ifdef USE_VK
    VkSurfaceKHR surf;
    const struct pl_vulkan *vk;
    const struct pl_vk_inst *vk_inst;
#endif

#ifdef USE_GL
    const struct pl_opengl *gl;
#endif

    float scroll_dx, scroll_dy;
    char **files;
    size_t files_num;
    size_t files_size;
    bool file_seen;
};

static void err_cb(int code, const char *desc)
{
    fprintf(stderr, "GLFW err %d: %s\n", code, desc);
}

static void close_cb(GLFWwindow *win)
{
    struct priv *p = glfwGetWindowUserPointer(win);
    p->w.window_lost = true;
}

static void resize_cb(GLFWwindow *win, int width, int height)
{
    struct priv *p = glfwGetWindowUserPointer(win);
    if (!pl_swapchain_resize(p->w.swapchain, &width, &height)) {
        fprintf(stderr, "libplacebo: Failed resizing swapchain? Exiting...\n");
        p->w.window_lost = true;
    }
}

static void scroll_cb(GLFWwindow *win, double dx, double dy)
{
    struct priv *p = glfwGetWindowUserPointer(win);
    p->scroll_dx += dx;
    p->scroll_dy += dy;
}

static void drop_cb(GLFWwindow *win, int num, const char *files[])
{
    struct priv *p = glfwGetWindowUserPointer(win);

    for (int i = 0; i < num; i++) {
        if (p->files_num == p->files_size) {
            size_t new_size = p->files_size ? p->files_size * 2 : 16;
            char **new_files = realloc(p->files, new_size * sizeof(char *));
            if (!new_files)
                return;
            p->files = new_files;
            p->files_size = new_size;
        }

        char *file = strdup(files[i]);
        if (!file)
            return;

        p->files[p->files_num++] = file;
    }
}

static struct window *glfw_create(struct pl_context *ctx, const char *title,
                                  int width, int height, enum winflags flags)
{
    struct priv *p = calloc(1, sizeof(struct priv));
    if (!p)
        return NULL;

    p->w.impl = &IMPL;
    if (!glfwInit()) {
        fprintf(stderr, "GLFW: Failed initializing?\n");
        goto error;
    }

    glfwSetErrorCallback(&err_cb);

#ifdef USE_VK
    if (!glfwVulkanSupported()) {
        fprintf(stderr, "GLFW: No vulkan support! Perhaps recompile with -DUSE_GL\n");
        goto error;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#endif // USE_VK

#ifdef USE_GL
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);

    // Request OpenGL 3.2 (or higher) core profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif // USE_GL

    bool alpha = flags & WIN_ALPHA;
    if (alpha)
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);

    printf("Creating %dx%d window%s...\n", width, height,
           alpha ? " (with alpha)" : "");

    p->win = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!p->win) {
        fprintf(stderr, "GLFW: Failed creating window\n");
        goto error;
    }

    // Set up GLFW event callbacks
    glfwSetWindowUserPointer(p->win, p);
    glfwSetFramebufferSizeCallback(p->win, resize_cb);
    glfwSetWindowCloseCallback(p->win, close_cb);
    glfwSetScrollCallback(p->win, scroll_cb);
    glfwSetDropCallback(p->win, drop_cb);

#ifdef USE_VK
    VkResult err;

    struct pl_vk_inst_params iparams = pl_vk_inst_default_params;
    iparams.debug = DEBUG;

    // Load all extensions required for WSI
    uint32_t num;
    iparams.extensions = glfwGetRequiredInstanceExtensions(&num);
    iparams.num_extensions = num;

    p->vk_inst = pl_vk_inst_create(ctx, &iparams);
    if (!p->vk_inst) {
        fprintf(stderr, "libplacebo: Failed creating vulkan instance\n");
        goto error;
    }

    err = glfwCreateWindowSurface(p->vk_inst->instance, p->win, NULL, &p->surf);
    if (err != VK_SUCCESS) {
        fprintf(stderr, "GLFW: Failed creating vulkan surface\n");
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
    struct pl_opengl_params params = pl_opengl_default_params;
    params.allow_software = true;
    params.debug = DEBUG;

    glfwMakeContextCurrent(p->win);

    p->gl = pl_opengl_create(ctx, &params);
    if (!p->gl) {
        fprintf(stderr, "libplacebo: Failed creating opengl device\n");
        goto error;
    }

    p->w.swapchain = pl_opengl_create_swapchain(p->gl, &(struct pl_opengl_swapchain_params) {
        .swap_buffers = (void (*)(void *)) glfwSwapBuffers,
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

static void glfw_destroy(struct window **window)
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
#endif

    for (int i = 0; i < p->files_num; i++)
        free(p->files[i]);
    free(p->files);

    glfwTerminate();
    free(p);
    *window = NULL;
}

static void glfw_poll(struct window *window, bool block)
{
    if (block) {
        glfwWaitEvents();
    } else {
        glfwPollEvents();
    }
}

static void glfw_get_cursor(const struct window *window, int *x, int *y)
{
    struct priv *p = (struct priv *) window;
    double dx, dy;
    glfwGetCursorPos(p->win, &dx, &dy);
    *x = dx;
    *y = dy;
}

static bool glfw_get_button(const struct window *window, enum button btn)
{
    static const int button_map[] = {
        [BTN_LEFT] = GLFW_MOUSE_BUTTON_LEFT,
        [BTN_RIGHT] = GLFW_MOUSE_BUTTON_RIGHT,
        [BTN_MIDDLE] = GLFW_MOUSE_BUTTON_MIDDLE,
    };

    struct priv *p = (struct priv *) window;
    return glfwGetMouseButton(p->win, button_map[btn]) == GLFW_PRESS;
}

static void glfw_get_scroll(const struct window *window, float *dx, float *dy)
{
    struct priv *p = (struct priv *) window;
    *dx = p->scroll_dx;
    *dy = p->scroll_dy;
    p->scroll_dx = p->scroll_dy = 0.0;
}

static char *glfw_get_file(const struct window *window)
{
    struct priv *p = (struct priv *) window;
    if (p->file_seen) {
        assert(p->files_num);
        free(p->files[0]);
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
    .create = glfw_create,
    .destroy = glfw_destroy,
    .poll = glfw_poll,
    .get_cursor = glfw_get_cursor,
    .get_button = glfw_get_button,
    .get_scroll = glfw_get_scroll,
    .get_file = glfw_get_file,
};
