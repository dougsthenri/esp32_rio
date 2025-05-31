/*
@file usb_console.c
@brief Implementation for the USB Serial/JTAG console component.

This file implements the command-line interface accessible via USB Serial/JTAG.
It handles command parsing, argument processing, and execution of console commands
such as displaying WiFi status and configuring WiFi credentials.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/usb_serial_jtag.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "usb_console.h"
#include "wifi_connect.h"

#define USB_SERIAL_JTAG_BUF_SIZE 1096

#define MAX_COMMAND_LENGTH 32
#define MAX_ARG_COUNT 2
#define MAX_ARG_LENGTH 64
#define MAX_CMD_OUTPUT_LENGTH (MAX_COMMAND_LENGTH + 3 + 128) //Takes into account the header with command name

static void console_task(void *);
static void reset_console_state(void);
static void evaluate_command(void);
static void usb_console_write_str(const char *);

typedef enum {
    STATE_IDLE,
    STATE_READING_COMMAND_NAME,
    STATE_EXPECTING_ARG,
    STATE_READING_ARG,
    STATE_READING_QUOTED_ARG,
    STATE_CLOSED_QUOTED_ARG,
    STATE_ERROR
} console_cmd_parse_state_t;

static const char *TAG = "ESP32_RIO_CONSOLE";

static char s_cmd_buffer[MAX_COMMAND_LENGTH + 1];
static char s_arg_buffer[MAX_ARG_COUNT][MAX_ARG_LENGTH + 1];
static int s_cmd_char_idx = 0;
static int s_arg_char_idx = 0;
static int s_arg_count = 0; //Current number of arguments parsed (0 to MAX_ARG_COUNT)
static char *s_current_arg_buf = NULL; //Pointer to the active argument buffer
static bool s_escape_next_char = false;
static console_cmd_parse_state_t s_current_state = STATE_IDLE;


/*
 Initialize and start USB serial console task
*/
esp_err_t esp32_rio_start_usb_console(void) {
    // Configure and install USB Serial/JTAG driver for interrupt-driven reads and writes
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_config.rx_buffer_size = USB_SERIAL_JTAG_BUF_SIZE;
    usb_serial_jtag_config.tx_buffer_size = USB_SERIAL_JTAG_BUF_SIZE;
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_serial_jtag_config),
                        TAG,
                        "usb_serial_jtag_driver_install fail.");
    
    BaseType_t ret_task_create = xTaskCreate(console_task, "console_task", 4096, NULL, 10, NULL);
    if (ret_task_create != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console_task: %d", ret_task_create);
        return ESP_FAIL;
    }
    return ESP_OK;
}


static void console_task(void *pvArg) {
    uint8_t rx_char;
    
    reset_console_state(); //Initialize state
    while (1) {
        int bytes_read = usb_serial_jtag_read_bytes(&rx_char, 1, 20 / portTICK_PERIOD_MS);
        
        if (bytes_read > 0) {
            switch (s_current_state) {
                case STATE_IDLE:
                    if (isalpha((int)rx_char)) {
                        // Start parsing of a new command
                        s_cmd_buffer[s_cmd_char_idx++] = rx_char;
                        s_current_state = STATE_READING_COMMAND_NAME;
                    } else if (rx_char != '\r' && rx_char != '\n') {
                        // Non-empty invalid command name
                        usb_console_write_str("\nError: Invalid character to start command.\n");
                        s_current_state = STATE_ERROR;
                    }
                    break;

                case STATE_READING_COMMAND_NAME:
                    if (rx_char == '\r' || rx_char == '\n') {
                        s_cmd_buffer[s_cmd_char_idx] = '\0'; //Null-terminate command name
                        evaluate_command();
                        reset_console_state();
                    } else if (isblank((int)rx_char)) {
                        s_cmd_buffer[s_cmd_char_idx] = '\0'; //Null-terminate command name
                        s_current_state = STATE_EXPECTING_ARG;
                    } else if (isalnum((int)rx_char) || rx_char == '-' || rx_char == '_') {
                        if (s_cmd_char_idx < MAX_COMMAND_LENGTH) {
                            s_cmd_buffer[s_cmd_char_idx++] = rx_char;
                        } else {
                            usb_console_write_str("\nError: Command name too long.\n");
                            s_current_state = STATE_ERROR;
                        }
                    } else {
                        usb_console_write_str("\nError: Invalid character in command name.\n");
                        s_current_state = STATE_ERROR;
                    }
                    break;
                    
                case STATE_EXPECTING_ARG:
                    s_arg_char_idx = 0; //Reset index for new argument
                    if (rx_char == '\r' || rx_char == '\n') {
                        // Expected argument is missing
                        usb_console_write_str("\nError: Malformed command.\n");
                        reset_console_state();
                    } else if (isblank((int)rx_char)) {
                        usb_console_write_str("\nError: Too much spacing before command argument.\n");
                        s_current_state = STATE_ERROR;
                    } else if (rx_char == '"') {
                        if (s_arg_count < MAX_ARG_COUNT) {
                            s_current_arg_buf = s_arg_buffer[s_arg_count]; //Point to the current argument buffer
                            s_arg_count++; //Increment argument count AFTER assignment
                            s_current_state = STATE_READING_QUOTED_ARG;
                        } else {
                            usb_console_write_str("\nError: Too many arguments for a command.\n");
                            s_current_state = STATE_ERROR;
                        }
                    } else {
                        if (s_arg_count < MAX_ARG_COUNT) {
                            s_current_arg_buf = s_arg_buffer[s_arg_count]; //Point to the current argument buffer
                            s_arg_count++; //Increment argument count AFTER assignment
                            s_current_arg_buf[s_arg_char_idx++] = rx_char; //Store first character of new argument
                            s_current_state = STATE_READING_ARG;
                        } else {
                            usb_console_write_str("\nError: Too many arguments for a command.\n");
                            s_current_state = STATE_ERROR;
                        }
                    }
                    break;
                    
                case STATE_READING_ARG:
                    if (rx_char == '\r' || rx_char == '\n') {
                        s_current_arg_buf[s_arg_char_idx++] = '\0'; //Null-terminate argument
                        evaluate_command();
                        reset_console_state();
                    } else if (isblank((int)rx_char)) {
                        s_current_arg_buf[s_arg_char_idx++] = '\0'; //Null-terminate argument
                        s_current_state = STATE_EXPECTING_ARG;
                    } else {
                        if (s_arg_char_idx < MAX_ARG_LENGTH) {
                            s_current_arg_buf[s_arg_char_idx++] = rx_char;
                        } else {
                            usb_console_write_str("\nError: Command argument is too long.\n");
                            s_current_state = STATE_ERROR;
                        }
                    }
                    break;
                    
                case STATE_READING_QUOTED_ARG:
                    if (rx_char == '"') {
                        if (s_escape_next_char) {
                            s_escape_next_char = false;
                            if (s_arg_char_idx < MAX_ARG_LENGTH) {
                                s_current_arg_buf[s_arg_char_idx++] = rx_char;
                            } else {
                                usb_console_write_str("\nError: Command argument is too long.\n");
                                s_current_state = STATE_ERROR;
                            }
                        } else {
                            s_current_state = STATE_CLOSED_QUOTED_ARG;
                        }
                    } else if (rx_char == '\\') {
                        if (s_escape_next_char) {
                            s_escape_next_char = false;
                            if (s_arg_char_idx < MAX_ARG_LENGTH) {
                                s_current_arg_buf[s_arg_char_idx++] = rx_char;
                            } else {
                                usb_console_write_str("\nError: Command argument is too long.\n");
                                s_current_state = STATE_ERROR;
                            }
                        } else {
                            s_escape_next_char = true;
                        }
                    } else {
                        if (s_arg_char_idx < MAX_ARG_LENGTH) {
                            s_current_arg_buf[s_arg_char_idx++] = rx_char;
                        } else {
                            usb_console_write_str("\nError: Command argument is too long.\n");
                            s_current_state = STATE_ERROR;
                        }
                        s_escape_next_char = false; //Neutralize invalid escape sequences
                    }
                    break;
                
                case STATE_CLOSED_QUOTED_ARG:
                    if (rx_char == '\r' || rx_char == '\n') {
                        s_current_arg_buf[s_arg_char_idx++] = '\0'; //Null-terminate argument
                        evaluate_command();
                        reset_console_state();
                    } else if (isblank((int)rx_char)) {
                        s_current_arg_buf[s_arg_char_idx++] = '\0'; //Null-terminate argument
                        s_current_state = STATE_EXPECTING_ARG;
                    } else {
                        usb_console_write_str("\nError: Malformed argument in command.\n");
                        s_current_state = STATE_ERROR;
                    }
                    break;
                    
                case STATE_ERROR:
                    // Consume characters until newline to clear the bad input
                    if (rx_char == '\r' || rx_char == '\n') {
                        ESP_LOGD(TAG, "Input cleared.");
                        reset_console_state();
                    }
            }
        }
    }
}


static void reset_console_state(void) {
    memset(s_cmd_buffer, 0, sizeof(s_cmd_buffer));
    for (int i = 0; i < MAX_ARG_COUNT; i++) {
        memset(s_arg_buffer[i], 0, sizeof(s_arg_buffer[i]));
    }
    s_cmd_char_idx = 0;
    s_arg_char_idx = 0;
    s_arg_count = 0;
    s_current_arg_buf = NULL;
    s_escape_next_char = false;
    s_current_state = STATE_IDLE;
}


static void evaluate_command(void) {
    char cmd_output_buf[MAX_CMD_OUTPUT_LENGTH];
    if (strcmp(s_cmd_buffer, "help") == 0) {
        if (s_arg_count == 0) {
            snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Recognized commands:\n", s_cmd_buffer);
            usb_console_write_str(cmd_output_buf);
            
            usb_console_write_str("  help\n");
            usb_console_write_str("    List and describe all available commands.\n");
            usb_console_write_str("  wifi-status\n");
            usb_console_write_str("    Show WiFi connection information.\n");
            usb_console_write_str("  wifi-config SSID PASSWORD\n");
            usb_console_write_str("    Configure stored WiFi connection information (SSID & mandatory password),\n");
            usb_console_write_str("    rebooting afterwards.\n");
        } else {
            snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Error: Command does not take arguments.\n", s_cmd_buffer);
            usb_console_write_str(cmd_output_buf);
        }
    } else if (strcmp(s_cmd_buffer, "wifi-status") == 0) {
        if (s_arg_count == 0) {
            wifi_ap_record_t ap_info;
            esp_netif_ip_info_t ip_info;
            char ip_str[16];

            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Connected to \"%s\":\n", s_cmd_buffer, ap_info.ssid);
                usb_console_write_str(cmd_output_buf);

                esp_netif_t *netif = esp32_rio_get_netif();
                if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
                    snprintf(cmd_output_buf, sizeof(cmd_output_buf), "  IP Address: %s\n", ip_str);
                    usb_console_write_str(cmd_output_buf);

                    sprintf(ip_str, IPSTR, IP2STR(&ip_info.netmask));
                    snprintf(cmd_output_buf, sizeof(cmd_output_buf), "  Subnet Mask: %s\n", ip_str);
                    usb_console_write_str(cmd_output_buf);

                    sprintf(ip_str, IPSTR, IP2STR(&ip_info.gw));
                    snprintf(cmd_output_buf, sizeof(cmd_output_buf), "  Gateway: %s\n", ip_str);
                    usb_console_write_str(cmd_output_buf);
                } else {
                    usb_console_write_str("  IP Information: Not available.\n");
                }
            } else {
                snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Disconnected.\n", s_cmd_buffer);
                usb_console_write_str(cmd_output_buf);
            }
        } else {
            snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Error: Command does not take arguments.\n", s_cmd_buffer);
            usb_console_write_str(cmd_output_buf);
        }
    } else if (strcmp(s_cmd_buffer, "wifi-config") == 0) {
        size_t ssid_length = strlen(s_arg_buffer[0]);
        size_t password_length = strlen(s_arg_buffer[1]);
        if (s_arg_count == 2 && ssid_length > 0 && password_length > 0) {
            if (ssid_length > ESP32_RIO_SSID_MAX_LENGTH - 1) {
                snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Error: SSID length is too long.\n", s_cmd_buffer);
                usb_console_write_str(cmd_output_buf);
                return;
            }
            if (password_length > ESP32_RIO_PASSWORD_MAX_LENGTH - 1) {
                snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Error: Password length is too long.\n", s_cmd_buffer);
                usb_console_write_str(cmd_output_buf);
                return;
            }
            ESP_ERROR_CHECK(esp32_rio_wifi_nv_params_save((const unsigned char *)s_arg_buffer[0],
                                                          (const unsigned char *)s_arg_buffer[1]));
            
            snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Configuration successful. Rebooting...\n", s_cmd_buffer);
            usb_console_write_str(cmd_output_buf);
            
            vTaskDelay(pdMS_TO_TICKS(1000)); //Give some time for messages to flush
            esp_restart();
        } else {
            snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\n[%s] Error: Command requires two (non-empty) arguments. See help.\n", s_cmd_buffer);
            usb_console_write_str(cmd_output_buf);
        }
    } else {
        snprintf(cmd_output_buf, sizeof(cmd_output_buf), "\nUnrecognized command: %s\n", s_cmd_buffer);
        usb_console_write_str(cmd_output_buf);
    }
}


static void usb_console_write_str(const char *str) {
    usb_serial_jtag_write_bytes(str, strlen(str), portMAX_DELAY);
}
