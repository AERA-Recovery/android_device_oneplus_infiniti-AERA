// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#define AERA_WLAN_MAC_TEST
#include "../aera_wlan_mac.c"
#include <assert.h>

int main(void) {
    uint8_t mac[6];
    const uint8_t expected[] = { 2, 1, 0xb8, 0x76, 0xfe, 0xb8 };
    assert(parse_mac("0201B876FEB8", mac) == 0);
    assert(memcmp(mac, expected, 6) == 0);
    assert(parse_mac("0201b876feb8", mac) == 0);
    assert(parse_mac("000000000000", mac) < 0);
    assert(parse_mac("FFFFFFFFFFFF", mac) < 0);
    assert(parse_mac("0201B876FEB", mac) < 0);
    assert(parse_mac("0201B876FEZZ", mac) < 0);
    derive_mac("3B15B600F4W00000", mac);
    assert(memcmp(mac, expected, 6) == 0);

    struct mac_request request = { .device = 0 };
    DEFINE_QRTR_PACKET(packet, 64);
    ssize_t length = qmi_encode_message(&packet, QMI_REQUEST, DMS_GET_MAC,
                                        17, &request, request_elements);
    assert(length > 0);
    packet.data_len = (size_t)length;
    struct mac_request decoded_request = { .device = -1 };
    unsigned int transaction = 0;
    assert(qmi_decode_message(&decoded_request, &transaction, &packet,
                              QMI_REQUEST, DMS_GET_MAC, request_elements) >= 0);
    assert(transaction == 17 && decoded_request.device == 0);

    struct mac_response response = { .valid = 1, .length = 6 };
    memcpy(response.mac, expected, 6);
    packet.data_len = 64;
    length = qmi_encode_message(&packet, QMI_RESPONSE, DMS_GET_MAC,
                                17, &response, response_elements);
    assert(length > 0);
    packet.data_len = (size_t)length;
    struct mac_response decoded_response = {0};
    assert(qmi_decode_message(&decoded_response, &transaction, &packet,
                              QMI_RESPONSE, DMS_GET_MAC, response_elements) >= 0);
    assert(decoded_response.valid && decoded_response.length == 6);
    assert(memcmp(decoded_response.mac, expected, 6) == 0);
    assert(decoded_response.result.result == QMI_RESULT_SUCCESS_V01);

    response.valid = 0;
    response.result.result = QMI_RESULT_FAILURE_V01;
    response.result.error = QMI_ERR_NOT_SUPPORTED_V01;
    packet.data_len = 64;
    length = qmi_encode_message(&packet, QMI_RESPONSE, DMS_GET_MAC,
                                18, &response, response_elements);
    assert(length > 0);
    packet.data_len = (size_t)length;
    memset(&decoded_response, 0, sizeof(decoded_response));
    assert(qmi_decode_message(&decoded_response, &transaction, &packet,
                              QMI_RESPONSE, DMS_GET_MAC, response_elements) >= 0);
    assert(transaction == 18 && !decoded_response.valid);
    assert(decoded_response.result.error == QMI_ERR_NOT_SUPPORTED_V01);
    puts("MAC parsing, stable identity and QMI request/response tests passed");
    return 0;
}
