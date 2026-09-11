/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <vndk/hardware_buffer.h>
#include <private/android/AHardwareBufferHelpers.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

template <typename T>
bool Load(void* library, const char* name, T* function) {
  *function = reinterpret_cast<T>(dlsym(library, name));
  if (*function) return true;
  fprintf(stderr, "aera-gpu-probe: missing %s: %s\n", name, dlerror());
  return false;
}

bool HasExtension(const char* extensions, const char* wanted) {
  if (!extensions || !wanted || !*wanted) return false;
  const size_t wanted_length = strlen(wanted);
  const char* match = extensions;
  while ((match = strstr(match, wanted)) != nullptr) {
    const bool starts_word = match == extensions || match[-1] == ' ';
    const char tail = match[wanted_length];
    if (starts_word && (tail == '\0' || tail == ' ')) return true;
    match += wanted_length;
  }
  return false;
}

}  // namespace

int main() {
  void* egl_library =
      dlopen("/vendor/lib64/egl/libEGL_adreno.so", RTLD_NOW | RTLD_GLOBAL);
  if (!egl_library) {
    fprintf(stderr, "aera-gpu-probe: EGL driver load failed: %s\n", dlerror());
    return 2;
  }
  void* gles_library =
      dlopen("/vendor/lib64/egl/libGLESv2_adreno.so", RTLD_NOW | RTLD_GLOBAL);
  if (!gles_library) {
    fprintf(stderr, "aera-gpu-probe: GLES driver load failed: %s\n", dlerror());
    return 3;
  }

  decltype(&eglGetDisplay) get_display;
  decltype(&eglInitialize) initialize;
  decltype(&eglBindAPI) bind_api;
  decltype(&eglChooseConfig) choose_config;
  decltype(&eglCreatePbufferSurface) create_surface;
  decltype(&eglCreateContext) create_context;
  decltype(&eglMakeCurrent) make_current;
  decltype(&eglGetError) get_error;
  decltype(&eglQueryString) query_string;
  decltype(&eglGetProcAddress) get_proc_address;
  decltype(&eglDestroyContext) destroy_context;
  decltype(&eglDestroySurface) destroy_surface;
  decltype(&eglTerminate) terminate;
  if (!Load(egl_library, "eglGetDisplay", &get_display) ||
      !Load(egl_library, "eglInitialize", &initialize) ||
      !Load(egl_library, "eglBindAPI", &bind_api) ||
      !Load(egl_library, "eglChooseConfig", &choose_config) ||
      !Load(egl_library, "eglCreatePbufferSurface", &create_surface) ||
      !Load(egl_library, "eglCreateContext", &create_context) ||
      !Load(egl_library, "eglMakeCurrent", &make_current) ||
      !Load(egl_library, "eglGetError", &get_error) ||
      !Load(egl_library, "eglQueryString", &query_string) ||
      !Load(egl_library, "eglGetProcAddress", &get_proc_address) ||
      !Load(egl_library, "eglDestroyContext", &destroy_context) ||
      !Load(egl_library, "eglDestroySurface", &destroy_surface) ||
      !Load(egl_library, "eglTerminate", &terminate)) {
    return 4;
  }

  EGLDisplay display = get_display(EGL_DEFAULT_DISPLAY);
  EGLint major = 0;
  EGLint minor = 0;
  if (display == EGL_NO_DISPLAY || !initialize(display, &major, &minor)) {
    fprintf(stderr, "aera-gpu-probe: eglInitialize failed: 0x%x\n", get_error());
    return 5;
  }
  if (!bind_api(EGL_OPENGL_ES_API)) {
    fprintf(stderr, "aera-gpu-probe: eglBindAPI failed: 0x%x\n", get_error());
    terminate(display);
    return 6;
  }

  const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config = nullptr;
  EGLint config_count = 0;
  if (!choose_config(display, config_attributes, &config, 1, &config_count) ||
      config_count != 1) {
    fprintf(stderr, "aera-gpu-probe: no pbuffer config: 0x%x\n", get_error());
    terminate(display);
    return 7;
  }

  const EGLint surface_attributes[] = {
      EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE,
  };
  const EGLint context_attributes[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
  };
  EGLSurface surface = create_surface(display, config, surface_attributes);
  EGLContext context = create_context(display, config, EGL_NO_CONTEXT,
                                      context_attributes);
  if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
      !make_current(display, surface, surface, context)) {
    fprintf(stderr, "aera-gpu-probe: context creation failed: 0x%x\n", get_error());
    if (context != EGL_NO_CONTEXT) destroy_context(display, context);
    if (surface != EGL_NO_SURFACE) destroy_surface(display, surface);
    terminate(display);
    return 8;
  }

  decltype(&glGetString) get_string;
  decltype(&glClearColor) clear_color;
  decltype(&glClear) clear;
  decltype(&glFinish) finish;
  decltype(&glReadPixels) read_pixels;
  decltype(&glGenTextures) gen_textures;
  decltype(&glBindTexture) bind_texture;
  decltype(&glGenFramebuffers) gen_framebuffers;
  decltype(&glBindFramebuffer) bind_framebuffer;
  decltype(&glFramebufferTexture2D) framebuffer_texture_2d;
  decltype(&glCheckFramebufferStatus) check_framebuffer_status;
  decltype(&glViewport) viewport;
  decltype(&glGetError) gl_get_error;
  decltype(&glDeleteFramebuffers) delete_framebuffers;
  decltype(&glDeleteTextures) delete_textures;
  if (!Load(gles_library, "glGetString", &get_string) ||
      !Load(gles_library, "glClearColor", &clear_color) ||
      !Load(gles_library, "glClear", &clear) ||
      !Load(gles_library, "glFinish", &finish) ||
      !Load(gles_library, "glReadPixels", &read_pixels) ||
      !Load(gles_library, "glGenTextures", &gen_textures) ||
      !Load(gles_library, "glBindTexture", &bind_texture) ||
      !Load(gles_library, "glGenFramebuffers", &gen_framebuffers) ||
      !Load(gles_library, "glBindFramebuffer", &bind_framebuffer) ||
      !Load(gles_library, "glFramebufferTexture2D", &framebuffer_texture_2d) ||
      !Load(gles_library, "glCheckFramebufferStatus", &check_framebuffer_status) ||
      !Load(gles_library, "glViewport", &viewport) ||
      !Load(gles_library, "glGetError", &gl_get_error) ||
      !Load(gles_library, "glDeleteFramebuffers", &delete_framebuffers) ||
      !Load(gles_library, "glDeleteTextures", &delete_textures)) {
    return 9;
  }
  clear_color(0.125f, 0.5f, 0.875f, 1.0f);
  clear(GL_COLOR_BUFFER_BIT);
  finish();
  unsigned char pixel[4]{};
  read_pixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  printf("AERA GPU ready: EGL %d.%d, %s, %s, pixel=%u,%u,%u,%u\n",
         major, minor,
         reinterpret_cast<const char*>(get_string(GL_VENDOR)),
         reinterpret_cast<const char*>(get_string(GL_RENDERER)),
         pixel[0], pixel[1], pixel[2], pixel[3]);

  const char* egl_extensions = query_string(display, EGL_EXTENSIONS);
  const char* gl_extensions =
      reinterpret_cast<const char*>(get_string(GL_EXTENSIONS));
  printf("EGL extensions: dma_buf=%d image_base=%d android_native_buffer=%d\n",
         HasExtension(egl_extensions, "EGL_EXT_image_dma_buf_import"),
         HasExtension(egl_extensions, "EGL_KHR_image_base"),
         HasExtension(egl_extensions, "EGL_ANDROID_image_native_buffer"));
  printf("GL extensions: OES_EGL_image=%d OES_EGL_image_external=%d\n",
         HasExtension(gl_extensions, "GL_OES_EGL_image"),
         HasExtension(gl_extensions, "GL_OES_EGL_image_external"));
  printf("entry points: eglCreateImageKHR=%d eglDestroyImageKHR=%d "
         "glEGLImageTargetTexture2DOES=%d\n",
         get_proc_address("eglCreateImageKHR") != nullptr,
         get_proc_address("eglDestroyImageKHR") != nullptr,
         get_proc_address("glEGLImageTargetTexture2DOES") != nullptr);

  AHardwareBuffer_Desc buffer_desc{};
  buffer_desc.width = 64;
  buffer_desc.height = 64;
  buffer_desc.layers = 1;
  buffer_desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  buffer_desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                      AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
                      AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                      AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
  AHardwareBuffer* hardware_buffer = nullptr;
  const int allocation_result =
      AHardwareBuffer_allocate(&buffer_desc, &hardware_buffer);
  printf("AHardwareBuffer allocation: result=%d buffer=%p\n",
         allocation_result, hardware_buffer);
  if (hardware_buffer) {
    AHardwareBuffer_Desc actual_desc{};
    AHardwareBuffer_describe(hardware_buffer, &actual_desc);
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(hardware_buffer);
    printf("AHardwareBuffer: %ux%u stride=%u format=%u usage=%#llx fds=%d ints=%d",
           actual_desc.width, actual_desc.height, actual_desc.stride,
           actual_desc.format,
           static_cast<unsigned long long>(actual_desc.usage),
           handle ? handle->numFds : -1, handle ? handle->numInts : -1);
    if (handle) {
      for (int i = 0; i < handle->numFds + handle->numInts; ++i)
        printf(" data[%d]=%d", i, handle->data[i]);
    }
    printf("\n");

    auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        get_proc_address("eglCreateImageKHR"));
    auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        get_proc_address("eglDestroyImageKHR"));
    auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        get_proc_address("glEGLImageTargetTexture2DOES"));
    printf("native-buffer entry points: create=%d target=%d\n",
           create_image != nullptr, image_target != nullptr);
    if (create_image && destroy_image && image_target) {
      EGLClientBuffer client_buffer = reinterpret_cast<EGLClientBuffer>(
          android::AHardwareBuffer_to_ANativeWindowBuffer(hardware_buffer));
      const EGLint image_attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                                         EGL_NONE};
      EGLImageKHR image = create_image(display, EGL_NO_CONTEXT,
                                       EGL_NATIVE_BUFFER_ANDROID, client_buffer,
                                       image_attributes);
      printf("native-buffer EGLImage: client=%p image=%p error=%#x\n",
             client_buffer, image, get_error());
      if (image != EGL_NO_IMAGE_KHR) {
        GLuint texture = 0;
        GLuint framebuffer = 0;
        gen_textures(1, &texture);
        bind_texture(GL_TEXTURE_2D, texture);
        image_target(GL_TEXTURE_2D, image);
        gen_framebuffers(1, &framebuffer);
        bind_framebuffer(GL_FRAMEBUFFER, framebuffer);
        framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, 0);
        const GLenum framebuffer_status = check_framebuffer_status(GL_FRAMEBUFFER);
        viewport(0, 0, 64, 64);
        clear_color(0.75f, 0.25f, 0.5f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        finish();
        printf("native-buffer framebuffer: status=%#x gl_error=%#x\n",
               framebuffer_status, gl_get_error());
        delete_framebuffers(1, &framebuffer);
        delete_textures(1, &texture);
        destroy_image(display, image);
      }
    }
    AHardwareBuffer_release(hardware_buffer);
  }
  printf("EGL_EXTENSIONS=%s\n", egl_extensions ? egl_extensions : "(null)");
  printf("GL_EXTENSIONS=%s\n", gl_extensions ? gl_extensions : "(null)");

  make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  destroy_context(display, context);
  destroy_surface(display, surface);
  terminate(display);
  dlclose(gles_library);
  dlclose(egl_library);
  return 0;
}
