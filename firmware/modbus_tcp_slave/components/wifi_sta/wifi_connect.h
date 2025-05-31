/*
@file wifi_connect.h
@brief Header for the WiFi connection component.

This file defines the public interface for initializing, connecting,
and disconnecting from WiFi networks in station mode, including
functionality for saving and loading WiFi credentials from NVS.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#ifndef WIFI_CONNECT_H
#define WIFI_CONNECT_H

#include "esp_err.h"
#include "esp_netif.h"

#define ESP32_RIO_SSID_MAX_LENGTH 32
#define ESP32_RIO_PASSWORD_MAX_LENGTH 64

typedef void (*connection_lost_cb_t)(void);

esp_err_t esp32_rio_wifi_init(connection_lost_cb_t);
esp_err_t esp32_rio_wifi_deinit(void);

bool esp32_rio_connect(void);
esp_err_t esp32_rio_disconnect(void);
esp_netif_t *esp32_rio_get_netif(void);

esp_err_t esp32_rio_wifi_nv_params_load(unsigned char *, unsigned char *);
esp_err_t esp32_rio_wifi_nv_params_save(const unsigned char *, const unsigned char *);

#endif //WIFI_CONNECT_H
