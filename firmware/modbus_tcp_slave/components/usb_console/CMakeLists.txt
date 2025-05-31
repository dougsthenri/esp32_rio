idf_component_register(SRCS "usb_console.c"
                       INCLUDE_DIRS "."
		       PRIV_REQUIRES esp_driver_usb_serial_jtag wifi_sta esp_wifi)