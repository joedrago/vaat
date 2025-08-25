#include "gfx.h"
#include "player.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <wayland-egl.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/gl/egl/gsteglimage.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/video/videooverlay.h>

// clang-format off
static const char* vertexShaderSource =
    "attribute vec2 position;\n"
    "attribute vec2 texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    v_texCoord = texCoord;\n"
    "}\n";

static const char* fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_texture, v_texCoord);\n"
    "}\n";

static unsigned char debugTextureData[] = { 255,   0,   0, 255,
                                              0, 255,   0, 255,
                                              0,   0, 255, 255,
                                            255, 255,   0, 255 };

static GLfloat vertices[] = { -0.5f, -0.5f,  0.0f,  0.0f,
                               0.5f, -0.5f,  1.0f,  0.0f,
                               0.5f,  0.5f,  1.0f,  1.0f,
                              -0.5f,  0.5f,  0.0f,  1.0f };

static GLuint indices[] = { 0, 1, 2, 2, 3, 0 };
// clang-format on

struct Gfx
{
    struct wl_egl_window * eglNative;
    EGLSurface eglSurface;
    EGLContext eglContext;
    EGLConfig eglConfig;
    EGLDisplay eglDisplay;

    GLuint shaderProgram;
    GLuint debugTexture;
    GLuint videoTexture;

    struct Player * player;
    GstSample * sample;
};

struct Gfx * gfxCreate(struct wl_display * display, struct wl_surface * surface, int width, int height, struct Player * player)
{
    struct Gfx * gfx = calloc(1, sizeof(struct Gfx));
    gfx->player = player;

    gfx->eglNative = wl_egl_window_create(surface, width, height);
    if (!gfx->eglNative) {
        fatal("wl_egl_window_create() failed");
    }

    EGLint numConfigs;
    EGLint majorVersion;
    EGLint minorVersion;
    EGLint fbAttribs[] = { EGL_SURFACE_TYPE,
                           EGL_WINDOW_BIT,
                           EGL_RENDERABLE_TYPE,
                           EGL_OPENGL_ES2_BIT,
                           EGL_RED_SIZE,
                           8,
                           EGL_GREEN_SIZE,
                           8,
                           EGL_BLUE_SIZE,
                           8,
                           EGL_NONE };
    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };
    gfx->eglDisplay = eglGetDisplay(display);
    if (gfx->eglDisplay == EGL_NO_DISPLAY) {
        fatal("eglGetDisplay() failed");
    }

    if (!eglInitialize(gfx->eglDisplay, &majorVersion, &minorVersion)) {
        fatal("eglInitialize() failed");
    }

    if ((eglGetConfigs(gfx->eglDisplay, NULL, 0, &numConfigs) != EGL_TRUE) || (numConfigs == 0)) {
        fatal("eglGetConfigs() failed");
    }

    if ((eglChooseConfig(gfx->eglDisplay, fbAttribs, &gfx->eglConfig, 1, &numConfigs) != EGL_TRUE) || (numConfigs != 1)) {
        fatal("eglChooseConfig() failed");
    }

    gfx->eglSurface = eglCreateWindowSurface(gfx->eglDisplay, gfx->eglConfig, (EGLNativeWindowType)gfx->eglNative, NULL);
    if (gfx->eglSurface == EGL_NO_SURFACE) {
        fatal("eglCreateWindowSurface() failed");
    }

    gfx->eglContext = eglCreateContext(gfx->eglDisplay, gfx->eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (gfx->eglContext == EGL_NO_CONTEXT) {
        fatal("eglCreateContext() failed");
    }

    // Make the context current
    if (!eglMakeCurrent(gfx->eglDisplay, gfx->eglSurface, gfx->eglSurface, gfx->eglContext)) {
        fatal("eglMakeCurrent() failed");
    }

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);

    gfx->shaderProgram = glCreateProgram();
    glAttachShader(gfx->shaderProgram, vertexShader);
    glAttachShader(gfx->shaderProgram, fragmentShader);
    glLinkProgram(gfx->shaderProgram);

    glGenTextures(1, &gfx->debugTexture);
    glBindTexture(GL_TEXTURE_2D, gfx->debugTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, debugTextureData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return gfx;
}

void gfxDestroy(struct Gfx * gfx)
{
    // TODO: implement

    // free(gfx);
}

// returns non-zero on success
static int gfxConvertSample(struct Gfx * gfx, GLuint * outTexture)
{
    GstBuffer * buffer = gst_sample_get_buffer(gfx->sample);
    gint64 const pts = (gint64)GST_BUFFER_PTS(buffer);
    GstCaps * caps = gst_sample_get_caps(gfx->sample);

    // debug dump some info about the sample
    gchar * capsString = gst_caps_to_string(caps);
    printf("adopted [%3.3f]: %s\n", (double)pts / 1000000000.0, capsString);
    g_free(capsString);

#if 0
    GstVideoInfoDmaDrm * videoInfo = gst_video_info_dma_drm_new_from_caps(caps);

    guint idx = 0;
    guint length = 0;
    gsize skip = 0;
    if (gst_buffer_find_memory(buffer, 0, 1, &idx, &length, &skip)) {
        printf("gst_buffer_find_memory returned true %u %u %zu\n", idx, length, skip);
    } else {
        printf("gst_buffer_find_memory returned false\n");
    }

    // if (videoInfo) {
    //     // printf("unref video info\n");
    //     // gst_object_unref(GST_OBJECT(videoInfo));
    // }

    GstGLDisplayEGL * gstEglDisplay = gst_gl_display_egl_new_with_egl_display(gfx->eglDisplay);

    GError * error = NULL;

    GstGLContext * gstParentContext =
        gst_gl_context_new_wrapped((GstGLDisplay *)gstEglDisplay, (guintptr)gfx->eglContext, GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);
    gst_gl_context_activate(gstParentContext, TRUE);
    if (!gst_gl_context_fill_info(gstParentContext, &error)) {
        printf("gst_gl_context_fill_info bad\n");
    }

    if (error) {
        printf("gst_gl_context_fill_info error: %s\n", error->message);
        g_error_free(error);
        error = NULL;
    }

    GstGLContext * gstEglContext = gst_gl_context_new((GstGLDisplay *)gstEglDisplay);
    gst_gl_context_create(gstEglContext, NULL /*gstParentContext*/, &error);
    gst_gl_context_activate(gstEglContext, TRUE);
    if (error) {
        printf("gst_gl_context_create error: %s\n", error->message);
        g_error_free(error);
        error = NULL;
    }

    assert(gstEglDisplay);
    assert(gstParentContext);
    assert(gstEglContext);

    gint fd = gst_dmabuf_memory_get_fd(gst_buffer_peek_memory(buffer, idx));
    printf("got fd %u\b", fd);

    GstEGLImage * gstImage = gst_egl_image_from_dmabuf_direct(gfx->eglContext, &fd, &skip, &videoInfo->vinfo);
    gst_object_unref(GST_OBJECT(gstImage));

    if (gstEglContext) {
        gst_gl_context_destroy(gstEglContext);
        gst_object_unref(GST_OBJECT(gstEglContext));
    }
    // if (gstParentContext) {
    //     gst_object_unref(GST_OBJECT(gstParentContext));
    // }
    if (gstEglDisplay) {
        gst_object_unref(GST_OBJECT(gstEglDisplay));
    }

#endif
    return 0;
}

void gfxRender(struct Gfx * gfx)
{
    GstSample * sample = playerAdoptSample(gfx->player);

    if (sample) {
        if (gfx->sample) {
            gst_sample_unref(gfx->sample);
        }
        gfx->sample = sample;

        GLuint outTexture = 0;
        if (gfxConvertSample(gfx, &outTexture)) {
            // TODO: cleanup old videoTexture
            gfx->videoTexture = outTexture;
        } else {
            gfx->videoTexture = 0;
        }
    }

    GLint positionAttrib = glGetAttribLocation(gfx->shaderProgram, "position");
    GLint texCoordAttrib = glGetAttribLocation(gfx->shaderProgram, "texCoord");
    GLint textureUniform = glGetUniformLocation(gfx->shaderProgram, "u_texture");

    glClearColor(0.0, 0.0, 0.1, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(gfx->shaderProgram);

    glEnableVertexAttribArray(positionAttrib);
    glVertexAttribPointer(positionAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);

    glEnableVertexAttribArray(texCoordAttrib);
    glVertexAttribPointer(texCoordAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);

    glActiveTexture(GL_TEXTURE0);
    if (gfx->videoTexture) {
        glBindTexture(GL_TEXTURE_2D, gfx->videoTexture);
    } else {
        glBindTexture(GL_TEXTURE_2D, gfx->debugTexture);
    }
    glUniform1i(textureUniform, 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices);
    eglSwapBuffers(gfx->eglDisplay, gfx->eglSurface);
}
