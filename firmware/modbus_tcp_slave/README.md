# ESP32 Remote I/O Modbus TCP Slave

## Project Overview

This project implements a Modbus TCP slave firmware for the ESP32 RIO board. The board's digital inputs (DI0-9) are exposed as Modbus discrete inputs and its digital outputs (DQ00-19) as Modbus coils. The output enable function is also controllable by means of a Modbus coil.

## 1. Setup, Compile, and Flash Instructions (ESP-IDF)

This project is built using the Espressif IoT Development Framework (ESP-IDF), version 5.4.1. Follow these steps to set up your development environment, compile the firmware, and flash it onto the ESP32 RIO board.

### 1.1. Prerequisites

* **ESP-IDF Installation:** If you haven't already, follow the official ESP-IDF Getting Started Guide for your operating system. This guide will walk you through installing the necessary tools, including Python, Git, and the ESP-IDF toolchain.
    * [ESP-IDF Get Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)
* **Python:** Ensure Python 3.7 or higher is installed.
* **Git:** Ensure Git is installed for cloning the repository.

### 1.2. Getting the Code

1.  **Clone the repository:**
    ```bash
    git clone [https://github.com/dougsthenri/esp32_rio.git](https://github.com/dougsthenri/esp32_rio.git)
    cd esp32_rio/firmware/modbus_tcp_slave
    ```

### 1.3. Environment Setup

Before building, you need to set up the ESP-IDF environment variables. If you followed the official guide, you should have a script to do this.

* **Linux/macOS:**
    ```bash
    . $HOME/esp/esp-idf/export.sh
    ```
    (Adjust path if your ESP-IDF installation is in a different location).
* **Windows (ESP-IDF Command Prompt):** Use the `ESP-IDF Command Prompt` shortcut installed by the ESP-IDF installer.

### 1.4. Configuration

1.  **Open the project configuration menu:**
    ```bash
    idf.py menuconfig
    ```
2.  **Navigate and configure:** Make any adjustments you deem necessary to the provided configuration.
3.  **Save and exit.**

### 1.5. Build the Project

1.  **Build the application:**
    ```bash
    idf.py build
    ```
    This command will compile all source files and generate the necessary firmware binaries.

### 1.6. Flash the Firmware

1.  **Connect the ESP32 RIO board:** Connect the ESP32 RIO board to your computer via the USB-C port (USB Serial/JTAG).
2.  **Flash the firmware:**
    ```bash
    idf.py -p YOUR_SERIAL_PORT flash
    ```
    Replace `YOUR_SERIAL_PORT` with the actual serial port of the board (e.g., `/dev/ttyUSB0` on Linux, `COMx` on Windows).
    This command will erase the flash and write the new firmware.

### 1.7. Monitor Serial Output

After flashing, you can monitor the serial output for logs and debugging information:
```bash
idf.py -p YOUR_SERIAL_PORT monitor
```
Press `Ctrl+]` to exit the monitor.

## 2. Modbus Objects

The firmware implements various Modbus objects (coils, discrete inputs) that map directly to the ESP32 RIO board's GPIOs and internal functionalities. The Modbus TCP port is **502**. The Modbus slave address is **1**.

### 2.1. Modbus Register Map

| Modbus Type     | Address Range | Assignment / Description                                                                          |
| :-------------- | :------------ | :------------------------------------------------------------------------------------------------ |
| **Coils** | `0x0000`      | `DQ00` (Digital Output 0)                                                                         |
|                 | `0x0001`      | `DQ01` (Digital Output 1)                                                                         |
|                 | ...           | ...                                                                                               |
|                 | `0x0009`      | `DQ09` (Digital Output 9)                                                                         |
|                 | `0x0010` (`16`) | `DQ10` (Digital Output 10)                                                                        |
|                 | ...           | ...                                                                                               |
|                 | `0x0019` (`25`) | `DQ19` (Digital Output 19)                                                                        |
|                 | `0x001F` (`31`) | `Output Enable (OE)`: Controls the global enable/disable state for all digital outputs. `1` = enabled, `0` = disabled. This coil also reflects the state set by the physical OE toggle button. |
| **Discrete Inputs** | `0x0000`      | `DI0` (Digital Input 0)                                                                           |
|                 | `0x0001`      | `DI1` (Digital Input 1)                                                                           |
|                 | ...           | ...                                                                                               |
|                 | `0x0009`      | `DI9` (Digital Input 9)                                                                           |

*Note: Holding Registers and Input Registers are currently not implemented in this version.*

## 3. USB Console Communication

You can interact with the board using a standard serial terminal application connected to the USB-C port. This provides a command-line interface for configuring and monitoring the Modbus slave operation.

### 3.1. How to Connect

1.  **Connect USB-C:** Plug the ESP32 RIO board into your computer using the USB-C cable.
2.  **Identify Serial Port:** The board will enumerate as a serial port. Identify its name (e.g., `/dev/ttyUSB0` on Linux, `COMx` on Windows, `/dev/cu.usbserial-xxxx` on macOS).
3.  **Open Terminal:** Use any raw serial terminal application (e.g., Arduino serial monitor, PuTTY, Termite, picocom or even `idf.py monitor`). Enable the terminal's local echo to see the commands you typed. There's no firmware support for line editing.
4.  **Configure Serial Settings:**
    * **Baud Rate:** Not strictly applicable for USB Serial, but many terminals will default to 115200. Setting it to a common speed like 115200 or 9600 usually works, as the communication is USB-native, not UART.
    * **Data Bits:** 8
    * **Stop Bits:** 1
    * **Parity:** None
    * **Flow Control:** None
5.  **Start Session:** Open the connection. You may see some log messages.

### 3.2. Console Commands

Once connected to the USB console, you can type commands and press Enter to execute them. There must be no space before or after the full command text and only a single space before each command argument. Enclose with double quotes (“) any text argument containing spaces. Double quotes and literal backslashes (\\) must be preceded by backslashes when inside quoted text.

| Command                     | Description                                                                                                                                                                                                                                                                                                                                             |
| :-------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `help`                      | Displays a list of all recognized commands and their brief descriptions.                                                                                                                                                                                                                                                                                  |
| `wifi-status`               | Shows the current WiFi connection status of the TCP Modbus slave. If connected, it will display the SSID, IP address, and other relevant network information.                                                                                                                                                                                                        |
| `wifi-config SSID PASSWORD` | Configures the WiFi network credentials (SSID and password) for the TCP Modbus slave to connect to. Both arguments are mandatory and non-empty. After a successful configuration, the device will save the credentials to NVS and reboot to connect. |

**Example Usage:**

```
wifi-config "My Wifi Network" MySecurePassword
[wifi-config] Configuration successful. Rebooting...
```
```
wifi-status
[wifi-status] Connected to "My Wifi Network":
  IP Address: 192.168.1.100
  Subnet Mask: 255.255.255.0
  Gateway: 192.168.1.1
```

---
*For any issues or contributions, please refer to the project's [GitHub repository](https://github.com/dougsthenri/esp32_rio.git).*

