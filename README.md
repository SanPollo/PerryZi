# PerryZi

## About

PerryZi is the official ESP8266 firmware for the [PerryFi](https://github.com/SanPollo/PerryFi2).

It is a version of the [Zimodem repository](https://github.com/bozimmerman/Zimodem), forked on 21st July 2026 (pre-4.0.3), and customised so it removes any Commodore-specific, and ESP32-specific modes so it can be used with the Amstrad PCW. It also maintains NIST compatibility with RetroWifiModem.

This firmware replaces the [RetroWiFiModem-based firmware](https://github.com/SanPollo/PerryFiFW) for the PerryFi, which is now considered obsolete.

PerryZi is licensed under the Apache License 2.0. Please see the [LICENSE](LICENSE) file for more information.

<br>

## Usage

* [Uploading the Firmware](https://github.com/SanPollo/PerryZi/wiki/Uploading-the-Firmware)
* [Command Reference](https://github.com/SanPollo/PerryZi/wiki/Command-Reference)

**NB:** After uploading the firmware to the ESP8266, the following command to configures the wifi: `AT+CONFIG`