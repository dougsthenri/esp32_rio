cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_rio_modbus_tcp_slave)

set(idf_project_app_dependencies remote_io usb_console wifi_sta)
