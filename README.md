# AERA Recovery Project device tree for OnePlus 15

Device codename: `infiniti`

Platform: Qualcomm SM8850 (`canoe`)
Recovery partition limit: 100 MiB

This tree preserves the history of the original OrangeFox device tree while
carrying the OnePlus 15 integration for AERA Recovery Project R1.0.

## Hardware support

- Display and touch
- File-based encryption
- A/B flashing, backup/restore, ADB, MTP, and fastbootd
- Wi-Fi
- Haptics and flashlight
- Adreno 840 recovery rendering with matching gen80200 firmware
- Qualcomm AGM/PAL audio using the installed OP15 stock partitions
- KernelSU, KernelSU Next, and SukiSU Ultra support

The proprietary graphics and audio files in this repository were extracted
from the matching OnePlus 15 stock OTA. Do not reuse them on another platform.

## Build

```sh
cd ~/Desktop/AERA_16.0
source build/envsetup.sh
lunch twrp_infiniti-bp2a-eng
mka adbd recoveryimage
```

The output is written to `out/target/product/infiniti/`.
