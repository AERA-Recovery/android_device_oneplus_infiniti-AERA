/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small recovery-side host for Qualcomm's device-matched AGM implementation.
 * The proprietary implementation remains on the installed stock /vendor and
 * is loaded only on Infiniti. Plugins never receive /dev/snd or partition access.
 */
#include <android/binder_ibinder.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

static const char kTag[] = "AERAAudio";
static const char kAgmService[] =
    "/mnt/aera-stock/vendor/lib64/libagmipcservice.so";
static const char kPalService[] =
    "/mnt/aera-stock/vendor/lib64/libpalipcservice.so";

// binder_manager.h currently exposes C++-only typedef spellings in this
// branch's platform header. The exported NDK function itself has a C ABI.
AIBinder* AServiceManager_checkService(const char* instance);

static void Log(int priority, const char* message) {
  __android_log_write(priority, kTag, message);
  fprintf(stderr, "%s\n", message);
}

static void* RegisterServiceLibrary(const char* path, const char* descriptor,
                                    const char* label) {
  void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    const char* error = dlerror();
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s load failed: %s", label,
                        error ? error : "unknown dynamic-linker error");
    return NULL;
  }

  dlerror();
  void (*register_service)(void) = dlsym(library, "registerService");
  const char* symbol_error = dlerror();
  if (!register_service || symbol_error) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s symbol failed: %s", label,
                        symbol_error ? symbol_error : "registerService missing");
    dlclose(library);
    return NULL;
  }

  register_service();
  AIBinder* service = AServiceManager_checkService(descriptor);
  if (!service) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "%s registration was rejected for %s", label,
                        descriptor);
    dlclose(library);
    return NULL;
  }
  AIBinder_decStrong(service);
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s registered.", label);
  return library;
}

int main(int argc, char** argv) {
  if (argc != 2 || strcmp(argv[1], "--infiniti-stock-agm") != 0) {
    Log(ANDROID_LOG_ERROR,
        "Refusing to start without the Infiniti stock AGM mode.");
    return 64;
  }
  if (getuid() != 0 || access(kAgmService, R_OK) != 0 ||
      access(kPalService, R_OK) != 0) {
    Log(ANDROID_LOG_ERROR, "The read-only stock audio runtime is unavailable.");
    return 66;
  }

  ABinderProcess_setThreadPoolMaxThreadCount(8);
  ABinderProcess_startThreadPool();

  void* agm_library = RegisterServiceLibrary(
      kAgmService, "vendor.qti.hardware.agm.IAGM/default", "Infiniti stock AGM");
  if (!agm_library) {
    return 70;
  }
  void* pal_library = RegisterServiceLibrary(
      kPalService, "vendor.qti.hardware.pal.IPAL/default", "Infiniti stock PAL");
  if (!pal_library) {
    dlclose(agm_library);
    return 70;
  }

  Log(ANDROID_LOG_INFO, "Infiniti stock AGM/PAL audio services are ready.");
  ABinderProcess_joinThreadPool();
  Log(ANDROID_LOG_ERROR, "Audio Binder thread pool exited unexpectedly.");
  dlclose(pal_library);
  dlclose(agm_library);
  return 70;
}
