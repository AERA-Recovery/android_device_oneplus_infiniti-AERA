// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <inttypes.h>
#include <libqrtr.h>
#include <logging.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define DMS_SERVICE 2
#define DMS_VERSION 1
#define DMS_GET_MAC 0x005c

struct mac_request { int32_t device; };
struct mac_response {
    struct qmi_response_type_v01 result;
    uint8_t valid;
    uint32_t length;
    uint8_t mac[8];
};

static struct qmi_elem_info request_elements[] = {
    { QMI_SIGNED_4_BYTE_ENUM, 1, sizeof(int32_t), NO_ARRAY, 1,
      offsetof(struct mac_request, device), NULL },
    { .data_type = QMI_EOTI },
};
static struct qmi_elem_info response_elements[] = {
    { QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 2,
      offsetof(struct mac_response, result), qmi_response_type_v01_ei },
    { QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x10,
      offsetof(struct mac_response, valid), NULL },
    { QMI_DATA_LEN, 1, sizeof(uint8_t), NO_ARRAY, 0x10,
      offsetof(struct mac_response, length), NULL },
    { QMI_UNSIGNED_1_BYTE, 8, sizeof(uint8_t), VAR_LEN_ARRAY, 0x10,
      offsetof(struct mac_response, mac), NULL },
    { .data_type = QMI_EOTI },
};

static volatile sig_atomic_t stopping;

static void stop_service(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_mac(const char *text, uint8_t mac[6]) {
    if (strlen(text) != 12) return -1;
    unsigned int nonzero = 0;
    for (size_t i = 0; i < 6; ++i) {
        int high = hex_digit((unsigned char)text[i * 2]);
        int low = hex_digit((unsigned char)text[i * 2 + 1]);
        if (high < 0 || low < 0) return -1;
        mac[i] = (uint8_t)((high << 4) | low);
        nonzero |= mac[i];
    }
    return nonzero && !(mac[0] & 1) ? 0 : -1;
}

static int read_mac_file(uint8_t mac[6]) {
    const char *paths[] = {
        "/vendor/firmware/wlan/qca_cld/peach_v2/wlan_mac.bin",
        "/vendor/firmware/wlan/qca_cld/wlan_mac.bin",
        "/system/etc/firmware/wlan/qca_cld/peach_v2/wlan_mac.bin",
        "/system/etc/firmware/wlan/qca_cld/wlan_mac.bin",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *file = fopen(paths[i], "re");
        if (!file) continue;
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            const char *prefix = "Intf0MacAddress=";
            if (strncmp(line, prefix, strlen(prefix)) != 0) continue;
            char *value = line + strlen(prefix);
            value[strcspn(value, "\r\n")] = '\0';
            if (parse_mac(value, mac) == 0) {
                fclose(file);
                return 0;
            }
        }
        fclose(file);
    }
    return -1;
}

static void derive_mac(const char *serial, uint8_t mac[6]) {
    // Match AERA's recovery-only MAC namespace when its RAM file is not ready.
    uint64_t hash = UINT64_C(1469598103934665603);
    const char *parts[] = { "AERA-WLAN-v1:", serial };
    for (size_t i = 0; i < 2; ++i) {
        for (const unsigned char *c = (const unsigned char *)parts[i]; *c; ++c) {
            hash ^= *c;
            hash *= UINT64_C(1099511628211);
        }
    }
    mac[0] = 2;
    for (size_t i = 1; i < 6; ++i) mac[i] = (uint8_t)(hash >> ((i - 1) * 8));
}

static void encode_dms_mac(uint8_t response_mac[6], const uint8_t mac[6]) {
    // Infiniti CNSS copies mac_address[5 - i] to dms.mac[i] in its DMS handler.
    // Compensate only here; the MAC file and serial-derived identity stay intact.
    for (size_t i = 0; i < 6; ++i) response_mac[i] = mac[5 - i];
}

static int respond(int socket_fd, struct qrtr_packet *packet,
                   const uint8_t fallback[6]) {
    unsigned int message;
    if (packet->data_len < 7 || qmi_decode_header(packet, &message) < 0) return 0;
    // This is a WLAN MAC provider, not a modem-management service.
    if (message != DMS_GET_MAC) return 0;
    struct mac_request request = { .device = -1 };
    unsigned int transaction;
    if (qmi_decode_message(&request, &transaction, packet, QMI_REQUEST,
                           DMS_GET_MAC, request_elements) < 0) return 0;
    struct mac_response response = {0};
    uint8_t interface_mac[6];
    if (request.device != 0) {
        response.result.result = QMI_RESULT_FAILURE_V01;
        response.result.error = QMI_ERR_NOT_SUPPORTED_V01;
    } else {
        response.valid = 1;
        response.length = 6;
        if (read_mac_file(interface_mac) != 0) memcpy(interface_mac, fallback, 6);
        encode_dms_mac(response.mac, interface_mac);
    }
    DEFINE_QRTR_PACKET(reply, 64);
    ssize_t length = qmi_encode_message(&reply, QMI_RESPONSE, DMS_GET_MAC,
                                        transaction, &response, response_elements);
    if (length < 0) return -1;
    int result = qrtr_sendto(socket_fd, packet->node, packet->port, reply.data,
                             (unsigned int)length);
    if (result >= 0 && response.valid)
        fprintf(stderr, "AERA WLAN MAC: interface %02x:%02x:%02x:%02x:%02x:%02x; "
                "DMS reply %02x:%02x:%02x:%02x:%02x:%02x\n",
                interface_mac[0], interface_mac[1], interface_mac[2],
                interface_mac[3], interface_mac[4], interface_mac[5],
                response.mac[0], response.mac[1], response.mac[2],
                response.mac[3], response.mac[4], response.mac[5]);
    return result;
}

#ifndef AERA_WLAN_MAC_TEST
int main(void) {
#ifdef __ANDROID__
    char device[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.product.device", device);
    if (strcmp(device, "infiniti") != 0) return 1;
    char serial[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.serialno", serial);
    if (!serial[0]) __system_property_get("ro.boot.serialno", serial);
#else
    const char *serial = getenv("AERA_TEST_SERIAL");
    if (!serial) return 1;
#endif
    uint8_t fallback[6];
    if (read_mac_file(fallback) != 0) {
        if (!serial[0]) return 1;
        derive_mac(serial, fallback);
    }
    qlog_setup("aera-wlan-mac", false);
    int socket_fd = qrtr_open(0);
    if (socket_fd < 0) return 1;
    if (qrtr_new_lookup(socket_fd, DMS_SERVICE, DMS_VERSION, 0) < 0) {
        qrtr_close(socket_fd);
        return 1;
    }
    // Never replace a real DMS provider already advertised by the platform.
    bool real_provider = false;
    while (qrtr_poll(socket_fd, 100) > 0) {
        char buffer[256];
        struct sockaddr_qrtr peer;
        socklen_t peer_size = sizeof(peer);
        ssize_t length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&peer, &peer_size);
        if (length < 0) break;
        struct qrtr_packet packet = {0};
        if (qrtr_decode(&packet, buffer, (size_t)length, &peer) < 0) continue;
        if (packet.type == QRTR_TYPE_NEW_SERVER && packet.service == DMS_SERVICE &&
            packet.version == DMS_VERSION && packet.port)
            real_provider = true;
        if (packet.type == QRTR_TYPE_NEW_SERVER && !packet.port) break;
    }
    if (real_provider || qrtr_new_server(socket_fd, DMS_SERVICE, DMS_VERSION, 0) < 0) {
        qrtr_close(socket_fd);
        return real_provider ? 0 : 1;
    }
    struct sockaddr_qrtr local;
    socklen_t local_size = sizeof(local);
    if (getsockname(socket_fd, (struct sockaddr *)&local, &local_size) < 0) {
        qrtr_close(socket_fd);
        return 1;
    }
    signal(SIGTERM, stop_service);
    signal(SIGINT, stop_service);
    fprintf(stderr, "AERA WLAN MAC: provider ready on %u:%u\n", local.sq_node, local.sq_port);
    while (!stopping) {
        if (qrtr_poll(socket_fd, 500) <= 0) continue;
        char buffer[256];
        struct sockaddr_qrtr peer;
        socklen_t peer_size = sizeof(peer);
        ssize_t length = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                  (struct sockaddr *)&peer, &peer_size);
        if (length < 0) continue;
        struct qrtr_packet packet = {0};
        if (qrtr_decode(&packet, buffer, (size_t)length, &peer) < 0) continue;
        if (packet.type == QRTR_TYPE_NEW_SERVER && packet.service == DMS_SERVICE &&
            packet.version == DMS_VERSION && packet.port &&
            (packet.node != local.sq_node || packet.port != local.sq_port)) {
            fprintf(stderr, "AERA WLAN MAC: real provider appeared on %u:%u\n",
                    packet.node, packet.port);
            break;
        }
        if (packet.type == QRTR_TYPE_DATA) respond(socket_fd, &packet, fallback);
    }
    qrtr_remove_server(socket_fd, DMS_SERVICE, DMS_VERSION, 0);
    qrtr_close(socket_fd);
    fprintf(stderr, "AERA WLAN MAC: provider stopped\n");
    return 0;
}
#endif
