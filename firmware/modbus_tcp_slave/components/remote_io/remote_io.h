/*
@file remote_io.h
@brief Header for the remote I/O component.

This file defines the public interface for configuring and interacting
with the ESP32's GPIOs for digital inputs, outputs, and an Output Enable button.
It also includes callback types for I/O events.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#ifndef REMOTE_IO_H
#define REMOTE_IO_H

#include "esp_err.h"

#define ESP32_RIO_NUM_IO_CHANNELS 10

typedef void (*oe_button_toggle_cb_t)(void);
typedef void (*di_level_change_cb_t)(unsigned int);

void esp32_rio_configure_gpio(void);
esp_err_t esp32_rio_io_services_init(oe_button_toggle_cb_t, di_level_change_cb_t);
esp_err_t esp32_rio_io_services_deinit(void);

bool esp32_rio_is_input_on(unsigned int);

void esp32_rio_turn_output_on(unsigned int, unsigned int);
void esp32_rio_turn_output_off(unsigned int, unsigned int);

void esp32_rio_disable_outputs(void);

void esp32_rio_turn_status_led_on(void);
void esp32_rio_turn_status_led_off(void);

void esp32_rio_start_morse_blinker(void);

#endif //REMOTE_IO_H
