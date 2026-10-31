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
   The parts of the SLIP and PPP modes that they have in common: the host
   shares the ESP8266's own Wi-Fi address, with no address translation.

   Design

   While SLIP or PPP mode runs, the host (the computer on the serial line)
   uses the ESP8266's own Wi-Fi address as its address. Nothing is
   translated in either direction:

   - From the host: every IPv4 packet whose source is that address is sent
     on to Wi-Fi exactly as it arrived (TTL and checksums untouched), with
     lwIP choosing only the Ethernet destination (ARP, and the Wi-Fi
     gateway for anything off the local network). Packets with any other
     source are dropped, since passing them on would need translation.
   - To the host: every TCP, UDP and ICMP packet that arrives for that
     address is passed to the host, ICMP errors included, so the host sees
     what any directly connected machine would. The one exception is UDP
     to port 68: the ESP8266's own DHCP client keeps its lease, so that
     the address stays the ESP8266's (and so the host's). Broadcasts and
     multicasts that lwIP accepts are passed only if S67 is 0; by default
     (S67=1) they are left to lwIP, as on a slow serial line they could
     crowd out the host's own traffic.

   Meanwhile the ESP8266 starts no TCP, UDP or ICMP traffic of its own
   apart from DHCP (the clock's NTP requests are paused; see loop() in
   perryzi.ino), so the host can use any port but UDP 68. Any connection
   PerryZi has open is closed when either mode starts (command mode's and
   the Espressif layer's); its listeners stay, but while the mode runs what
   arrives for them goes to the host. lwIP still answers ARP for the
   address, which is how the network reaches the host.

   Packets for the host are taken from lwIP through "raw" protocol control
   blocks (raw_new()) for TCP, UDP and ICMP bound to the Wi-Fi interface:
   lwIP offers each packet that is addressed to the ESP8266 to these before
   its own TCP, UDP and ICMP code, and a packet taken there is never seen
   by them (so no resets or "port unreachable" answers from the ESP8266).
   This is the earliest point the ESP8266 core's precompiled lwIP lets a
   sketch take a packet: lwIP has already checked the IPv4 header and
   reassembled fragments. A reassembled packet larger than the host takes
   is dropped.

   Packets for the host can arrive in the SDK's system context, where
   nothing may block, so they are only encoded into the serial output
   buffer (TBUF from serout.h, unused while these modes run) by the mode's
   output function, and the mode's loop() moves the buffer to the UART as
   the 128-byte FIFO allows; a packet that does not fit whole is dropped
   and TCP recovers. With RTS/CTS, no more than SNIF_CTS_INFLIGHT bytes are
   kept in the FIFO.

   The host learns the address with DHCP (or BOOTP): requests from the
   host (UDP to port 67) are answered here, on the serial line, and never
   reach Wi-Fi. The answer gives the ESP8266's address, netmask and
   gateway and its DNS servers, with a lease of SNIF_DHCP_LEASE seconds;
   when the host renews it and the ESP8266's address has changed, the
   answer is a DHCPNAK, so the host starts again and learns the new one.
   A DHCP request is a valid IPv4 packet, so it also enters SLIP mode
   automatically (S65). In PPP mode IPCP gives the host the address, and
   DHCPINFORM is answered too.

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
#include <lwip/raw.h>

#if !LWIP_RAW
	#error SLIP and PPP need the raw API of lwIP (LWIP_RAW), which every lwIP variant of the ESP8266 core has.
#endif

// Largest IP packet carried in either direction.
#define SNIF_MTU 1500

// The shared receive buffer: a whole PPP frame as received, without its
// flags (address and control, protocol, SNIF_MTU bytes, and the FCS),
// rounded up.
#define SNIF_BUF_SIZE (SNIF_MTU + 8)

// The lease given to the host by the link's DHCP server, in seconds. It is
// the ESP8266's own address, so the lease only sets how soon the host
// notices if that address changes.
#define SNIF_DHCP_LEASE 3600

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

/*
   A mode's output function: encode the IP packet p (a pbuf chain, which
   the caller keeps) into the serial output buffer as one frame. Returns
   false if it did not fit whole and was dropped. It can be called in the
   SDK's system context, so it must not block or call lwIP.
*/
typedef bool (*SnifHostOutput)(struct pbuf *p);

// Packets dropped, for the modes' log lines (and the test bench). Defined
// here rather than in serialnetif.ino, as the IDE puts ppp.ino before it.
struct SnifCounters
{
  uint32_t full;    // for the host: no room in the serial output buffer
  uint32_t big;     // for the host: larger than the host takes
  uint32_t source;  // from the host: not from the shared address
  uint32_t bad;     // from the host: not a valid IPv4 packet
  uint32_t noWifi;  // from the host: Wi-Fi not connected, or no memory
};
static SnifCounters snifCount = { 0, 0, 0, 0, 0 };

static bool snifNetStart(SnifHostOutput out);
static void snifNetStop();
static bool snifActive();
static void snifFromHost(const uint8_t *pkt, int len);
static bool snifWifiAddr(ip4_addr_t *addr);
static void snifLinkPeer(ip4_addr_t *addr);
static uint16_t snifFcsByte(uint16_t fcs, uint8_t c);
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
static void snifLogCounters();

#endif /* INCLUDE_SLIP || INCLUDE_PPP */
#endif /* PERRYZI_SERIALNETIF_H */