# Infiniti GPU recovery prebuilts

These proprietary ARM64 Adreno userspace libraries, kernel module, and Gen8
firmware are copied from the owner's extracted OnePlus 15 OTA at
`MIO-KITCHEN/OP15`. They stay in the Infiniti device tree because they are
specific to the SM8850/`canoe` platform and its Adreno 840.

`msm_kgsl.ko` is taken from the matching OP15 vendor_dlkm image. All KGSL hard
and soft dependencies are loaded by the stock recovery module set.
`BoardConfig.mk` requests KGSL through `TW_LOAD_VENDOR_MODULES`, using the same
dependency-aware loader as the rest of Infiniti's vendor modules.

The userspace closure includes only EGL/GLES2 and the mapper/gralloc libraries
needed by Qualcomm's Android EGL subdriver. Vulkan, SurfaceFlinger, and the rest
of the Android graphics services are intentionally excluded. The software
renderer remains the fail-safe path.

The OP15 native-window ABI is packaged unmodified because both the Adreno driver
and the HexLP service use it directly. No proprietary binary is patched.

The validation probe creates an EGL pbuffer, renders a known pixel, and reads it
back. It provides an end-to-end check of KGSL, gen80200 firmware, EGL/GLES2, and
the Adreno 840 shader compiler before hardware rendering is trusted.
