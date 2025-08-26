#include "app.h"

#include "gfx.h"
#include "player.h"
#include "util.h"

#include <gst/gst.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

// --------------------------------------------------------------------------------------
// Forward decls

static void appRegisterGlobal(void * data, struct wl_registry * registry, uint32_t name, char const * interface, uint32_t version);
static void appRegisterRemove(void * data, struct wl_registry * registry, uint32_t name);
static const struct wl_registry_listener registryListener = { appRegisterGlobal, appRegisterRemove };

static void xdgSurfaceConfigure(void * data, struct xdg_surface * surface, uint32_t serial);
static const struct xdg_surface_listener xdgSurfaceListener = { xdgSurfaceConfigure };

static void xdgToplevelConfigure(void * data, struct xdg_toplevel * toplevel, int32_t width, int32_t height, struct wl_array * states);
static void xdgToplevelClose(void * data, struct xdg_toplevel * xdg_toplevel);
static void xdgToplevelConfigureBounds(void * data, struct xdg_toplevel * xdg_toplevel, int32_t width, int32_t height);
static void xdgToplevelWMCapabilities(void * data, struct xdg_toplevel * xdg_toplevel, struct wl_array * capabilities);
static const struct xdg_toplevel_listener xdgToplevelListener = {
    xdgToplevelConfigure,
    xdgToplevelClose,
    xdgToplevelConfigureBounds,
    xdgToplevelWMCapabilities,
};

// --------------------------------------------------------------------------------------
// app

struct App
{
    // Connection
    struct wl_display * display;
    struct wl_registry * registry;

    // Interfaces
    struct wl_compositor * interfaceCompositor;
    struct wp_viewporter * interfaceViewporter;
    struct xdg_wm_base * interfaceWmBase;

    // Objects
    struct wl_surface * surface;
    struct xdg_surface * xdgSurface;
    struct xdg_toplevel * xdgToplevel;

    struct Gfx * gfx;
    struct Player * player;

    int dispatchRunning;
    struct Task * dispatchThread;

    // state
    uint32_t width;
    uint32_t height;
};

static void appDispatchThread(struct App * app)
{
    printf("appDispatchThread(): dispatch start\n");

    int ret = 0;
    while (app->dispatchRunning && (ret != -1)) {
        ret = wl_display_dispatch(app->display);

        printf("appDispatchThread(): dispatch loop\n");
    }

    printf("appDispatchThread(): dispatch end\n");
}

struct App * appCreate()
{
    struct App * app = calloc(1, sizeof(struct App));

    app->width = 3840;
    app->height = 2160;

    app->display = wl_display_connect(NULL);
    app->registry = wl_display_get_registry(app->display);
    wl_registry_add_listener(app->registry, &registryListener, app);
    wl_display_roundtrip(app->display); // roundtrip once to register listeners
    wl_display_roundtrip(app->display); // roundtrip again to listen

    if (!app->interfaceCompositor) {
        fatal("Wayland didn't provide a compositor!");
    }
    if (!app->interfaceViewporter) {
        fatal("Wayland didn't provide a viewporter!");
    }
    if (!app->interfaceWmBase) {
        fatal("Wayland didn't provide a wm_base!");
    }

    app->surface = wl_compositor_create_surface(app->interfaceCompositor);
    if (!app->surface) {
        fatal("wl_compositor_create_surface() failed");
    }

    app->xdgSurface = xdg_wm_base_get_xdg_surface(app->interfaceWmBase, app->surface);
    xdg_surface_add_listener(app->xdgSurface, &xdgSurfaceListener, app);
    if (!app->xdgSurface) {
        fatal("xdg_wm_base_get_xdg_surface() failed");
    }

    app->xdgToplevel = xdg_surface_get_toplevel(app->xdgSurface);
    xdg_toplevel_add_listener(app->xdgToplevel, &xdgToplevelListener, app);
    if (!app->xdgToplevel) {
        fatal("xdg_surface_get_toplevel() failed");
    }
    xdg_toplevel_set_title(app->xdgToplevel, "vaat");
    xdg_toplevel_set_fullscreen(app->xdgToplevel, NULL);

    wl_display_roundtrip(app->display);

    app->player = playerCreate();
    app->gfx = gfxCreate(app->display, app->surface, app->width, app->height, app->player);

    wl_surface_commit(app->surface);

    app->dispatchRunning = 1;
    app->dispatchThread = taskCreate((TaskFunc)appDispatchThread, app);
    return app;
}

void appDestroy(struct App * app)
{
    // TODO: implement

    // app->dispatchRunning = 0;
    // taskDestroy(app->dispatchThread);
    // free(app);
}

// --------------------------------------------------------------------------------------
// Listener: xdg_wm_base_listener

static void xdgWMBasePing(void * data, struct xdg_wm_base * shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = {
    xdgWMBasePing,
};

// --------------------------------------------------------------------------------------
// Listener: xdg_surface_listener

static void xdgSurfaceConfigure(void * data, struct xdg_surface * surface, uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

// --------------------------------------------------------------------------------------
// Listener: xdg_toplevel_listener

static void xdgToplevelConfigure(void * data, struct xdg_toplevel * toplevel, int32_t width, int32_t height, struct wl_array * states)
{
}

static void xdgToplevelClose(void * data, struct xdg_toplevel * xdg_toplevel)
{
    // kill app?
}

static void xdgToplevelConfigureBounds(void * data, struct xdg_toplevel * xdg_toplevel, int32_t width, int32_t height)
{
}

static void xdgToplevelWMCapabilities(void * data, struct xdg_toplevel * xdg_toplevel, struct wl_array * capabilities)
{
}

// --------------------------------------------------------------------------------------
// Listener: wl_registry_listener

static void appRegisterGlobal(void * data, struct wl_registry * registry, uint32_t name, char const * interface, uint32_t version)
{
    struct App * app = (struct App *)data;

    printf("appRegisterGlobal: %s [%u]\n", interface, version);

    if (strcmp(interface, "wl_compositor") == 0) {
        app->interfaceCompositor = (struct wl_compositor *)wl_registry_bind(registry, name, &wl_compositor_interface, 4);

    } else if (strcmp(interface, "wp_viewporter") == 0) {
        app->interfaceViewporter = (struct wp_viewporter *)wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        app->interfaceWmBase = (struct xdg_wm_base *)wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->interfaceWmBase, &wmBaseListener, app);
    }
}

static void appRegisterRemove(void * data, struct wl_registry * registry, uint32_t name)
{
}

// --------------------------------------------------------------------------------------

static void gmainThread(void * ignored)
{
    printf("gmainThread begin\n");

    GMainLoop * mainLoop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(mainLoop);

    printf("gmainThread end\n");
}

// --------------------------------------------------------------------------------------

int main(int argc, char * argv[])
{
    gst_init(NULL, NULL);
    taskCreate((TaskFunc)gmainThread, NULL);

    struct App * app = appCreate();
    for (;;) {
        printf("rendering graphics...\n");
        gfxRender(app->gfx);
        usleep(1000000 / 60);
    }

    appDestroy(app);
    return 0;
}
