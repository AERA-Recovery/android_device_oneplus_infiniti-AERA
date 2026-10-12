// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

#define AERA_WLAN_MAC_TEST
#include "../aera_wlan_mac.c"
#include <assert.h>

static void check_dms_mac(const uint8_t intended[6]) {
    struct mac_response response = { .valid = 1, .length = 6 };
    encode_dms_mac(response.mac, intended);
    DEFINE_QRTR_PACKET(packet, 64);
    ssize_t length = qmi_encode_message(&packet, QMI_RESPONSE, DMS_GET_MAC,
                                        17, &response, response_elements);
    assert(length == 24);
    // Check the wire bytes independently of the library's round-trip decoder.
    const uint8_t header_and_tlvs[] = {
        QMI_RESPONSE, 17, 0, 0x5c, 0, 17, 0,
        2, 4, 0, 0, 0, 0, 0, 0x10, 7, 0, 6,
    };
    assert(memcmp(packet.data, header_and_tlvs, sizeof(header_and_tlvs)) == 0);
    uint8_t received[6];
    const uint8_t *wire = packet.data;
    for (size_t i = 0; i < 6; ++i) {
        assert(wire[18 + i] == intended[5 - i]);
        // Match cnss_qmi_get_dms_mac's reverse copy in the stock cnss2.ko.
        received[i] = wire[18 + 5 - i];
    }
    assert(memcmp(received, intended, 6) == 0);
    assert((received[0] & 3) == 2);
}

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
    check_dms_mac(mac);

    const uint8_t affected[] = { 2, 0xbd, 0xf5, 0x72, 0xbf, 0xc7 };
    assert(parse_mac("02BDF572BFC7", mac) == 0);
    assert(memcmp(mac, affected, 6) == 0);
    derive_mac("3B15BD00G8P00000", mac);
    assert(memcmp(mac, affected, 6) == 0);
    assert(affected[5] & 1);  // The old response became multicast on this device.
    check_dms_mac(mac);
    for (unsigned int last_byte = 0; last_byte < 256; ++last_byte) {
        mac[5] = (uint8_t)last_byte;
        check_dms_mac(mac);
    }

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
    encode_dms_mac(response.mac, expected);
    packet.data_len = 64;
    length = qmi_encode_message(&packet, QMI_RESPONSE, DMS_GET_MAC,
                                17, &response, response_elements);
    assert(length > 0);
    packet.data_len = (size_t)length;
    struct mac_response decoded_response = {0};
    assert(qmi_decode_message(&decoded_response, &transaction, &packet,
                              QMI_RESPONSE, DMS_GET_MAC, response_elements) >= 0);
    assert(decoded_response.valid && decoded_response.length == 6);
    for (size_t i = 0; i < 6; ++i)
        assert(decoded_response.mac[i] == expected[5 - i]);
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
    puts("MAC parsing, both reported identities, DMS wire order, 256 last-byte values and QMI errors passed");
    return 0;
}
