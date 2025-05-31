/*
@file usb_console.h
@brief Header for the USB Serial/JTAG console component.

This file defines the public interface for starting and managing the
USB Serial/JTAG console, which provides a command-line interface for
user interaction, such as WiFi configuration and status checks.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#ifndef USB_CONSOLE_H
#define USB_CONSOLE_H

#include "esp_err.h"

esp_err_t esp32_rio_start_usb_console(void);

#endif //USB_CONSOLE_H
