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

static bool snifNaptInit();
static void snifStartOutput();
static int snifTxFree();
static void snifTxPut(uint8_t c);
static void snifTxDrain();
static int snifRead(uint8_t *buf, int max);
static void snifLogf(PGM_P format, ...);

#endif /* INCLUDE_SLIP || INCLUDE_PPP */
#endif /* PERRYZI_SERIALNETIF_H */
