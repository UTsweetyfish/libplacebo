// License: CC0 / Public Domain

#include "common.h"
#include "window.h"

extern const struct window_impl win_impl_glfw_vk;
extern const struct window_impl win_impl_glfw_gl;
extern const struct window_impl win_impl_sdl_vk;
extern const struct window_impl win_impl_sdl_gl;

static const struct window_impl *win_impls[] = {
#ifdef HAVE_GLFW
# ifdef PL_HAVE_VULKAN
    &win_impl_glfw_vk,
# endif
# ifdef PL_HAVE_OPENGL
    &win_impl_glfw_gl,
# endif
#endif // HAVE_GLFW

#ifdef HAVE_SDL
# ifdef PL_HAVE_VULKAN
    &win_impl_sdl_vk,
# endif
# ifdef PL_HAVE_OPENGL
    &win_impl_sdl_gl,
# endif
#endif // HAVE_SDL
    NULL
};

struct window *window_create(struct pl_context *ctx, const char *title,
                             int width, int height, enum winflags flags)
{
    for (const struct window_impl **impl = win_impls; *impl; impl++) {
        printf("Attempting to initialize API: %s\n", (*impl)->name);
        struct window *win = (*impl)->create(ctx, title, width, height, flags);
        if (win)
            return win;
    }

    fprintf(stderr, "No windowing system / graphical API compiled or supported!");
    exit(1);
}

void window_destroy(struct window **win)
{
    if (*win)
        (*win)->impl->destroy(win);
}

void window_poll(struct window *win, bool block)
{
    return win->impl->poll(win, block);
}

void window_get_cursor(const struct window *win, int *x, int *y)
{
    return win->impl->get_cursor(win, x, y);
}

void window_get_scroll(const struct window *win, float *dx, float *dy)
{
    return win->impl->get_scroll(win, dx, dy);
}

bool window_get_button(const struct window *win, enum button btn)
{
    return win->impl->get_button(win, btn);
}

char *window_get_file(const struct window *win)
{
    return win->impl->get_file(win);
}
