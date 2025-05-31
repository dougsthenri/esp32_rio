/*
@file modbus_params.h
@brief Modbus parameter definitions and register mapping for the Modbus TCP Slave.

This file defines Modbus register addresses, data structures for coils and discrete inputs,
and utility macros for accessing and manipulating Modbus parameters with critical section protection.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#ifndef MODBUS_PARAMS_H
#define MODBUS_PARAMS_H

#define MB_REG_DISCRETE_INPUT_START 0x0000
#define MB_REG_COILS_START          0x0000

#define OE_COIL_ADDR 31 //Coil for enabling/disabling outputs

#define MB_IS_COIL_ON(address) \
    ({ \
        uint16_t addr = (address); \
        bool result = false; \
        portENTER_CRITICAL(&param_lock); \
        if (addr < 16) { \
            result = (coil_reg_params.coils_bank0 & (1U << addr)) != 0; \
        } else if (addr >= 16 && addr < 32) { \
            result = (coil_reg_params.coils_bank1 & (1U << (addr - 16))) != 0; \
        } \
        portEXIT_CRITICAL(&param_lock); \
        result; /* Return the result of the expression */ \
    })

#define MB_TURN_COIL_ON(address) \
    do { \
        uint16_t addr = (address); \
        portENTER_CRITICAL(&param_lock); \
        if (addr < 16) { \
            coil_reg_params.coils_bank0 |= (1U << addr); \
        } else if (addr >= 16 && addr < 32) { \
            coil_reg_params.coils_bank1 |= (1U << (addr - 16)); \
        } \
        portEXIT_CRITICAL(&param_lock); \
    } while(0)

#define MB_TURN_COIL_OFF(address) \
    do { \
        uint16_t addr = (address); \
        portENTER_CRITICAL(&param_lock); \
        if (addr < 16) { \
            coil_reg_params.coils_bank0 &= ~(1ULL << addr); \
        } else if (addr >= 16 && addr < 32) { \
            coil_reg_params.coils_bank1 &= ~(1ULL << (addr - 16)); \
        } \
        portEXIT_CRITICAL(&param_lock); \
    } while(0)

/*
 Modbus parameters declaring modbus address space for each modbus register type (coils, discrete inputs, holding registers, input registers)
 
 Coils bank 0:
 Address    Assignment
 0          DQ00
 1          DQ01
 2          DQ02
 3          DQ03
 4          DQ04
 5          DQ05
 6          DQ06
 7          DQ07
 8          DQ08
 9          DQ09
 10         (Reserved)
 11         (Reserved)
 12         (Reserved)
 13         (Reserved)
 14         (Reserved)
 15         (Reserved)
 
 Coils bank 1:
 Address    Assignment
 16         DQ10
 17         DQ11
 18         DQ12
 19         DQ13
 20         DQ14
 21         DQ15
 22         DQ16
 23         DQ17
 24         DQ18
 25         DQ19
 26         (Reserved)
 27         (Reserved)
 28         (Reserved)
 29         (Reserved)
 30         (Reserved)
 31         Output Enable
 
 Discrete Inputs:
 Address    Assignment
 0          DI0
 1          DI1
 2          DI2
 3          DI3
 4          DI4
 5          DI5
 6          DI6
 7          DI7
 8          DI8
 9          DI9
 10         (Reserved)
 11         (Reserved)
 12         (Reserved)
 13         (Reserved)
 14         (Reserved)
 15         (Reserved)
*/

typedef struct {
    uint16_t coils_bank0;
    uint16_t coils_bank1;
} coil_reg_params_t;

typedef struct {
    uint16_t discrete_inputs;
} discrete_reg_params_t;

#endif //MODBUS_PARAMS_H
