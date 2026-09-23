# PerryZi

## About

PerryZi is the official ESP8266 firmware for the [PerryFi](https://github.com/SanPollo/PerryFi2). It replaces the [RetroWiFiModem-based firmware](https://github.com/SanPollo/PerryFiFW) for the PerryFi, which is now considered obsolete.

The project started off as an offshoot of the [Zimodem repository](https://github.com/bozimmerman/Zimodem), forked on 21st July 2026 (pre-4.0.3), customised to remove Commodore-specific, and ESP32-specific modes. It also implemented RetroWifiModem NIST compatibility to support ST.COM for the Amstrad PCW. The idea was to keep the rich features of Zimodem, while being lean enough to be able to add more functionality in future releases.

From release 1.2.4, PerryZi now contains the AT-commands from the default Espressif firmware (implemented from scratch), as its new goal is to be a universal ESP8266 firmware for vintage computers combining Zimodem functionality with maintaining compatibility with existing tools written for a specific platform. For example, the Agon Light, which uses the [Olimex MOD-WIFI-ESP8266 module](MOD-WIFI-ESP8266), has many network utilities that use Espressif firmware commands, and these can now be used with PerryZi alongside the SLIP and PPP network modes that Zimodem provides.

PerryZi is licensed under the Apache License 2.0, with some MIT-licensed components. Please see the [LICENSE](LICENSE) file for more information, and the [NOTICE](NOTICE) file for attributions.

<br>

## Usage

* [Uploading the Firmware](https://github.com/SanPollo/PerryZi/wiki/Uploading-the-Firmware)
* [Command Reference](https://github.com/SanPollo/PerryZi/wiki/Command-Reference)

**NB:** After uploading the firmware to the ESP8266, entering the following command in a terminal program will configure the wifi: `AT+CONFIG`

<br>

## Project PerryFi

Project PerryFi consists of several separate components which make the PerryFi 2 possible:

* [PerryFi 2](https://github.com/SanPollo/PerryFi2) - Hardware
* [PerryDART](https://github.com/SanPollo/PicoDART) - Pico Firmware
* [PerryZi](https://github.com/SanPollo/PicoZi) - ESP8266 Firmware