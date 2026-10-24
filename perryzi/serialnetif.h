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
   The parts of the SLIP and PPP modes that they have in common: the serial
   line as a routed lwIP network interface with NAT (NAPT) to Wi-Fi.

   Design

   The serial line is a network interface of its own. The host has a fixed
   private address on it (SNIF_PEER_ADDR), the ESP8266 is its gateway
   (SNIF_LOCAL_ADDR), and lwIP forwards packets between the serial interface
   and Wi-Fi, translating the host's address to the ESP8266's Wi-Fi address
   on the way out (NAPT, as in the core's RangeExtender-NAPT example). The
   ESP8266 keeps its own Wi-Fi address and traffic, so DHCP, DNS and NTP for
   the ESP8266 itself are not disturbed, and the host's address does not
   change when the Wi-Fi lease does.

   Packets from the host are handed to lwIP's ip_input(), which takes
   ownership of them. Packets for the host reach the interface's output
   function, possibly in the SDK's system context (a packet from Wi-Fi is
   forwarded there), where nothing may block or yield. The output functions
   therefore only encode each packet into the serial output buffer (TBUF in
   serout.h, otherwise unused while these modes run) and return; the mode's
   loop() then moves the buffer to the UART as space in its FIFO allows. A
   packet that does not fit whole in the buffer is dropped, as a router
   would, and TCP recovers.

   NAPT needs an lwIP variant "with features" (IP_FORWARD and IP_NAPT); the
   board menu default, "v2 Lower Memory", is one.

   Leaving the modes

   Both modes end with the escape sequence used everywhere else: a second
   of silence, the escape character (S2, default '+') three times, and a
   second of silence (ESCAPE_GUARD_MS). A working SLIP or PPP host cannot
   send this by accident: every frame begins and ends with a delimiter
   (0xC0 for SLIP, 0x7E for PPP), so the last byte before a pause on the
   line is never the escape character, and a packet after a pause starts
   with a delimiter or an IPv4 header (0x45 to 0x4F), never with '+'. The
   escape is not recognised if S2 is set to one of the framing bytes. SLIP
   mode then ends at once; PPP mode first closes the session with the host
   (LCP Terminate-Request). Either way the modem answers NO CARRIER.

   Entering the modes automatically

   In command mode, a complete frame from the host switches the modem to
   SLIP (S65=1) or PPP (S66=1) mode without any AT command, and the frame
   is then handled by the mode, so network software can simply start
   sending. Nothing typed on an ASCII terminal can do this:

   - SLIP: 0xC0, then a valid IPv4 packet (version 4, header checksum
     correct, total length equal to the frame's length), then 0xC0. 0xC0 is
     not an ASCII character.
   - PPP: an LCP Configure-Request, which is what a PPP host always sends
     first: 0x7E, 0xFF, 0x03, 0xC0, 0x21, code 1, ..., a correct frame
     check sequence, 0x7E. RFC 1661 requires the full address, control and
     protocol fields in LCP packets, so 0xFF and 0xC0 are always present,
     and neither is an ASCII character.

   The bytes of a possible frame are held back (not echoed, not added to
   the command line) until it is known whether they are one. If they are
   not, the bytes that could have been typed are processed as usual,
   slightly later: a '~' (0x7E) not followed by 0xFF, and the bytes after
   a 0xC0 when the IPv4 header checks fail. What remains (0xC0, and a frame
   that failed later checks, which no typing can produce) is dropped. The
   modes are only ever entered from command mode, never while a connection
   is open (so a file upload cannot trigger them), and AT+CIPSEND's data is
   not looked at.
*/

#ifndef PERRYZI_SERIALNETIF_H
#define PERRYZI_SERIALNETIF_H

#if INCLUDE_SLIP || INCLUDE_PPP

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/ip.h>
#include <lwip/ip4_addr.h>
#include <lwip/napt.h>

#if !IP_NAPT
	#error SLIP and PPP need an lwIP variant with features (IP_NAPT): choose "v2 Lower Memory" or another variant without "(no features)" under Tools > lwIP Variant, or set INCLUDE_SLIP and INCLUDE_PPP to false.
#endif

// Link addresses: the ESP8266 (gateway) and the host on the serial line.
#define SNIF_LOCAL_ADDR(a)  IP4_ADDR((a), 192, 168, 240, 1)
#define SNIF_PEER_ADDR(a)   IP4_ADDR((a), 192, 168, 240, 2)
#define SNIF_NETMASK(a)     IP4_ADDR((a), 255, 255, 255, 0)

// Largest IP packet carried in either direction.
#define SNIF_MTU 1500

// NAPT table entries (24 bytes each on the heap, allocated once, on first
// use of SLIP or PPP, and never freed: lwIP cannot release the table). The
// port map table is not used; lwIP is given its smallest size, 1.
#define SNIF_NAPT_MAX     128
#define SNIF_PORTMAP_MAX  1

// With RTS/CTS flow control, CTS is read in software, and whatever is in
// the UART FIFO is sent even after the host drops CTS. Only this many bytes
// are kept in the FIFO at once, so the host is never overrun by more.
#define SNIF_CTS_INFLIGHT 16

// The UART transmit FIFO size of the ESP8266.
#define SNIF_UART_FIFO 128

// Automatic entry: the most bytes of a possible PPP frame held back (an
// LCP Configure-Request is far smaller), how long a possible frame may
// pause before it is given up, and how long a lone '~' is held back.
#define SNIF_AUTO_PPP_MAX   256
#define SNIF_AUTO_GAP_MS    1000
#define SNIF_AUTO_TILDE_MS  200

// Bytes given back to command mode at once: at most a SLIP header (60)
// and the byte that ended it, or a '~' and the byte after it.
#define SNIF_RELEASE_MAX    64

static bool snifNaptInit();
static uint8_t *snifBuffer();
static void snifEscapeStart();
static void snifEscapeByte(uint8_t c);
static bool snifEscaped();
static bool snifAutoInput(uint8_t c);
static void snifAutoPoll();
static void snifAutoReset();
static int snifReleased();
static bool snifReleaseWaiting();
static void snifStartOutput();
static int snifTxFree();
static void snifTxPut(uint8_t c);
static void snifTxDrain();
static int snifRead(uint8_t *buf, int max);
static void snifLogf(PGM_P format, ...);

#endif /* INCLUDE_SLIP || INCLUDE_PPP */
#endif /* PERRYZI_SERIALNETIF_H */
