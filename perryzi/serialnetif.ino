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
   Shared serial and network plumbing for the SLIP and PPP modes. See
   serialnetif.h for the design.
*/

#if INCLUDE_SLIP || INCLUDE_PPP

// RTS/CTS flow control in effect for the current SLIP or PPP session.
static bool snifCtsFlow = false;

// The running mode's output function (0 while neither mode runs), and the
// raw PCBs that take packets for the host from lwIP: TCP, UDP and ICMP.
static SnifHostOutput snifOut = 0;
static struct raw_pcb *snifPcb[3] = { 0, 0, 0 };

#define SNIF_PROTO_ICMP 1
#define SNIF_PROTO_TCP  6
#define SNIF_PROTO_UDP  17
#define SNIF_DHCP_SERVER_PORT 67
#define SNIF_DHCP_CLIENT_PORT 68

/* ---- addresses ------------------------------------------------------- */

/* The Wi-Fi station interface (the core's glue names it "st"). */
static netif *snifWifi()
{
  for(struct netif *n = netif_list; n != NULL; n = n->next)
  {
    if((n->name[0] == 's') && (n->name[1] == 't'))
      return n;
  }
  return NULL;
}

/* The ESP8266's Wi-Fi address, which the host shares. False while it has
   none (not connected). */
static bool snifWifiAddr(ip4_addr_t *addr)
{
  struct netif *wifi = snifWifi();
  if((wifi == NULL) || !netif_is_up(wifi)
  || ip4_addr_isany_val(*netif_ip4_addr(wifi)))
    return false;
  ip4_addr_copy(*addr, *netif_ip4_addr(wifi));
  return true;
}

/*
   The address given as the other end of the serial line, where one is
   needed (the DHCP server identifier, the modem's address in IPCP): the
   Wi-Fi gateway, the host's real next hop. Zero if there is none, or if
   it is the shared address itself.
*/
static void snifLinkPeer(ip4_addr_t *addr)
{
  ip4_addr_set_zero(addr);
  struct netif *wifi = snifWifi();
  if(wifi == NULL)
    return;
  const ip4_addr_t *gw = netif_ip4_gw(wifi);
  if(!ip4_addr_isany_val(*gw)
  && !ip4_addr_cmp(gw, netif_ip4_addr(wifi)))
    ip4_addr_copy(*addr, *gw);
}

/* ---- the Internet checksum ------------------------------------------- */

/* The one's complement sum of len bytes (big-endian 16-bit words), added
   to sum; fold it and complement it with snifSumDone(). */
static uint32_t snifSum(const uint8_t *d, int len, uint32_t sum)
{
  int i = 0;
  for(; i + 1 < len; i += 2)
    sum += ((uint32_t)d[i] << 8) | d[i+1];
  if(i < len)
    sum += (uint32_t)d[i] << 8;
  return sum;
}

static uint16_t snifSumDone(uint32_t sum)
{
  while(sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  return (uint16_t)~sum;
}

/* ---- to the host ----------------------------------------------------- */

/*
   lwIP offers each TCP, UDP or ICMP packet that arrives on Wi-Fi for the
   ESP8266 here first (p->payload is the IPv4 header; the header has been
   checked and any fragments reassembled). Returning 1 means the packet is
   taken (and freed here); 0 leaves it to lwIP as usual. This may run in
   the SDK's system context: only the mode's output function is called.
*/
static u8_t snifRawRecv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr)
{
  (void)arg;
  (void)pcb;
  (void)addr;
  struct netif *wifi = ip_current_input_netif();
  if((snifOut == 0) || (wifi == NULL))
    return 0;
  const uint8_t *h = (const uint8_t *)p->payload;
  // Broadcasts and multicasts (anything not for the ESP8266's own
  // address) go to the host only with S67=0.
  if(!ip4_addr_cmp(ip_2_ip4(ip_current_dest_addr()), netif_ip4_addr(wifi))
  && commandMode.blockCasts)
    return 0;
  // UDP to port 68 is the ESP8266's own DHCP client's.
  if(h[9] == SNIF_PROTO_UDP)
  {
    uint8_t port[2];
    if((pbuf_copy_partial(p, port, 2, (u16_t)((h[0] & 0x0F) * 4 + 2)) == 2)
    && (port[0] == 0) && (port[1] == SNIF_DHCP_CLIENT_PORT))
      return 0;
  }
  if(p->tot_len > SNIF_MTU)
    snifCount.big++;
  else
  if(!snifOut(p))
    snifCount.full++;
  pbuf_free(p);
  return 1;
}

/*
   Start passing packets between the host and Wi-Fi, with out as the
   output function. False (nothing started) if the raw PCBs cannot be
   had: lwIP has MEMP_NUM_RAW_PCB of them (4 in the core's variants).
*/
static bool snifNetStart(SnifHostOutput out)
{
  static const uint8_t protos[3] = { SNIF_PROTO_TCP, SNIF_PROTO_UDP,
                                     SNIF_PROTO_ICMP };
  struct netif *wifi = snifWifi();
  if(wifi == NULL)
    return false;
  // The ESP8266's own connections end first: from now on, what arrives
  // for them would go to the host.
  commandMode.dropAllConnections();
  snifOut = out;
  for(int i=0; i<3; i++)
  {
    snifPcb[i] = raw_new(protos[i]);
    if(snifPcb[i] == NULL)
    {
      snifNetStop();
      return false;
    }
    raw_bind_netif(snifPcb[i], wifi);
    raw_recv(snifPcb[i], snifRawRecv, NULL);
  }
  return true;
}

/* Stop passing packets: lwIP handles everything itself again. */
static void snifNetStop()
{
  for(int i=0; i<3; i++)
  {
    if(snifPcb[i] != NULL)
      raw_remove(snifPcb[i]);
    snifPcb[i] = NULL;
  }
  snifOut = 0;
}

/* True while SLIP or PPP mode runs. */
static bool snifActive()
{
  return snifOut != 0;
}

/* Send len bytes at d (an IPv4 packet) to the host. Loop context only. */
static bool snifToHost(uint8_t *d, int len)
{
  if(snifOut == 0)
    return false;
  // The output functions read only payload, len, tot_len and next, so a
  // pbuf on the stack around the buffer will do.
  struct pbuf q;
  memset(&q, 0, sizeof(q));
  q.payload = d;
  q.len = q.tot_len = (u16_t)len;
  if(!snifOut(&q))
  {
    snifCount.full++;
    return false;
  }
  return true;
}

/* ---- DHCP for the host ----------------------------------------------- */

// Offsets in a DHCP (BOOTP) message (RFC 2131, RFC 951).
#define SNIF_BOOTP_XID     4
#define SNIF_BOOTP_FLAGS   10
#define SNIF_BOOTP_CIADDR  12
#define SNIF_BOOTP_YIADDR  16
#define SNIF_BOOTP_GIADDR  24
#define SNIF_BOOTP_CHADDR  28
#define SNIF_BOOTP_COOKIE  236
#define SNIF_BOOTP_OPTIONS 240
#define SNIF_BOOTP_MIN     300   // RFC 1542: at least 300 bytes

/* Append option t with len bytes at v; returns the new end. */
static int snifDhcpOpt(uint8_t *o, int at, uint8_t t, const void *v, int len)
{
  o[at++] = t;
  o[at++] = (uint8_t)len;
  memcpy(o + at, v, len);
  return at + len;
}

/*
   A DHCP or BOOTP request from the host (the IPv4 packet at ip, total
   bytes, UDP to port 67). It is answered here, on the serial line, and
   never sent to Wi-Fi: DHCPDISCOVER with DHCPOFFER, DHCPREQUEST with
   DHCPACK for the shared address (DHCPNAK for any other), DHCPINFORM with
   DHCPACK, and a BOOTP request with a BOOTP reply. DHCPDECLINE and
   DHCPRELEASE need no answer. Nothing is answered while Wi-Fi has no
   address; the host will ask again.
*/
static void snifDhcp(const uint8_t *ip, int total)
{
  int hl = (ip[0] & 0x0F) * 4;
  const uint8_t *m = ip + hl + 8;
  int mlen = total - hl - 8;
  ip4_addr_t own, sid, gw, mask;
  if((mlen < SNIF_BOOTP_COOKIE) || (m[0] != 1) || !snifWifiAddr(&own))
    return;
  struct netif *wifi = snifWifi();
  ip4_addr_copy(gw, *netif_ip4_gw(wifi));
  ip4_addr_copy(mask, *netif_ip4_netmask(wifi));
  snifLinkPeer(&sid);
  if(ip4_addr_isany_val(sid))
    ip4_addr_copy(sid, own);

  // The options that matter: the message type (53), the address asked
  // for (50) and the server chosen (54).
  static const uint8_t cookie[4] = { 99, 130, 83, 99 };
  bool dhcp = (mlen >= SNIF_BOOTP_OPTIONS)
            && (memcmp(m + SNIF_BOOTP_COOKIE, cookie, 4) == 0);
  int type = 0;
  const uint8_t *asked = m + SNIF_BOOTP_CIADDR;
  const uint8_t *server = NULL;
  for(int i = SNIF_BOOTP_OPTIONS; dhcp && (i < mlen); )
  {
    uint8_t t = m[i];
    if(t == 0)
    {
      i++;
      continue;
    }
    if((t == 255) || (i + 1 >= mlen) || (i + 2 + m[i+1] > mlen))
      break;
    uint8_t n = m[i+1];
    if((t == 53) && (n == 1))
      type = m[i+2];
    if((t == 50) && (n == 4))
      asked = m + i + 2;
    if((t == 54) && (n == 4))
      server = m + i + 2;
    i += 2 + n;
  }

  // DHCPDISCOVER (1), DHCPREQUEST (3), DHCPINFORM (8), or BOOTP (none).
  uint8_t reply;
  bool give = true;          // the shared address as yiaddr
  if(type == 1)
    reply = 2;               // DHCPOFFER
  else
  if(type == 3)
  {
    if((server != NULL) && (memcmp(server, &sid, 4) != 0))
      return;                // another server's (there is none here)
    if(memcmp(asked, &own, 4) == 0)
      reply = 5;             // DHCPACK
    else
    {
      reply = 6;             // DHCPNAK: the address has changed
      give = false;
    }
  }
  else
  if(type == 8)
  {
    reply = 5;               // DHCPACK, no address
    give = false;
  }
  else
  if(type == 0)
    reply = 0;               // a BOOTP reply
  else
    return;                  // DHCPDECLINE, DHCPRELEASE, or unknown

  // The reply: IPv4 header, UDP header, then the message.
  uint8_t r[20 + 8 + SNIF_BOOTP_MIN + 32];
  memset(r, 0, sizeof(r));
  uint8_t *o = r + 28;
  o[0] = 2;                                   // BOOTREPLY
  o[1] = m[1];                                // htype, hlen as asked
  o[2] = m[2];
  memcpy(o + SNIF_BOOTP_XID, m + SNIF_BOOTP_XID, 4);
  memcpy(o + SNIF_BOOTP_FLAGS, m + SNIF_BOOTP_FLAGS, 2);
  if((type == 8) || ((type == 3) && (reply == 5)))
    memcpy(o + SNIF_BOOTP_CIADDR, m + SNIF_BOOTP_CIADDR, 4);
  if(give)
    memcpy(o + SNIF_BOOTP_YIADDR, &own, 4);
  memcpy(o + SNIF_BOOTP_GIADDR, m + SNIF_BOOTP_GIADDR, 4);
  memcpy(o + SNIF_BOOTP_CHADDR, m + SNIF_BOOTP_CHADDR, 16);
  int at = SNIF_BOOTP_COOKIE;
  if(dhcp)
  {
    memcpy(o + at, cookie, 4);
    at += 4;
    if(reply != 0)
    {
      at = snifDhcpOpt(o, at, 53, &reply, 1);
      at = snifDhcpOpt(o, at, 54, &sid, 4);
    }
    if(reply != 6)
    {
      if((reply == 2) || ((reply == 5) && give))
      {
        uint8_t lease[4] = { (uint8_t)(SNIF_DHCP_LEASE >> 24),
                             (uint8_t)(SNIF_DHCP_LEASE >> 16),
                             (uint8_t)(SNIF_DHCP_LEASE >> 8),
                             (uint8_t)SNIF_DHCP_LEASE };
        at = snifDhcpOpt(o, at, 51, lease, 4);
      }
      at = snifDhcpOpt(o, at, 1, &mask, 4);
      if(!ip4_addr_isany_val(gw))
        at = snifDhcpOpt(o, at, 3, &gw, 4);
      uint8_t dns[8];
      int n = 0;
      for(int i=0; i<2; i++)
      {
        const ip_addr_t *d = dns_getserver(i);
        if((d != NULL) && !ip_addr_isany(d))
        {
          memcpy(dns + n, ip_2_ip4(d), 4);
          n += 4;
        }
      }
      if(n > 0)
        at = snifDhcpOpt(o, at, 6, dns, n);
    }
    o[at++] = 255;
  }
  int mtotal = (at < SNIF_BOOTP_MIN) ? SNIF_BOOTP_MIN : at;
  int len = 28 + mtotal;

  // To the host's address if it has one (a renewal or DHCPINFORM),
  // otherwise to everyone: the line has only the host on it.
  // (Copied out byte by byte: the ESP8266 cannot read a 32-bit word at an
  // unaligned address.)
  ip4_addr_t dst;
  memcpy(&dst, m + SNIF_BOOTP_CIADDR, 4);
  if((reply == 6) || ip4_addr_isany_val(dst))
    ip4_addr_set_u32(&dst, IPADDR_BROADCAST);
  static uint16_t ipId = 0;
  ipId++;
  r[0] = 0x45;
  r[2] = (uint8_t)(len >> 8);
  r[3] = (uint8_t)len;
  r[4] = (uint8_t)(ipId >> 8);
  r[5] = (uint8_t)ipId;
  r[8] = 64;                                  // TTL
  r[9] = SNIF_PROTO_UDP;
  memcpy(r + 12, &sid, 4);
  memcpy(r + 16, &dst, 4);
  uint16_t s = snifSumDone(snifSum(r, 20, 0));
  r[10] = (uint8_t)(s >> 8);
  r[11] = (uint8_t)s;
  uint8_t *u = r + 20;
  u[1] = SNIF_DHCP_SERVER_PORT;
  u[3] = SNIF_DHCP_CLIENT_PORT;
  u[4] = (uint8_t)((8 + mtotal) >> 8);
  u[5] = (uint8_t)(8 + mtotal);
  // The UDP checksum covers a pseudo header: both addresses, the protocol
  // and the UDP length.
  uint32_t sum = snifSum(r + 12, 8, SNIF_PROTO_UDP + 8 + mtotal);
  s = snifSumDone(snifSum(u, 8 + mtotal, sum));
  if(s == 0)
    s = 0xFFFF;
  u[6] = (uint8_t)(s >> 8);
  u[7] = (uint8_t)s;
  if(snifToHost(r, len))
  {
    static const char names[7][9] = { "BOOTP", "", "OFFER", "", "", "ACK",
                                      "NAK" };
    snifLogf(PSTR("DHCP for the host: %s %s"), names[reply],
             ip4addr_ntoa(&own));
  }
}

/* ---- from the host --------------------------------------------------- */

/*
   An IPv4 packet from the host (len bytes at pkt), from loop() only. A
   valid packet from the shared address is sent on to Wi-Fi unchanged;
   DHCP requests are answered here; anything else is dropped.
*/
static void snifFromHost(const uint8_t *pkt, int len)
{
  if((len < 20) || ((pkt[0] >> 4) != 4))
  {
    snifCount.bad++;
    return;
  }
  int hl = (pkt[0] & 0x0F) * 4;
  int total = (pkt[2] << 8) | pkt[3];
  // SLIP has no checksum of its own, so the IPv4 header's is the only
  // protection against line noise here.
  if((hl < 20) || (total < hl) || (total > len) || (total > SNIF_MTU)
  || (snifSumDone(snifSum(pkt, hl, 0)) != 0))
  {
    snifCount.bad++;
    return;
  }
  if((pkt[9] == SNIF_PROTO_UDP) && (total >= hl + 8)
  && (pkt[hl + 2] == 0) && (pkt[hl + 3] == SNIF_DHCP_SERVER_PORT))
  {
    snifDhcp(pkt, total);
    return;
  }
  ip4_addr_t own, dst;
  struct netif *wifi = snifWifi();
  if(!snifWifiAddr(&own))
  {
    snifCount.noWifi++;
    return;
  }
  if(memcmp(pkt + 12, &own, 4) != 0)
  {
    snifCount.source++;
    return;
  }
  memcpy(&dst, pkt + 16, 4);
  if(ip4_addr_cmp(&dst, &own) || ip4_addr_isloopback(&dst))
  {
    snifCount.bad++;
    return;
  }
  // PBUF_LINK leaves room in front for the Wi-Fi link-layer header. The
  // interface's output function (etharp_output) finds the next hop
  // (directly, or the gateway), adds the Ethernet header and sends it; it
  // does not take p, so it is freed here.
  struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)total, PBUF_RAM);
  if(p == NULL)
  {
    snifCount.noWifi++;
    return;
  }
  memcpy(p->payload, pkt, total);
  wifi->output(wifi, p, &dst);
  pbuf_free(p);
  internalLedNetActivity();
}

/* ---- the serial line ------------------------------------------------- */

/*
   Start a session's output side. Flow control follows the command mode
   setting as it always has: RTS/CTS if that is selected (and a CTS pin is
   assigned), otherwise none. XON/XOFF cannot be used, as the data is
   binary.
*/
static void snifStartOutput()
{
  snifCtsFlow = (commandMode.getFlowControlType() == FCT_RTSCTS)
              && pinIsSupported(pinCTS);
}

/* Free space in the serial output buffer, in bytes. */
static int snifTxFree()
{
  return serialOutBufferBytesRemaining();
}

/* Append one byte to the serial output buffer; the caller has checked
   that there is room. */
static void snifTxPut(uint8_t c)
{
  TBUF[TBUFtail] = c;
  if(++TBUFtail >= SER_WRITE_BUFSIZE)
    TBUFtail = 0;
}

/*
   Move as much of the serial output buffer to the UART as its transmit
   FIFO has room for, without waiting. Called from loop() only. The
   per-character delay of ATS44 is not applied: these are framed binary
   protocols, and delaying would only stall the link.
*/
static void snifTxDrain()
{
  while(TBUFhead != TBUFtail)
  {
    int room = HWSerial.availableForWrite();
    if(snifCtsFlow)
    {
      if(digitalRead(pinCTS) != ctsActive)
        break;
      room = SNIF_CTS_INFLIGHT - (SNIF_UART_FIFO - room);
    }
    if(room <= 0)
      break;
    // The run of bytes up to the write index, or to the end of the ring.
    int run = (TBUFtail > TBUFhead) ? (TBUFtail - TBUFhead)
                                    : (SER_WRITE_BUFSIZE - TBUFhead);
    if(run > room)
      run = room;
    HWSerial.write(&TBUF[TBUFhead], run);
    if(logFileOpen)
    {
      for(int i=0; i<run; i++)
        logSerialOut(TBUF[TBUFhead + i]);
    }
    TBUFhead += run;
    if(TBUFhead >= SER_WRITE_BUFSIZE)
      TBUFhead = 0;
    internalLedSerialActivity();
  }
}

/* Read up to max bytes that have already arrived, without waiting. */
static int snifRead(uint8_t *buf, int max)
{
  int n = HWSerial.available();
  if(n > max)
    n = max;
  if(n <= 0)
    return 0;
  n = HWSerial.read(buf, n);
  if(logFileOpen)
  {
    for(int i=0; i<n; i++)
      logSerialIn(buf[i]);
  }
  return n;
}

/*
   The receive buffer, SNIF_BUF_SIZE bytes: a SLIP or PPP frame being
   received, or a possible frame in command mode (automatic entry). Never
   both at once, so one buffer serves both. Allocated on first use and
   kept.
*/
static uint8_t *snifBuffer()
{
  static uint8_t *buf = 0;
  if(buf == 0)
    buf = (uint8_t *)malloc(SNIF_BUF_SIZE);
  return buf;
}

/* ---- leaving: the escape sequence ------------------------------------ */

/* The escape character, if it can be used in these modes: not if S2 has
   been set to a SLIP or PPP framing or escape byte. */
static bool snifEscapeUsable()
{
  uint8_t ec = (uint8_t)commandMode.EC;
  return (ec != 0xC0) && (ec != 0xDB) && (ec != 0x7E) && (ec != 0x7D);
}

/* A mode has started: no escape characters counted yet. */
static void snifEscapeStart()
{
  plussesInARow = 0;
  lastInputTimeMs = millis();
}

/* Every byte from the host goes through here, for the escape sequence's
   count and guard time (processPlusPlusPlus(), shared with the other
   modes). */
static void snifEscapeByte(uint8_t c)
{
  if(snifEscapeUsable())
    processPlusPlusPlus(c);
  else
    lastInputTimeMs = millis();
}

/* True once the escape sequence and the guard time after it are complete. */
static bool snifEscaped()
{
  return snifEscapeUsable() && checkPlusPlusPlusEscape();
}

/* ---- entering: a frame in command mode ------------------------------- */

enum SnifAutoState
{
  SNIF_AUTO_IDLE,   // nothing held
  SNIF_AUTO_TILDE,  // a 0x7E held: a PPP frame, or a typed '~'?
  SNIF_AUTO_FLAG,   // a 0x7E ending a frame: a new one, or nothing
  SNIF_AUTO_SLIP,   // after 0xC0: a SLIP frame?
  SNIF_AUTO_PPP,    // after 0x7E 0xFF: an LCP Configure-Request?
  SNIF_AUTO_SKIP    // the rest of a frame that is not wanted, to its end
};

static uint8_t snifAutoState = SNIF_AUTO_IDLE;
static int snifAutoLen = 0;          // bytes in snifBuffer()
static bool snifAutoEsc = false;     // the previous byte was an escape
static bool snifAutoEscSeen = false; // the frame so far had an escape
static int snifAutoPppData = 0;      // unescaped PPP bytes so far
static uint16_t snifAutoFcs = 0;     // PPP frame check sequence so far
static unsigned long snifAutoLastMs = 0;

// Bytes given back to command mode, in order (a ring).
static uint8_t snifRelease[SNIF_RELEASE_MAX];
static int snifReleaseHead = 0;
static int snifReleaseCount = 0;

static void snifReleaseByte(uint8_t c)
{
  if(snifReleaseCount < SNIF_RELEASE_MAX)
  {
    snifRelease[(snifReleaseHead + snifReleaseCount) % SNIF_RELEASE_MAX] = c;
    snifReleaseCount++;
  }
}

/* The next byte given back to command mode, or -1 if there is none. */
static int snifReleased()
{
  if(snifReleaseCount == 0)
    return -1;
  uint8_t c = snifRelease[snifReleaseHead];
  snifReleaseHead = (snifReleaseHead + 1) % SNIF_RELEASE_MAX;
  snifReleaseCount--;
  return c;
}

/* True if bytes are waiting to be given back to command mode. */
static bool snifReleaseWaiting()
{
  return snifReleaseCount > 0;
}

/* Forget a possible frame, and anything not yet given back. */
static void snifAutoReset()
{
  snifAutoState = SNIF_AUTO_IDLE;
  snifAutoLen = 0;
  snifAutoEsc = false;
  snifAutoEscSeen = false;
  snifReleaseHead = 0;
  snifReleaseCount = 0;
}

/*
   The FCS-16 of RFC 1662 (polynomial 0x8408, reflected), one byte at a
   time, without a table: the byte-wise form of the bit-by-bit loop, as in
   avr-libc's _crc_ccitt_update(), which documents it as the CRC used by
   PPP. Start with 0xFFFF; a whole frame with its FCS gives 0xF0B8.
*/
static uint16_t snifFcsByte(uint16_t fcs, uint8_t c)
{
  c ^= (uint8_t)fcs;
  c ^= (uint8_t)(c << 4);
  return (uint16_t)((fcs >> 8) ^ ((uint16_t)c << 8) ^ ((uint16_t)c << 3)
                    ^ (c >> 4));
}

#if INCLUDE_SLIP
/*
   The checks a SLIP frame's IPv4 header must pass, as far as the bytes
   received so far allow (len bytes at b). With complete true, the frame
   has ended and its length must also match.
*/
static bool snifSlipHeaderOk(const uint8_t *b, int len, bool complete)
{
  if(len >= 1)
  {
    if((b[0] < 0x45) || (b[0] > 0x4F))  // version 4, header 20 to 60 bytes
      return false;
  }
  if(len >= 4)
  {
    int total = (b[2] << 8) | b[3];
    if((total < (b[0] & 0x0F) * 4) || (total > SNIF_MTU))
      return false;
    if(complete && (total != len))
      return false;
  }
  int hlen = (b[0] & 0x0F) * 4;
  if((len >= hlen) && (len >= 20))
  {
    uint32_t sum = 0;
    for(int i=0; i<hlen; i+=2)
      sum += (b[i] << 8) | b[i+1];
    while(sum >> 16)
      sum = (sum & 0xFFFF) + (sum >> 16);
    if(sum != 0xFFFF)
      return false;
  }
  else
  if(complete)
    return false;  // too short to be IPv4
  return true;
}

/* True once a possible SLIP frame's whole IPv4 header has arrived (and,
   as the checks are made byte by byte, passed its checks). */
static bool snifSlipHeaderDone()
{
  uint8_t *b = snifBuffer();
  return (snifAutoLen >= 20) && (snifAutoLen >= (b[0] & 0x0F) * 4);
}

/* Give up a possible SLIP frame. Its bytes are given back if they could
   have been typed: no escape was seen and the IPv4 header was not
   complete (typed text always fails the header checks). */
static void snifAutoSlipFail()
{
  if(!snifAutoEscSeen && !snifSlipHeaderDone())
  {
    uint8_t *b = snifBuffer();
    for(int i=0; i<snifAutoLen; i++)
      snifReleaseByte(b[i]);
    snifAutoState = SNIF_AUTO_IDLE;
  }
  else
    snifAutoState = SNIF_AUTO_SKIP;  // binary: drop the rest of the frame
  snifAutoLen = 0;
}
#endif

/*
   A byte from the host in command mode, before anything else is done with
   it. Returns true if command mode must stop reading (a mode has just been
   entered). Every byte that is not (or turns out not to be) part of a
   frame is given back through snifReleased(), in order, for command mode
   to handle as usual.
*/
static bool snifAutoInput(uint8_t c)
{
  snifAutoLastMs = millis();
  switch(snifAutoState)
  {
  case SNIF_AUTO_IDLE:
#if INCLUDE_SLIP
    if((c == SLIP_END) && commandMode.autoSlip && (snifBuffer() != 0))
    {
      snifAutoState = SNIF_AUTO_SLIP;
      snifAutoLen = 0;
      snifAutoEsc = false;
      snifAutoEscSeen = false;
      return false;
    }
#endif
#if INCLUDE_PPP
    if((c == 0x7E) && commandMode.autoPpp && (snifBuffer() != 0))
    {
      snifAutoState = SNIF_AUTO_TILDE;
      return false;
    }
#endif
    snifReleaseByte(c);
    return false;

#if INCLUDE_PPP
  case SNIF_AUTO_TILDE:
  case SNIF_AUTO_FLAG:
    // A '~' typed on its own is given back; the flag that closed a binary
    // frame is not (it was never typed).
    if(c == 0x7E)
    {
      if(snifAutoState == SNIF_AUTO_TILDE)
        snifReleaseByte(0x7E);  // the earlier '~': waiting on this one now
      snifAutoState = SNIF_AUTO_TILDE;
      return false;
    }
    if((c != 0xFF) || !commandMode.autoPpp)
    {
      if(snifAutoState == SNIF_AUTO_TILDE)
        snifReleaseByte(0x7E);
      snifAutoState = SNIF_AUTO_IDLE;
      return snifAutoInput(c);  // c may start something itself
    }
    {
      uint8_t *b = snifBuffer();
      b[0] = 0x7E;
      b[1] = 0xFF;
      snifAutoLen = 2;
      snifAutoEsc = false;
      snifAutoPppData = 1;
      snifAutoFcs = snifFcsByte(0xFFFF, 0xFF);
      snifAutoState = SNIF_AUTO_PPP;
    }
    return false;

  case SNIF_AUTO_PPP:
  {
    uint8_t *b = snifBuffer();
    if(snifAutoLen >= SNIF_AUTO_PPP_MAX)
    {
      // Far too long for a Configure-Request: dropped, to its end.
      snifAutoState = SNIF_AUTO_SKIP;
      return snifAutoInput(c);
    }
    b[snifAutoLen++] = c;
    if(c == 0x7E)
    {
      // The closing flag. Address, control, protocol and code were checked
      // on the way; the FCS is checked here (PPP mode checks the rest).
      if((snifAutoPppData >= 10) && (snifAutoFcs == 0xF0B8)
      && !snifAutoEsc)
      {
        snifAutoState = SNIF_AUTO_IDLE;
        return pppMode.autoEnter(b, snifAutoLen);
      }
      // Not a Configure-Request. This flag may begin the next frame.
      snifAutoState = SNIF_AUTO_FLAG;
      return false;
    }
    if(snifAutoEsc)
    {
      snifAutoEsc = false;
      c ^= 0x20;
    }
    else
    if(c == 0x7D)
    {
      snifAutoEsc = true;
      return false;
    }
    // The unescaped bytes must begin FF 03 C0 21 01 (FF already seen).
    static const uint8_t start[5] = { 0xFF, 0x03, 0xC0, 0x21, 0x01 };
    if((snifAutoPppData < 5) && (c != start[snifAutoPppData]))
    {
      // Another PPP frame (0xFF was sent): dropped, to its closing flag.
      snifAutoState = SNIF_AUTO_SKIP;
      return false;
    }
    snifAutoPppData++;
    snifAutoFcs = snifFcsByte(snifAutoFcs, c);
    return false;
  }
#endif

#if INCLUDE_SLIP
  case SNIF_AUTO_SLIP:
  {
    uint8_t *b = snifBuffer();
    if(c == SLIP_END)
    {
      if(snifAutoLen == 0)
        return false;  // END END: a SLIP host's line noise flush
      if(snifSlipHeaderOk(b, snifAutoLen, true) && !snifAutoEsc)
      {
        int len = snifAutoLen;
        snifAutoState = SNIF_AUTO_IDLE;
        snifAutoLen = 0;
        return slipMode.autoEnter(len);
      }
      // Not a frame. This END may begin the next one.
      snifAutoSlipFail();
      snifAutoState = SNIF_AUTO_SLIP;
      snifAutoEsc = false;
      snifAutoEscSeen = false;
      return false;
    }
    if(snifAutoEsc)
    {
      snifAutoEsc = false;
      if(c == SLIP_ESC_END)
        c = SLIP_END;
      else
      if(c == SLIP_ESC_ESC)
        c = SLIP_ESC;
      else
      {
        snifAutoSlipFail();  // a SLIP protocol error (and not typed)
        return false;
      }
    }
    else
    if(c == SLIP_ESC)
    {
      snifAutoEsc = true;
      snifAutoEscSeen = true;
      return false;
    }
    if(snifAutoLen >= SNIF_MTU)
    {
      snifAutoSlipFail();
      return false;
    }
    // Checked byte by byte until the header is complete and correct.
    bool checking = !snifSlipHeaderDone();
    b[snifAutoLen++] = c;
    if(checking && !snifSlipHeaderOk(b, snifAutoLen, false))
      snifAutoSlipFail();
    return false;
  }
#endif

  case SNIF_AUTO_SKIP:
    // A frame delimiter ends the unwanted frame, and may begin another.
    if(c == 0x7E)
    {
      snifAutoState = SNIF_AUTO_FLAG;
      return false;
    }
    if(c == 0xC0)
    {
      snifAutoState = SNIF_AUTO_IDLE;
      return snifAutoInput(c);
    }
    return false;

  default:
    snifAutoState = SNIF_AUTO_IDLE;
    snifReleaseByte(c);
    return false;
  }
}

/*
   Called from command mode's loop(): a '~' held for SNIF_AUTO_TILDE_MS
   with nothing after it was typed, and a possible frame that has paused
   for SNIF_AUTO_GAP_MS is given up.
*/
static void snifAutoPoll()
{
  if(snifAutoState == SNIF_AUTO_IDLE)
    return;
  unsigned long idle = millis() - snifAutoLastMs;
  switch(snifAutoState)
  {
  case SNIF_AUTO_TILDE:
    if(idle >= SNIF_AUTO_TILDE_MS)
    {
      snifAutoState = SNIF_AUTO_IDLE;
      snifReleaseByte(0x7E);
    }
    break;
  case SNIF_AUTO_FLAG:
    if(idle >= SNIF_AUTO_TILDE_MS)
      snifAutoState = SNIF_AUTO_IDLE;
    break;
#if INCLUDE_SLIP
  case SNIF_AUTO_SLIP:
    if(idle >= SNIF_AUTO_GAP_MS)
      snifAutoSlipFail();
    break;
#endif
  default:
    if(idle >= SNIF_AUTO_GAP_MS)
      snifAutoState = SNIF_AUTO_IDLE;
    break;
  }
}

/* Write a line to the log file (AT&O), if it is open. The format is a
   flash string (PSTR). Call from loop() only: not in system context. */
static void snifLogf(PGM_P format, ...)
{
  if(!logFileOpen)
    return;
  char line[80];
  va_list args;
  va_start(args, format);
  vsnprintf_P(line, sizeof(line), format, args);
  va_end(args);
  logPrintln(line);
}

/* Log the dropped-packet counters when they have changed (from loop()). */
static void snifLogCounters()
{
  static SnifCounters logged = { 0, 0, 0, 0, 0 };
  if(memcmp(&logged, &snifCount, sizeof(logged)) == 0)
    return;
  logged = snifCount;
  snifLogf(PSTR("Dropped: full %lu, big %lu, source %lu, bad %lu, Wi-Fi %lu"),
           (unsigned long)snifCount.full, (unsigned long)snifCount.big,
           (unsigned long)snifCount.source, (unsigned long)snifCount.bad,
           (unsigned long)snifCount.noWifi);
}

#endif /* INCLUDE_SLIP || INCLUDE_PPP */