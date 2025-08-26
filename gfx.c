#include "gfx.h"
#include "player.h"
#include "util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-egl.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/gl/egl/gsteglimage.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/video/video-info-dma.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

#include <drm/drm_fourcc.h>

static const char * vertexShaderSource = "attribute vec2 position;\n"
                                         "attribute vec2 texCoord;\n"
                                         "varying vec2 v_texCoord;\n"
                                         "void main() {\n"
                                         "    gl_Position = vec4(position, 0.0, 1.0);\n"
                                         "    v_texCoord = texCoord;\n"
                                         "}\n";

static const char * fragmentShaderSource = "precision mediump float;\n"
                                           "varying vec2 v_texCoord;\n"
                                           "uniform sampler2D u_texture;\n"
                                           "void main() {\n"
                                           "    gl_FragColor = texture2D(u_texture, v_texCoord);\n"
                                           "}\n";

static const char * yuvVertexShaderSource = "attribute vec2 position;\n"
                                            "attribute vec2 texCoord;\n"
                                            "varying vec2 v_texCoord;\n"
                                            "void main() {\n"
                                            "    gl_Position = vec4(position, 0.0, 1.0);\n"
                                            "    v_texCoord = texCoord;\n"
                                            "}\n";

static const char * yuvFragmentShaderSource = "precision mediump float;\n"
                                              "varying vec2 v_texCoord;\n"
                                              "uniform sampler2D u_textureY;\n"
                                              "uniform sampler2D u_textureUV;\n"
                                              "uniform int u_hasUV;\n"
                                              "void main() {\n"
                                              "    vec2 yCoord = v_texCoord;\n"
                                              "    vec2 uvCoord = v_texCoord;\n"
                                              "    float y = texture2D(u_textureY, yCoord).r;\n"
                                              "    if (u_hasUV == 1) {\n"
                                              "        vec2 uv_sample = texture2D(u_textureUV, uvCoord).rg - 0.5;\n"
                                              "        float u = uv_sample.r;\n"
                                              "        float v = uv_sample.g;\n"
                                              "        float r = y + 1.5748 * v;\n"
                                              "        float g = y - 0.1873 * u - 0.4681 * v;\n"
                                              "        float b = y + 1.8556 * u;\n"
                                              "        gl_FragColor = vec4(r, g, b, 1.0);\n"
                                              "    } else {\n"
                                              "        gl_FragColor = vec4(y, y, y, 1.0);\n"
                                              "    }\n"
                                              "}\n";

// clang-format off
static unsigned char debugTextureData[] = { 255,   0,   0, 255,
                                              0, 255,   0, 255,
                                              0,   0, 255, 255,
                                            255, 255,   0, 255 };

static GLfloat convertVertices[] = { -1.0f, -1.0f,  0.0f,  0.0f,
                                      1.0f, -1.0f,  1.0f,  0.0f,
                                      1.0f,  1.0f,  1.0f,  1.0f,
                                     -1.0f,  1.0f,  0.0f,  1.0f };

static GLfloat renderVertices[]  = { -1.0f, -1.0f,  0.0f,  1.0f,
                                      1.0f, -1.0f,  1.0f,  1.0f,
                                      1.0f,  1.0f,  1.0f,  0.0f,
                                     -1.0f,  1.0f,  0.0f,  0.0f };

static GLuint indices[] = { 0, 1, 2, 2, 3, 0 };
// clang-format on

static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

struct Gfx
{
    struct wl_egl_window * eglNative;
    EGLSurface eglSurface;
    EGLContext eglContext;
    EGLConfig eglConfig;
    EGLDisplay eglDisplay;

    GLuint shaderProgram;
    GLuint yuvShaderProgram;
    GLuint debugTexture;
    GLuint videoTexture;
    GLuint rgbTexture;
    GLuint framebuffer;

    int width;
    int height;

    int videoWidth;
    int videoHeight;

    struct Player * player;
    GstSample * sample;
};

struct Gfx * gfxCreate(struct wl_display * display, struct wl_surface * surface, int width, int height, struct Player * player)
{
    struct Gfx * gfx = calloc(1, sizeof(struct Gfx));
    gfx->width = width;
    gfx->height = height;
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

    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        printf("Vertex shader compilation failed: %s\n", infoLog);
        fatal("Vertex shader compilation failed");
    }

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        printf("Fragment shader compilation failed: %s\n", infoLog);
        fatal("Fragment shader compilation failed");
    }

    gfx->shaderProgram = glCreateProgram();
    glAttachShader(gfx->shaderProgram, vertexShader);
    glAttachShader(gfx->shaderProgram, fragmentShader);
    glLinkProgram(gfx->shaderProgram);

    glGetProgramiv(gfx->shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(gfx->shaderProgram, 512, NULL, infoLog);
        printf("Shader program linking failed: %s\n", infoLog);
        fatal("Shader program linking failed");
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    glGenTextures(1, &gfx->debugTexture);
    glBindTexture(GL_TEXTURE_2D, gfx->debugTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, debugTextureData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    GLuint yuvVertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(yuvVertexShader, 1, &yuvVertexShaderSource, NULL);
    glCompileShader(yuvVertexShader);

    glGetShaderiv(yuvVertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(yuvVertexShader, 512, NULL, infoLog);
        printf("YUV vertex shader compilation failed: %s\n", infoLog);
        fatal("YUV vertex shader compilation failed");
    }

    GLuint yuvFragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(yuvFragmentShader, 1, &yuvFragmentShaderSource, NULL);
    glCompileShader(yuvFragmentShader);

    glGetShaderiv(yuvFragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(yuvFragmentShader, 512, NULL, infoLog);
        printf("YUV fragment shader compilation failed: %s\n", infoLog);
        fatal("YUV fragment shader compilation failed");
    }

    gfx->yuvShaderProgram = glCreateProgram();
    glAttachShader(gfx->yuvShaderProgram, yuvVertexShader);
    glAttachShader(gfx->yuvShaderProgram, yuvFragmentShader);
    glLinkProgram(gfx->yuvShaderProgram);

    glGetProgramiv(gfx->yuvShaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(gfx->yuvShaderProgram, 512, NULL, infoLog);
        printf("YUV shader program linking failed: %s\n", infoLog);
        fatal("YUV shader program linking failed");
    }

    glDeleteShader(yuvVertexShader);
    glDeleteShader(yuvFragmentShader);

    return gfx;
}

void gfxDestroy(struct Gfx * gfx)
{
    if (!gfx)
        return;

    if (gfx->sample) {
        gst_sample_unref(gfx->sample);
    }

    if (gfx->debugTexture) {
        glDeleteTextures(1, &gfx->debugTexture);
    }
    if (gfx->rgbTexture) {
        glDeleteTextures(1, &gfx->rgbTexture);
    }
    if (gfx->framebuffer) {
        glDeleteFramebuffers(1, &gfx->framebuffer);
    }

    if (gfx->shaderProgram) {
        glDeleteProgram(gfx->shaderProgram);
    }
    if (gfx->yuvShaderProgram) {
        glDeleteProgram(gfx->yuvShaderProgram);
    }

    if (gfx->eglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(gfx->eglDisplay, gfx->eglContext);
    }
    if (gfx->eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(gfx->eglDisplay, gfx->eglSurface);
    }
    if (gfx->eglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(gfx->eglDisplay);
    }
    if (gfx->eglNative) {
        wl_egl_window_destroy(gfx->eglNative);
    }

    free(gfx);
}

// returns non-zero on success
static int gfxConvertSample(struct Gfx * gfx)
{
    GstBuffer * buffer = gst_sample_get_buffer(gfx->sample);
    gint64 const pts = (gint64)GST_BUFFER_PTS(buffer);
    GstCaps * caps = gst_sample_get_caps(gfx->sample);

    // debug dump some info about the sample
    gchar * capsString = gst_caps_to_string(caps);
    printf("adopted [%3.3f]: %s\n", (double)pts / 1000000000.0, capsString);
    g_free(capsString);

    if (!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        printf("EGL extensions not available\n");
        return 0;
    }

    GstVideoInfoDmaDrm dma_info;
    if (!gst_video_info_dma_drm_from_caps(&dma_info, caps)) {
        printf("Failed to get DMA DRM video info from caps\n");
        return 0;
    }

    gint width = GST_VIDEO_INFO_WIDTH(&dma_info.vinfo);
    gint height = GST_VIDEO_INFO_HEIGHT(&dma_info.vinfo);
    guint32 fourcc = dma_info.drm_fourcc;

    if (fourcc != DRM_FORMAT_NV12 && fourcc != DRM_FORMAT_NV21) {
        printf("Unsupported DRM fourcc: 0x%08x\n", fourcc);
        return 0;
    }

    // Get DMA-BUF fd from first memory block
    GstMemory * mem = gst_buffer_peek_memory(buffer, 0);
    if (!mem || !gst_is_dmabuf_memory(mem)) {
        printf("Buffer is not DMA-BUF memory\n");
        return 0;
    }

    gint fd = gst_dmabuf_memory_get_fd(mem);
    if (fd < 0) {
        printf("Failed to get DMA-BUF fd\n");
        return 0;
    }

    // Try to get stride/offset from VideoMeta first, fall back to VideoInfo
    gsize y_offset, uv_offset;
    gint y_stride, uv_stride;

    GstVideoMeta * video_meta = gst_buffer_get_video_meta(buffer);
    if (video_meta) {
        y_offset = video_meta->offset[0];
        y_stride = video_meta->stride[0];
        uv_offset = video_meta->offset[1];
        uv_stride = video_meta->stride[1];
        // printf("Using VideoMeta: Y stride=%d offset=%zu, UV stride=%d offset=%zu\n", y_stride, y_offset, uv_stride, uv_offset);
    } else {
        // Fall back to GstVideoInfo plane offsets
        y_offset = GST_VIDEO_INFO_PLANE_OFFSET(&dma_info.vinfo, 0);
        y_stride = GST_VIDEO_INFO_PLANE_STRIDE(&dma_info.vinfo, 0);
        uv_offset = GST_VIDEO_INFO_PLANE_OFFSET(&dma_info.vinfo, 1);
        uv_stride = GST_VIDEO_INFO_PLANE_STRIDE(&dma_info.vinfo, 1);
        // printf("Using VideoInfo: Y stride=%d offset=%zu, UV stride=%d offset=%zu\n", y_stride, y_offset, uv_stride, uv_offset);
    }

    // printf("DMA-BUF fd=%d, Y: offset=%zu stride=%d, UV: offset=%zu stride=%d, fourcc=0x%08x, %dx%d\n",
    //        fd,
    //        y_offset,
    //        y_stride,
    //        uv_offset,
    //        uv_stride,
    //        fourcc,
    //        width,
    //        height);

    GLuint externalTexture;
    glGenTextures(1, &externalTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);

    EGLImage image = EGL_NO_IMAGE;

    if (gfx->videoWidth != width || gfx->videoHeight != height) {
        if (gfx->rgbTexture) {
            glDeleteTextures(1, &gfx->rgbTexture);
        }
        if (gfx->framebuffer) {
            glDeleteFramebuffers(1, &gfx->framebuffer);
        }

        glGenTextures(1, &gfx->rgbTexture);
        glBindTexture(GL_TEXTURE_2D, gfx->rgbTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // printf("Created RGB texture %d (%dx%d)\n", gfx->rgbTexture, width, height);

        glGenFramebuffers(1, &gfx->framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, gfx->framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gfx->rgbTexture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        // printf("Framebuffer status: 0x%x (complete=0x%x)\n", status, GL_FRAMEBUFFER_COMPLETE);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            printf("Framebuffer is not complete\n");
            glDeleteTextures(1, &externalTexture);
            eglDestroyImageKHR(gfx->eglDisplay, image);
            return 0;
        }

        gfx->videoWidth = width;
        gfx->videoHeight = height;
    }

    // Let's try just filling the RGB texture with a solid color to test
    glBindFramebuffer(GL_FRAMEBUFFER, gfx->framebuffer);
    glViewport(0, 0, width, height);
    glClearColor(1.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // Unbind framebuffer

    // Create Y plane texture
    EGLint yAttribs[] = { EGL_WIDTH,
                          width,
                          EGL_HEIGHT,
                          height,
                          EGL_LINUX_DRM_FOURCC_EXT,
                          DRM_FORMAT_R8,
                          EGL_DMA_BUF_PLANE0_FD_EXT,
                          fd,
                          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                          y_offset,
                          EGL_DMA_BUF_PLANE0_PITCH_EXT,
                          y_stride,
                          EGL_NONE };

    EGLImage yImage = eglCreateImageKHR(gfx->eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, yAttribs);
    if (yImage == EGL_NO_IMAGE) {
        printf("Failed to create Y plane image\n");
        glDeleteTextures(1, &externalTexture);
        return 0;
    }

    GLuint yTexture;
    glGenTextures(1, &yTexture);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, yImage);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint uvTexture = 0;
    EGLImage uvImage = EGL_NO_IMAGE;

    if (fourcc == DRM_FORMAT_NV12) {
        EGLint uvAttribs[] = { EGL_WIDTH,
                               width / 2,
                               EGL_HEIGHT,
                               height / 2,
                               EGL_LINUX_DRM_FOURCC_EXT,
                               DRM_FORMAT_GR88,
                               EGL_DMA_BUF_PLANE0_FD_EXT,
                               fd,
                               EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                               uv_offset,
                               EGL_DMA_BUF_PLANE0_PITCH_EXT,
                               uv_stride,
                               EGL_NONE };

        uvImage = eglCreateImageKHR(gfx->eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, uvAttribs);
        if (uvImage == EGL_NO_IMAGE) {
            printf("Failed to create UV plane image with GR88\n");
            uvTexture = 0;
        } else {
            printf("GR88 format worked!\n");
            glGenTextures(1, &uvTexture);
            glBindTexture(GL_TEXTURE_2D, uvTexture);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, uvImage);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    // Render YUV to RGB
    GLint oldViewport[4];
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    GLint oldFramebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFramebuffer);
    GLint oldProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    GLint oldActiveTexture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    GLint oldTexture0, oldTexture1;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture1);

    // Save vertex attribute array state
    GLint oldArrayBuffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);

    glBindFramebuffer(GL_FRAMEBUFFER, gfx->framebuffer);
    glViewport(0, 0, width, height);
    glUseProgram(gfx->yuvShaderProgram);

    GLint positionAttrib = glGetAttribLocation(gfx->yuvShaderProgram, "position");
    GLint texCoordAttrib = glGetAttribLocation(gfx->yuvShaderProgram, "texCoord");
    GLint yTextureUniform = glGetUniformLocation(gfx->yuvShaderProgram, "u_textureY");
    GLint uvTextureUniform = glGetUniformLocation(gfx->yuvShaderProgram, "u_textureUV");
    GLint hasUVUniform = glGetUniformLocation(gfx->yuvShaderProgram, "u_hasUV");

    glEnableVertexAttribArray(positionAttrib);
    glVertexAttribPointer(positionAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), convertVertices);
    glEnableVertexAttribArray(texCoordAttrib);
    glVertexAttribPointer(texCoordAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), convertVertices + 2);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, yTexture);
    glUniform1i(yTextureUniform, 0);

    if (uvTexture != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uvTexture);
        glUniform1i(uvTextureUniform, 1);
        glUniform1i(hasUVUniform, 1);
    } else {
        glUniform1i(hasUVUniform, 0);
    }

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices);

    // Restore all OpenGL state
    glBindFramebuffer(GL_FRAMEBUFFER, oldFramebuffer);
    glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glUseProgram(oldProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, oldTexture0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, oldTexture1);
    glActiveTexture(oldActiveTexture);
    glBindBuffer(GL_ARRAY_BUFFER, oldArrayBuffer);
    glDisableVertexAttribArray(positionAttrib);
    glDisableVertexAttribArray(texCoordAttrib);

    // Cleanup
    glDeleteTextures(1, &yTexture);
    if (uvTexture)
        glDeleteTextures(1, &uvTexture);
    eglDestroyImageKHR(gfx->eglDisplay, yImage);
    if (uvImage != EGL_NO_IMAGE)
        eglDestroyImageKHR(gfx->eglDisplay, uvImage);

    // printf("Rendered YUV planes to RGB texture\n");

    glDeleteTextures(1, &externalTexture);
    eglDestroyImageKHR(gfx->eglDisplay, image);
    return 1;
}

void gfxRender(struct Gfx * gfx)
{
    GstSample * sample = playerAdoptSample(gfx->player);

    if (sample) {
        if (gfx->sample) {
            gst_sample_unref(gfx->sample);
        }
        gfx->sample = sample;

        if (gfxConvertSample(gfx)) {
            if (gfx->videoTexture && gfx->videoTexture != gfx->rgbTexture) {
                glDeleteTextures(1, &gfx->videoTexture);
            }
            gfx->videoTexture = gfx->rgbTexture;
        } else {
            gfx->videoTexture = 0;
        }
    }

    GLint positionAttrib = glGetAttribLocation(gfx->shaderProgram, "position");
    GLint texCoordAttrib = glGetAttribLocation(gfx->shaderProgram, "texCoord");
    GLint textureUniform = glGetUniformLocation(gfx->shaderProgram, "u_texture");

    glViewport(0, 0, gfx->width, gfx->height);
    glClearColor(0.0, 0.0, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(gfx->shaderProgram);

    glEnableVertexAttribArray(positionAttrib);
    glVertexAttribPointer(positionAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), renderVertices);

    glEnableVertexAttribArray(texCoordAttrib);
    glVertexAttribPointer(texCoordAttrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), renderVertices + 2);

    glActiveTexture(GL_TEXTURE0);
    if (gfx->videoTexture) {
        // printf("Using video texture %d\n", gfx->videoTexture);
        glBindTexture(GL_TEXTURE_2D, gfx->videoTexture);
    } else {
        // printf("Using debug texture %d\n", gfx->debugTexture);
        glBindTexture(GL_TEXTURE_2D, gfx->debugTexture);
    }
    glUniform1i(textureUniform, 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices);
    eglSwapBuffers(gfx->eglDisplay, gfx->eglSurface);
}
