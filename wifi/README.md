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

The original helper exposed a byte-order mismatch in Infiniti's observed
DMS path: `0201B876FEB8` in the MAC file became `b8:fe:76:b8:01:02` on
`wlan0`. A second device's logs confirmed that `02BDF572BFC7` became
`c7:bf:72:f5:bd:02`, which the WLAN driver rejected as multicast before
creating the interface. Passing a unicast check on one device was therefore
not sufficient validation.

The helper now reverses the six bytes only in its DMS response to compensate
for this observed behavior. The MAC-file format and serial-derived identity
are unchanged, and logs show both the intended interface MAC and response
bytes. The MAC received and provisioned by CNSS must match the MAC file,
including its locally administered unicast `02` prefix. The subsequent
interface path can derive a different address: on the tested Wi-Fi 7/MLO
connection, `wlan0` used `00:01:b8:76:fe:b8` while CNSS correctly received
`02:01:b8:76:fe:b8`. Both first-byte values are unicast; the original
multicast rejection happens at the earlier provisioning stage.

Read-only inspection of the stock `cnss2.ko` on the tested device confirmed
the precise cause: `cnss_qmi_get_dms_mac()` copies `mac_address[5 - i]` into
`dms.mac[i]` before logging and provisioning the MAC. This differs from the
plain `memcpy` in the publicly available kernel source. The compensation is
therefore local to the Infiniti helper, not the shared QMI codec or other
devices. After a rebuild, both firmware startups received the correct
`02:01:b8:76:fe:b8` address, the supplicant reached `COMPLETED`, and DHCP
provided an address and default route. The affected second device still
needs its own hardware test. The earlier timing measurements above describe
the original helper.

Host regression tests cover both reported identities, the actual response
TLV bytes, and odd/even last-byte values. Run them without an Android build:

```sh
cc -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-function \
    -Wno-sign-compare -Iwifi/qrtr/include wifi/tests/mac_test.c \
    wifi/qrtr/lib/qmi.c wifi/qrtr/lib/qrtr.c wifi/qrtr/lib/logging.c \
    -o /tmp/aera-infiniti-mac-test
/tmp/aera-infiniti-mac-test
```

## Dependencies

`qrtr/` contains unmodified QRTR/QMI library files from
https://github.com/linux-msm/qrtr at
`29e36ae164389580a0f8ea7a7fdb728140ae978d`.
The upstream BSD license and per-file copyright notices are preserved.

Protocol reference: OnePlusOSS
`android_kernel_modules_and_devicetree_oneplus_sm8850`, branch
`oneplus/sm8850_b_16.0.0_oneplus_15`,
`vendor/qcom/opensource/wlan/platform/cnss_utils/device_management_service_v01.*`.
