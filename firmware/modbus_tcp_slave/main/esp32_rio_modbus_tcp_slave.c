/*
@file esp32_rio_modbus_tcp_slave.c
@brief Main application file for the Modbus TCP Slave.

This file initializes all services (WiFi, Modbus, I/O, USB Console)
and orchestrates the Modbus TCP slave functionality, including handling
callbacks from I/O events and managing Modbus register updates.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "remote_io.h"
#include "usb_console.h"
#include "wifi_connect.h"
#include "modbus_params.h"
#include "mbcontroller.h"

#define MB_SLAVE_ADDR 1
#define MB_TCP_PORT_NUMBER 502

#define MB_PAR_INFO_GET_TOUT 10 //Timeout for getting parameter info
#define MB_READ_MASK (MB_EVENT_DISCRETE_RD | MB_EVENT_COILS_RD)
#define MB_WRITE_MASK MB_EVENT_COILS_WR
#define MB_READ_WRITE_MASK (MB_READ_MASK | MB_WRITE_MASK)

static void on_oe_button_toggle(void);
static void on_di_level_change(unsigned int);
static void on_connection_lost(void);
static void update_digital_outputs(void);
static esp_err_t init_services(void);
static esp_err_t destroy_services(void);
static void setup_reg_data(void);
static esp_err_t mb_slave_init(void);
static esp_err_t slave_destroy(void);
static void mb_slave_run(void *);

static const char *TAG = "ESP32RIO_MB_SLAVE";

static coil_reg_params_t coil_reg_params = { 0 };
static discrete_reg_params_t discrete_reg_params = { 0 };

// For concurrent access to Modbus registers
static portMUX_TYPE param_lock = portMUX_INITIALIZER_UNLOCKED;

static bool outputs_enabled = false;


static void on_oe_button_toggle(void) {
    if (outputs_enabled) {
        outputs_enabled = false;
        MB_TURN_COIL_OFF(OE_COIL_ADDR);
        esp32_rio_disable_outputs();
        esp32_rio_turn_status_led_off(); //Alert operator
    } else {
        update_digital_outputs();
        outputs_enabled = true;
        MB_TURN_COIL_ON(OE_COIL_ADDR);
        esp32_rio_turn_status_led_on(); //Alert operator
    }
    ESP_LOGI(TAG, "Digital outputs %s.", outputs_enabled ? "enabled" : "disabled");
}


static void on_di_level_change(unsigned int input_number) {
    portENTER_CRITICAL(&param_lock);
    if (esp32_rio_is_input_on(input_number)) {
        discrete_reg_params.discrete_inputs |= (1U << input_number);
    } else {
        discrete_reg_params.discrete_inputs &= ~(1U << input_number);
    }
    portEXIT_CRITICAL(&param_lock);
}


static void on_connection_lost(void) {
    esp32_rio_start_morse_blinker(); //Alert user
}


static void update_digital_outputs(void) {
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; i++) {
        // Bank 0 (coil i corresponds to output i of bank 0)
        if (MB_IS_COIL_ON(i)) {
            esp32_rio_turn_output_on(0, i);
        } else {
            esp32_rio_turn_output_off(0, i);
        }
        // Bank 1 (coil i + 16 corresponds to output i of bank 1)
        if (MB_IS_COIL_ON(i + 16)) {
            esp32_rio_turn_output_on(1, i);
        } else {
            esp32_rio_turn_output_off(1, i);
        }
    }
}


static esp_err_t init_services(void) {
    // NVS (needed for WiFi and other configuration storage)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "nvs_flash_init fail, returns(0x%x).",
                       (int)err);
    
    // TCP/IP stack
    err = esp_netif_init();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp_netif_init fail, returns(0x%x).",
                       (int)err);
    
    // Default Event Loop
    err = esp_event_loop_create_default();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp_event_loop_create_default fail, returns(0x%x).",
                       (int)err);
    
    // WiFi
    err = esp32_rio_wifi_init(on_connection_lost);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp32_rio_wifi_init fail, returns(0x%x).",
                       (int)err);
    
    // I/O
    err = esp32_rio_io_services_init(on_oe_button_toggle, on_di_level_change);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp32_rio_io_services_init fail, returns(0x%x).",
                       (int)err);
    
    return ESP_OK;
}


static esp_err_t destroy_services(void) {
    esp_err_t err = esp32_rio_wifi_deinit();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp32_rio_wifi_deinit fail, returns(0x%x).",
                       (int)err);
    
    err = esp_event_loop_delete_default();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp_event_loop_delete_default fail, returns(0x%x).",
                       (int)err);
    
    err = esp_netif_deinit();
    MB_RETURN_ON_FALSE((err == ESP_OK || err == ESP_ERR_NOT_SUPPORTED), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp_netif_deinit fail, returns(0x%x).",
                       (int)err);
    
    err = nvs_flash_deinit();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "nvs_flash_deinit fail, returns(0x%x).",
                       (int)err);
    
    err = esp32_rio_io_services_deinit();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                       TAG,
                       "esp32_rio_io_services_deinit fail, returns(0x%x).",
                       (int)err);
    
    return ESP_OK;
}


static void setup_reg_data(void) {
    // Define initial default state of coils
    coil_reg_params.coils_bank0 = 0x0000;
    coil_reg_params.coils_bank1 = 0x0000;
    
    // Probe current state of discrete inputs corresponding to digital inputs
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; i++) {
        if (esp32_rio_is_input_on(i)) {
            discrete_reg_params.discrete_inputs |= (1U << i);
        } else {
            discrete_reg_params.discrete_inputs &= ~(1U << i);
        }
    }
}


static esp_err_t mb_slave_init(void) {
    mb_communication_info_t comm_info = { 0 };
    
    mb_register_area_descriptor_t reg_area;
    void* slave_handler = NULL;
    
    // Initialization of Modbus controller
    esp_err_t err = mbc_slave_init_tcp(&slave_handler);
    MB_RETURN_ON_FALSE((err == ESP_OK && slave_handler != NULL), ESP_ERR_INVALID_STATE,
                       TAG,
                       "mb controller initialization fail.");
    
    // Setup communication parameters and start stack
    comm_info.ip_addr_type = MB_IPV4;
    comm_info.ip_mode = MB_MODE_TCP;
    comm_info.ip_port = MB_TCP_PORT_NUMBER;
    comm_info.ip_addr = NULL; //Bind to any address
    comm_info.ip_netif_ptr = (void*)esp32_rio_get_netif();
    comm_info.slave_uid = MB_SLAVE_ADDR;
    err = mbc_slave_setup((void*)&comm_info);
    MB_RETURN_ON_FALSE((err == ESP_OK),
                       ESP_ERR_INVALID_STATE,
                       TAG,
                       "mbc_slave_setup fail, returns(0x%x).",
                       (int)err);
    
    // Initialization of Coils register area
    reg_area.type = MB_PARAM_COIL;
    reg_area.start_offset = MB_REG_COILS_START;
    reg_area.address = (void*)&coil_reg_params;
    reg_area.size = sizeof(coil_reg_params);
    err = mbc_slave_set_descriptor(reg_area);
    MB_RETURN_ON_FALSE((err == ESP_OK),
                       ESP_ERR_INVALID_STATE,
                       TAG,
                       "mbc_slave_set_descriptor fail, returns(0x%x).",
                       (int)err);
    
    // Initialization of Discrete Inputs register area
    reg_area.type = MB_PARAM_DISCRETE;
    reg_area.start_offset = MB_REG_DISCRETE_INPUT_START;
    reg_area.address = (void*)&discrete_reg_params;
    reg_area.size = sizeof(discrete_reg_params);
    err = mbc_slave_set_descriptor(reg_area);
    MB_RETURN_ON_FALSE((err == ESP_OK),
                       ESP_ERR_INVALID_STATE,
                       TAG,
                       "mbc_slave_set_descriptor fail, returns(0x%x).",
                       (int)err);
    
    // Set register values to a known state
    setup_reg_data();
    
    // Start modbus controller and stack
    err = mbc_slave_start();
    MB_RETURN_ON_FALSE((err == ESP_OK),
                       ESP_ERR_INVALID_STATE,
                       TAG,
                       "mbc_slave_start fail, returns(0x%x).",
                       (int)err);
    
    vTaskDelay(5);
    ESP_LOGI(TAG, "Modbus slave stack initialized.");
    return err;
}


static esp_err_t slave_destroy(void) {
    esp_err_t err = mbc_slave_destroy();
    MB_RETURN_ON_FALSE((err == ESP_OK),
                       ESP_ERR_INVALID_STATE,
                       TAG,
                       "mbc_slave_destroy fail, returns(0x%x).",
                       (int)err);
    return err;
}


static void mb_slave_run(void *arg) {
    mb_param_info_t reg_info; //Keeps the Modbus registers access information
    
    ESP_LOGI(TAG, "Modbus slave running.");
    while (1) {
        // Check for read/write events of Modbus master for certain events
        (void)mbc_slave_check_event(MB_READ_WRITE_MASK);
        ESP_ERROR_CHECK_WITHOUT_ABORT(mbc_slave_get_param_info(&reg_info, MB_PAR_INFO_GET_TOUT));
        // Filter events and process them accordingly
        if (reg_info.type & MB_EVENT_DISCRETE_RD) {
            ESP_LOGI(TAG, "DISCRETE READ (%" PRIu32 " us), ADDR:%u, TYPE:%u, INST_ADDR:0x%" PRIx32 ", SIZE:%u",
                     reg_info.time_stamp,
                     (unsigned)reg_info.mb_offset,
                     (unsigned)reg_info.type,
                     (uint32_t)reg_info.address,
                     (unsigned)reg_info.size);
        } else if (reg_info.type & MB_EVENT_COILS_RD) {
            ESP_LOGI(TAG, "COILS READ (%" PRIu32 " us), ADDR:%u, TYPE:%u, INST_ADDR:0x%" PRIx32 ", SIZE:%u",
                     reg_info.time_stamp,
                     (unsigned)reg_info.mb_offset,
                     (unsigned)reg_info.type,
                     (uint32_t)reg_info.address,
                     (unsigned)reg_info.size);
        } else if (reg_info.type & MB_EVENT_COILS_WR) {
            ESP_LOGI(TAG, "COILS WRITE (%" PRIu32 " us), ADDR:%u, TYPE:%u, INST_ADDR:0x%" PRIx32 ", SIZE:%u",
                     reg_info.time_stamp,
                     (unsigned)reg_info.mb_offset,
                     (unsigned)reg_info.type,
                     (uint32_t)reg_info.address,
                     (unsigned)reg_info.size);
            
            bool oe_coil_on = MB_IS_COIL_ON(OE_COIL_ADDR);
            if (outputs_enabled) {
                if (!oe_coil_on) {
                    // Outputs disabled by Modbus master
                    esp32_rio_disable_outputs();
                    outputs_enabled = false;
                    esp32_rio_turn_status_led_off(); //Alert operator
                    ESP_LOGI(TAG, "Digital outputs disabled.");
                    continue;
                }
                // Update digital outputs based on corresponding coil values
                update_digital_outputs();
            } else if (oe_coil_on) {
                // Outputs enabled by Modbus master. Update digital outputs based on corresponding coil values
                update_digital_outputs();
                outputs_enabled = true;
                esp32_rio_turn_status_led_on(); //Alert operator
                ESP_LOGI(TAG, "Digital outputs enabled.");
            }
        }
    }
}


void app_main(void) {
    esp32_rio_configure_gpio();
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_ERROR_CHECK(init_services());
    ESP_ERROR_CHECK(esp32_rio_start_usb_console());
    
    bool wifi_connected = esp32_rio_connect();
    if (wifi_connected) {
        ESP_LOGI(TAG, "Initializing Modbus slave...");
        esp_err_t err = mb_slave_init();
        if (err == ESP_OK) {
            mb_slave_run(NULL);
            
            ESP_ERROR_CHECK(slave_destroy()); //Safeguard. Should not be reached
        } else {
            ESP_LOGE(TAG, "Failed to initialize Modbus slave.");
        }
        ESP_ERROR_CHECK(esp32_rio_disconnect());
        ESP_ERROR_CHECK(destroy_services());
    } else {
        ESP_LOGE(TAG, "Failed to establish WiFi connection.");
        ESP_ERROR_CHECK(destroy_services());
        // Only USB serial console and morse blinker shall remain from here on
        esp32_rio_start_morse_blinker();
    }
}
