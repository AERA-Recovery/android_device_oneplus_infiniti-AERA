# Recovery WLAN MAC provider

Infiniti's CNSS driver waits up to 200 times for 50 ms to obtain its MAC
from modem DMS. MPSS is offline in recovery, so this adds roughly ten seconds
to each firmware startup before the existing MAC-file fallback is used.

This recovery-only service supplies just the WLAN MAC response over the
driver's existing QRTR/QMI interface. It reads AERA's `wlan_mac.bin`; if the
RAM file has not been prepared yet, it uses the same serial-derived
`AERA-WLAN-v1:` address as recovery. It does not write calibration, persist,
Android files, boot images or kernel memory. Cold-boot calibration and the
post-decrypt module load order remain unchanged.

The init service starts after successful decryption, with the existing
post-decrypt event also covering devices without a screen-lock credential.
A real DMS provider takes precedence. Unsupported requests are not treated
as successful modem operations. If the helper cannot start, existing driver
timeouts and MAC-file fallback remain available.

## Hardware validation

On the tested OnePlus 15, both firmware startups received the MAC immediately,
without the previous roughly ten-second DMS wait on each startup. Driver
readiness took about 11 seconds after decryption, including 8.4 seconds of
unchanged calibration. Automatic association and DHCP completed about 20
seconds after decryption.

The driver presents the supplied MAC bytes in reverse order on this device:
`0201B876FEB8` in the MAC file became `b8:fe:76:b8:01:02` on `wlan0`. The
interface address remained identical after a second recovery boot. This is
a different recovery address from the previous MAC-file fallback, so the
router may create a new DHCP lease when first using this helper. Timing and
address behavior on other firmware revisions remain to be verified.

## Dependencies

`qrtr/` contains unmodified QRTR/QMI library files from
https://github.com/linux-msm/qrtr at
`29e36ae164389580a0f8ea7a7fdb728140ae978d`.
The upstream BSD license and per-file copyright notices are preserved.

Protocol reference: OnePlusOSS
`android_kernel_modules_and_devicetree_oneplus_sm8850`, branch
`oneplus/sm8850_b_16.0.0_oneplus_15`,
`vendor/qcom/opensource/wlan/platform/cnss_utils/device_management_service_v01.*`.
