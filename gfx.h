#ifndef VAAT_GFX_H
#define VAAT_GFX_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

struct wl_display;
struct wl_surface;
struct Player;

struct Gfx * gfxCreate(struct wl_display * display, struct wl_surface * surface, int width, int height, struct Player * player);
void gfxDestroy(struct Gfx * gfx);

void gfxRender(struct Gfx * gfx);

#endif
