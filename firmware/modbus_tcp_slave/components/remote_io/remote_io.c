/*
@file remote_io.c
@brief Implementation for the remote I/O component.

This file provides functions for configuring and managing GPIOs on the ESP32,
including digital inputs, digital outputs, a status LED, and an output enable (OE) button.
It handles GPIO interrupts, debouncing, and Morse code blinking for status indication.

@copyright 2025 Douglas Almeida

This file is part of the ESP32 Remote IO Modbus TCP Slave project.
It is subject to the terms of the MIT License, which can be found in the LICENSE file.

SPDX-License-Identifier: MIT
*/

#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include "remote_io.h"

#define STATUS_LED      43  //IO43 (TXD0)
#define OE_TOGGLE_BTN   3   //IO3

// Morse code timings (in milliseconds)
#define MORSE_DOT_DURATION_MS       250
#define MORSE_DASH_DURATION_MS      (3 * MORSE_DOT_DURATION_MS)
#define MORSE_ELEMENT_PAUSE_MS      MORSE_DOT_DURATION_MS
#define MORSE_LETTER_PAUSE_MS       (3 * MORSE_DOT_DURATION_MS)
#define MORSE_WORD_PAUSE_MS         (7 * MORSE_DOT_DURATION_MS)

static void io_task(void *);
static void io_isr_handler(void *);
static void debounce_timer_callback(TimerHandle_t);
static void morse_blinker_task(void *);

static const char *TAG = "ESP32_RIO_IO";

static const gpio_num_t DI[ESP32_RIO_NUM_IO_CHANNELS]   = {4, 5, 6, 7, 15, 16, 17, 9, 8, 18};
static const gpio_num_t DQ0[ESP32_RIO_NUM_IO_CHANNELS]  = {10, 12, 14, 47, 39, 40, 41, 42, 2, 1};
static const gpio_num_t DQ1[ESP32_RIO_NUM_IO_CHANNELS]  = {46, 11, 13, 21, 48, 45, 35, 36, 37, 38};

#define DEBOUNCE_TIME_MS 250
static TimerHandle_t s_debounce_timer = NULL;
static bool s_button_pressed = false;

static oe_button_toggle_cb_t s_oe_button_toggle_callback = NULL;
static di_level_change_cb_t s_di_level_change_callback = NULL;

static QueueHandle_t s_io_event_queue = NULL;
static TaskHandle_t s_io_task_handle = NULL;


/*
 Configure GPIO for esp32_rio board
*/
void esp32_rio_configure_gpio(void) {
    // Configure input for outputs enable/disable button
    gpio_config_t di_cfg = {
        .pin_bit_mask = 1ULL << OE_TOGGLE_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE //Interrupt on falling edge (button press)
    };
    gpio_config(&di_cfg);
    // Configure DI pins as input
    di_cfg.intr_type = GPIO_INTR_ANYEDGE; //Interrupt on change
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; ++i) {
        di_cfg.pin_bit_mask = 1ULL << DI[i];
        gpio_config(&di_cfg);
    }

    // Configure status led
    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out_cfg);
    gpio_set_level(STATUS_LED, 0); //LED off, outputs disabled by default
    // Configure DQ0x, DQ1x pins as outputs
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; ++i) {
        out_cfg.pin_bit_mask = (1ULL << DQ0[i]) | (1ULL << DQ1[i]);
        gpio_config(&out_cfg);

        gpio_set_level(DQ0[i], 0);
        gpio_set_level(DQ1[i], 0);
    }
}


/*
 Initialize I/O services
*/
esp_err_t esp32_rio_io_services_init(oe_button_toggle_cb_t oe_button_toggle_callback,
                                     di_level_change_cb_t di_level_change_callback) {
    // GPIO event queue
    s_io_event_queue = xQueueCreate(10, sizeof(uint32_t)); //Hold up to 10 GPIO numbers
    if (s_io_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO event queue.");
        return ESP_FAIL;
    }

    // Install the GPIO ISR service
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0),
                        TAG,
                        "gpio_install_isr_service fail.");
    
    // Hook the GPIO interrupt handlers
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(OE_TOGGLE_BTN, io_isr_handler, (void*)OE_TOGGLE_BTN),
                        TAG,
                        "gpio_isr_handler_add fail.");
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; ++i) {
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(DI[i], io_isr_handler, (void*)DI[i]),
                            TAG,
                            "gpio_isr_handler_add fail.");
    }
    
    // Button debounce timer
    s_debounce_timer = xTimerCreate("DebounceTimer", pdMS_TO_TICKS(DEBOUNCE_TIME_MS), pdFALSE, (void *)0, debounce_timer_callback);
    if (s_debounce_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create debounce timer.");
        vQueueDelete(s_io_event_queue);
        return ESP_FAIL;
    }
    
    // Register callbacks
    s_oe_button_toggle_callback = oe_button_toggle_callback;
    s_di_level_change_callback = di_level_change_callback;

    // GPIO task
    BaseType_t ret_task_create = xTaskCreate(io_task, "io_task", 4096, NULL, 10, &s_io_task_handle);
    if (ret_task_create != pdPASS) {
        ESP_LOGE(TAG, "Failed to create io_task: %d", ret_task_create);
        vQueueDelete(s_io_event_queue);
        xTimerDelete(s_debounce_timer, 0);
        return ESP_FAIL;
    }
    
    return ESP_OK;
}


/*
 Destroy I/O services
*/
esp_err_t esp32_rio_io_services_deinit(void){
    s_oe_button_toggle_callback = NULL;
    s_di_level_change_callback = NULL;
    
    ESP_RETURN_ON_ERROR(gpio_isr_handler_remove(OE_TOGGLE_BTN),
                        TAG,
                        "gpio_isr_handler_remove fail.");
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; ++i) {
        ESP_RETURN_ON_ERROR(gpio_isr_handler_remove(DI[i]),
                            TAG,
                            "gpio_isr_handler_remove fail.");
    }
    
    gpio_uninstall_isr_service();
    
    if (s_debounce_timer != NULL) {
        if (xTimerIsTimerActive(s_debounce_timer) != pdFALSE) {
            xTimerStop(s_debounce_timer, portMAX_DELAY);
        }
        xTimerDelete(s_debounce_timer, portMAX_DELAY);
        s_debounce_timer = NULL;
    }
    
    if (s_io_task_handle != NULL) {
        vTaskDelete(s_io_task_handle);
        s_io_task_handle = NULL;
    }
    
    if (s_io_event_queue != NULL) {
        vQueueDelete(s_io_event_queue);
        s_io_event_queue = NULL;
    }
    
    return ESP_OK;
}


/*
 Query if digital input of given number is on
*/
bool esp32_rio_is_input_on(unsigned int input_number) {
    return gpio_get_level(DI[input_number]) != 0;
}


/*
 Disable all digital outputs
*/
void esp32_rio_disable_outputs(void) {
    for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; i++) {
        gpio_set_level(DQ0[i], 0);
        gpio_set_level(DQ1[i], 0);
    }
}


/*
 Turn status LED on/off
*/

void esp32_rio_turn_status_led_on(void) {
    gpio_set_level(STATUS_LED, 1);
}


void esp32_rio_turn_status_led_off(void) {
    gpio_set_level(STATUS_LED, 0);
}


/*
 Turn on/off a given digital output of a given output bank
*/


void esp32_rio_turn_output_on(unsigned int bank_number, unsigned int output_number) {
    if (bank_number == 0) {
        gpio_set_level(DQ0[output_number], 1U);
    } else if (bank_number == 1U) {
        gpio_set_level(DQ1[output_number], 1U);
    }
}


void esp32_rio_turn_output_off(unsigned int bank_number, unsigned int output_number) {
    if (bank_number == 0) {
        gpio_set_level(DQ0[output_number], 0);
    } else if (bank_number == 1U) {
        gpio_set_level(DQ1[output_number], 0);
    }
}


/*
 Start Morse blinking for "W".
 The task is meant to run indefinitely
*/
void esp32_rio_start_morse_blinker(void) {
    BaseType_t ret_task_create = xTaskCreate(morse_blinker_task, "morse_blinker", 2048, NULL, 5, NULL);
    if (ret_task_create != pdPASS) {
        ESP_LOGE(TAG, "Failed to create morse_blinker_task: %d", ret_task_create);
    }
}


static void IRAM_ATTR io_isr_handler(void *arg) {
    uint32_t gpio_num = (uint32_t)arg; //Get the GPIO number from the argument
    if (gpio_num == OE_TOGGLE_BTN) {
        s_button_pressed = true;
        xTimerStartFromISR(s_debounce_timer, NULL);
    } else {
        xQueueSendFromISR(s_io_event_queue, &gpio_num, NULL); //Send input pin number to the queue
    }
}


static void io_task(void *pvArg) {
    uint32_t io_num;
    while (1) {
        if (xQueueReceive(s_io_event_queue, &io_num, portMAX_DELAY)) {
            // A digital input pin (DIx) changed state
            ESP_LOGI(TAG, "GPIO %d was interrupted.", (int)io_num);
            
            for (int i = 0; i < ESP32_RIO_NUM_IO_CHANNELS; i++) {
                if (io_num == DI[i]) {
                    int level = gpio_get_level(DI[i]);
                    ESP_LOGI(TAG, "DI%d changed to %s.", i, level ? "HIGH" : "LOW");
                    
                    // Notify main task
                    if (s_di_level_change_callback) {
                        s_di_level_change_callback(i);
                    }
                    
                    break; //No need to check other pins
                }
            }
        }
    }
}


static void debounce_timer_callback(TimerHandle_t xTimer) {
    if (s_button_pressed) {
        // OE toggle button pressed
        s_button_pressed = false;
        ESP_LOGI(TAG, "OE Button (IO%d) pressed (debounced).", OE_TOGGLE_BTN);
        
        // Notify main task
        if (s_oe_button_toggle_callback) {
            s_oe_button_toggle_callback();
        }
    }
}


static void morse_blinker_task(void *pvParameters) {
    // The status LED must be configured already
    gpio_set_level(STATUS_LED, 0); //Ensure it is off

    while (1) {
        /*
         Morse "W": .-- (Dot, Dash, Dash)
        */
        
        // Dot
        gpio_set_level(STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(MORSE_DOT_DURATION_MS));
        gpio_set_level(STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(MORSE_ELEMENT_PAUSE_MS));

        // Dash
        gpio_set_level(STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(MORSE_DASH_DURATION_MS));
        gpio_set_level(STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(MORSE_ELEMENT_PAUSE_MS));

        // Dash
        gpio_set_level(STATUS_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(MORSE_DASH_DURATION_MS));
        gpio_set_level(STATUS_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(MORSE_LETTER_PAUSE_MS)); //Pause between letters (or repetition)
    }
}
