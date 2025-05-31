/*
@file wifi_connect.c
@brief Implementation for the WiFi connection component.

This file handles WiFi station mode operations, including initialization,
event handling for connection/disconnection, IP address acquisition,
and persistent storage of WiFi credentials using NVS (Non-Volatile Storage).

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"

#include "wifi_connect.h"

#define ESP32_RIO_NVS_NAMESPACE "wifi_config"
#define ESP32_RIO_NVS_KEY_SSID "ssid"
#define ESP32_RIO_NVS_KEY_PASSWORD "password"

#define ESP32_RIO_WIFI_CONN_MAX_RETRY 10
#define ESP32_RIO_NETIF_DESC_STA "esp32_rio_netif_sta"

static esp_err_t esp32_rio_wifi_sta_do_connect(wifi_config_t, bool);
static esp_err_t esp32_rio_wifi_sta_do_disconnect(void);
static void esp32_rio_print_netif_ip_info(void);

static const char *TAG = "ESP32_RIO_WIFI";

static esp_netif_t *s_esp32_rio_sta_netif = NULL;
static SemaphoreHandle_t s_semph_get_ip_addrs = NULL;
static int s_retry_num = 0;
static connection_lost_cb_t s_connection_lost_callback = NULL;


/*
 Initializes WiFi service
*/
esp_err_t esp32_rio_wifi_init(connection_lost_cb_t connection_lost_callback) {
    wifi_config_t wifi_config = { 0 };
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    
    s_connection_lost_callback = connection_lost_callback;
    
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg),
                        TAG,
                        "esp_wifi_init fail.");
    
    esp_netif_config.if_desc = ESP32_RIO_NETIF_DESC_STA;
    esp_netif_config.route_prio = 128;
    s_esp32_rio_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);
    if (s_esp32_rio_sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_wifi fail.");
        return ESP_FAIL;
    }
    
    esp_wifi_set_default_wifi_sta_handlers();

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM),
                        TAG,
                        "esp_wifi_set_storage fail.");
    
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                        TAG,
                        "esp_wifi_set_mode fail.");
    
    ESP_RETURN_ON_ERROR(esp_wifi_start(),
                        TAG,
                        "esp_wifi_start fail.");
    
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -127;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
                        TAG,
                        "esp_wifi_set_config fail.");
    
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE),
                        TAG,
                        "esp_wifi_set_ps fail.");
    
    return ESP_OK;
}


/*
 De-initializes WiFi service
*/
esp_err_t esp32_rio_wifi_deinit(void) {
    s_connection_lost_callback = NULL;
    
    ESP_RETURN_ON_ERROR(esp_wifi_stop(),
                        TAG,
                        "esp_wifi_stop fail.");
    
    ESP_RETURN_ON_ERROR(esp_wifi_deinit(),
                        TAG,
                        "esp_wifi_deinit fail.");
    
    ESP_RETURN_ON_ERROR(esp_wifi_clear_default_wifi_driver_and_handlers(s_esp32_rio_sta_netif),
                        TAG,
                        "esp_wifi_clear_default_wifi_driver_and_handlers fail.");
    
    esp_netif_destroy(s_esp32_rio_sta_netif);
    s_esp32_rio_sta_netif = NULL;
    
    return ESP_OK;
}


/*
 Initial WiFi configuration check and connection attempt
*/
bool esp32_rio_connect(void) {
    wifi_config_t wifi_config = { 0 };
    esp_err_t err;

    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
    // Try to load WiFi network info from NVS
    err = esp32_rio_wifi_nv_params_load(wifi_config.sta.ssid, wifi_config.sta.password);
    if (err == ESP_OK) {
        // Connection info loaded, try to connect
        ESP_LOGI(TAG, "Attempting to connect with stored network info...");
        err = esp32_rio_wifi_sta_do_connect(wifi_config, true);
        if (err == ESP_OK) {
            // Connection successful
            ESP_LOGI(TAG, "Successfully connected to WiFi with stored network info.");
            esp32_rio_print_netif_ip_info();
            return true;
        } else {
            // Connection failed
            ESP_LOGW(TAG, "WiFi connection failed.");
        }
    } else {
        // No credentials found
        ESP_LOGW(TAG, "No WiFi network info found in NVS or error loading.");
    }
    return false;
}


/*
 Disconnects from WiFi network
*/
esp_err_t esp32_rio_disconnect(void) {
    esp32_rio_wifi_sta_do_disconnect();
    return ESP_OK;
}


/*
 Returns a pointer to the network interface created
*/
esp_netif_t *esp32_rio_get_netif(void) {
    return s_esp32_rio_sta_netif;
}


/*
 Retrieve SSID and password from NVS
*/
esp_err_t esp32_rio_wifi_nv_params_load(unsigned char *ssid, unsigned char *password) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Ensure NVS is initialized
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    err = nvs_open(ESP32_RIO_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS namespace '%s' not found or error opening: %s", ESP32_RIO_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    // Get stored SSID
    size_t ssid_len = ESP32_RIO_SSID_MAX_LENGTH;
    err = nvs_get_str(nvs_handle, ESP32_RIO_NVS_KEY_SSID, (char *)ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to read SSID from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Get stored password
    size_t password_len = ESP32_RIO_PASSWORD_MAX_LENGTH;
    err = nvs_get_str(nvs_handle, ESP32_RIO_NVS_KEY_PASSWORD, (char *)password, &password_len);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Failed to read Password from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi credentials for SSID '%s' loaded from NVS.", ssid);
    return ESP_OK;
}


/*
 Store referred SSID and password on NVS
*/
esp_err_t esp32_rio_wifi_nv_params_save(const unsigned char *ssid, const unsigned char *password) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Ensure NVS is initialized
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    err = nvs_open(ESP32_RIO_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS namespace for write: %s", esp_err_to_name(err));
        return err;
    }

    // Store SSID
    err = nvs_set_str(nvs_handle, ESP32_RIO_NVS_KEY_SSID, (const char *)ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error storing SSID to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Store Password
    err = nvs_set_str(nvs_handle, ESP32_RIO_NVS_KEY_PASSWORD, (const char *)password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error storing Password to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi credentials for SSID '%s' saved to NVS.", ssid);
    }

    nvs_close(nvs_handle);
    return err;
}


static void esp32_rio_print_netif_ip_info(void) {
    ESP_LOGI(TAG, "Connected using %s:", esp_netif_get_desc(s_esp32_rio_sta_netif));
    esp_netif_dhcp_status_t status;
    ESP_ERROR_CHECK(esp_netif_dhcpc_get_status(s_esp32_rio_sta_netif, &status));
    if (status == ESP_NETIF_DHCP_STOPPED) {
        ESP_LOGI(TAG, "- Static IP configured.");
    }
    esp_netif_ip_info_t ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_esp32_rio_sta_netif, &ip));
    ESP_LOGI(TAG, "- IP Address:\t" IPSTR, IP2STR(&ip.ip));
    ESP_LOGI(TAG, "- Subnet Mask:\t" IPSTR, IP2STR(&ip.netmask));
    ESP_LOGI(TAG, "- Gateway:\t" IPSTR, IP2STR(&ip.gw));
}


static void esp32_rio_handler_on_wifi_disconnect(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    s_retry_num++;
    if (s_retry_num > ESP32_RIO_WIFI_CONN_MAX_RETRY) {
        ESP_LOGI(TAG, "WiFi Connect failed %d times, stop reconnect.", s_retry_num);
        if (s_semph_get_ip_addrs) {
            // Let esp32_rio_wifi_sta_do_connect() return
            xSemaphoreGive(s_semph_get_ip_addrs);
        }
        esp32_rio_wifi_sta_do_disconnect();
        
        // Notify main task
        if (s_connection_lost_callback) {
            s_connection_lost_callback();
        }
        
        return;
    }
    wifi_event_sta_disconnected_t *disconn = event_data;
    if (disconn->reason == WIFI_REASON_ROAMING) {
        ESP_LOGD(TAG, "station roaming, do nothing.");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi disconnected %d, trying to reconnect...", disconn->reason);
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return;
    }
    ESP_ERROR_CHECK(err);
}


static void esp32_rio_handler_on_wifi_connect(void *esp_netif, esp_event_base_t event_base, int32_t event_id, void *event_data) {}


static void esp32_rio_handler_on_sta_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    s_retry_num = 0;
    if (s_semph_get_ip_addrs) {
        xSemaphoreGive(s_semph_get_ip_addrs);
    }
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGD(TAG, "Got IP event: Interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
}


static esp_err_t esp32_rio_wifi_sta_do_connect(wifi_config_t wifi_config, bool wait) {
    if (wait) {
        s_semph_get_ip_addrs = xSemaphoreCreateBinary();
        if (s_semph_get_ip_addrs == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &esp32_rio_handler_on_wifi_connect, s_esp32_rio_sta_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &esp32_rio_handler_on_wifi_disconnect, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &esp32_rio_handler_on_sta_got_ip, NULL));
    
    ESP_LOGI(TAG, "Connecting to '%s'...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed! ret:%x", ret);
        return ret;
    }
    if (wait) {
        ESP_LOGI(TAG, "Waiting for IP...");
        xSemaphoreTake(s_semph_get_ip_addrs, portMAX_DELAY);
        vSemaphoreDelete(s_semph_get_ip_addrs);
        s_semph_get_ip_addrs = NULL;
        if (s_retry_num > ESP32_RIO_WIFI_CONN_MAX_RETRY) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}


static esp_err_t esp32_rio_wifi_sta_do_disconnect(void) {
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &esp32_rio_handler_on_wifi_connect));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &esp32_rio_handler_on_wifi_disconnect));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &esp32_rio_handler_on_sta_got_ip));
    return esp_wifi_disconnect();
}
