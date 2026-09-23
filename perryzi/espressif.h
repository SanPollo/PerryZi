/*
   PerryZi - ESP8266 Firmware for the PerryFi

   The MIT License (MIT)

   Copyright (c) 2026 Nick J. Date

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
*/

/*
   Espressif ESP-AT compatibility layer.

   PROVENANCE
   ----------
   This is an original implementation written against the published ESP-AT
   v2.3.0.0 command reference (the docs/en/AT_Command_Set tree of the
   espressif/esp-at repository, tag v2.3.0.0_esp8266). No Espressif source
   code is incorporated: the AT command core in that release is distributed
   only as a pre-compiled library (libesp8266_at_core.a), so there is no
   source to reuse. Response formats below were taken from the published
   reference, and cross-checked against the format strings held in that library.

   Nothing here is derived from the utilities this layer is meant to serve,
   either. Those were read to determine which commands are required, and how
   their replies are parsed, not for code.

   PURPOSE
   -------
   Allows software written for the stock Espressif AT firmware to drive a
   compatible board. The whole layer is gated by ESPRESSIF_CMDS; with that
   undefined the build is byte-for-byte what it would be without this file.

   TARGET
   ------
   Which command set is emitted is chosen by ESPRESSIF_VER:

     V1_7_6 - NonOS SDK v3.0.6, the last of the 1.x branch
     V2_3_0 - ESP-AT v2.3.0.0

   The differences below were taken from the two firmwares themselves -
   the command tables and response format strings held in NonOS
   lib/libat.a and in esp-at components/at/lib/libesp8266_at_core.a -
   rather than from documentation, which is silent or wrong on several
   of these points. Four behaviours differ:

     1. AT+CIPRECVDATA. v1.x builds its reply with "%s,%d:", giving
        +CIPRECVDATA,<len>:<data>. v2.x builds it with "%s:%d,", giving
        +CIPRECVDATA:<len>,<data>. Note that the order reverses.
     2. The _CUR and _DEF command suffixes exist throughout v1.x and
        were removed in v2.x.
     3. AT+CIPDOMAIN replies with a bare address in v1.x and a quoted
        one in v2.x.
     4. A failed AT+CWJAP ends with FAIL in v1.x and ERROR in v2.x.

   Everything else, including the +IPD framing and the three-line
   AT+CIPSTA? reply, is identical in both. Commands that exist only in
   v2.x are left available under V1_7_6 as a harmless superset rather
   than being artificially blocked.

   SCOPE
   -----
   Single connection only (AT+CIPMUX=0), which is what the stock ESP-01 style
   module is normally driven as. AT+CIPMUX=1 is rejected. Plaintext TCP and
   UDP only as PerryZi does not support TLS/SSL. Therefore, the "SSL"
   transmission type, and the AT+CIPSSLC* family are not supported. IPv6 is
   also absent because the ESP8266 Arduino core this builds against compiles
   lwIP with LWIP_IPV6=0.

   The Espressif socket is deliberately held separately from Zimodem's
   WiFiClientNode list to keep Zimodem's packet/header machinery (headerOut(),
   sendNextPacket()) away from it, so the two command surfaces cannot
   interfere with each another's framing.
*/

#ifndef ZHEADER_ESPRESSIF_H
#define ZHEADER_ESPRESSIF_H

#ifdef ESPRESSIF_CMDS

/* Version reported by AT+GMR.  The AT version is quoted as the ESP-AT
   release whose command set is implemented, with a PerryZi suffix so
   that anything probing the module can tell what it is really talking
   to.  'get' (and anything like it) looks for the "AT version:" prefix
   and reads to end of line.  */
#if ESPRESSIF_VER == V1_7_6
#define ESPAT_AT_VERSION "1.7.6.0-perryzi-" PERRYZI_VERSION
#else
#define ESPAT_AT_VERSION "2.3.0.0-perryzi-" PERRYZI_VERSION
#endif

/* AT+CIPSEND payload ceiling.  The stock ESP8266 firmware accepts 2048
   bytes in a single normal-mode send. */
#define ESPAT_MAX_SEND 2048

/* How long a half-finished AT+CIPSEND may stall before it is abandoned.
   Generous, because a slow host trickling bytes is legitimate; the point
   is only to guarantee the firmware cannot be wedged for ever. */
#define ESPAT_SEND_TIMEOUT_MS 10000

/* Chunk used when streaming socket data out to the host.  Kept small on
   purpose: the data is streamed rather than buffered, so no allocation
   proportional to the transfer size is ever made. */
#define ESPAT_IO_CHUNK 64

/* Largest +IPD push emitted in active receive mode. */
#define ESPAT_IPD_MAX 1460

enum EspAtLink
{
  ESPAT_LINK_NONE = 0,
  ESPAT_LINK_TCP  = 1,
  ESPAT_LINK_UDP  = 2
};

/* Entry points used by zcommand.ino.  All three are no-ops unless the
   Espressif layer is actually in use, so the hooks are cheap. */

/* Dispatch for 'AT+...'.  Called from the '+' branch of the command
   parser BEFORE that branch lower-cases its argument buffer, so the
   original case of quoted arguments survives.  Returns true when the
   command was recognised and handled, in which case *result carries the
   response to send; returns false to let the existing PerryZi handling
   run unchanged. */
static bool espAtCommand(uint8_t *vbuf, int vlen, ZResult *result);

/* Raw payload capture for AT+CIPSEND.  Called at the top of
   ZCommand::serialIncoming().  While a send is in progress this
   consumes bytes straight off the UART -- bypassing the line-oriented
   command reader, which would otherwise strip NULs, act on CR/LF and
   interpret XON/XOFF.  Returns true while it owns the input. */
static bool espAtRawCapture();

/* Socket pump.  Called from ZCommand::loop(). */
static void espAtLoop();

#endif /* ESPRESSIF_CMDS */
#endif /* ZHEADER_ESPRESSIF_H */
