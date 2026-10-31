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
   PPP mode: PerryZi's own PPP server. See ppp.h, and serialnetif.h for the
   design it shares with SLIP mode.

   The option negotiation follows the automaton of RFC 1661, section 4, as
   far as a passive server needs it: the actions are named as there (irc,
   scr, sca, scn, str, sta, tlu, tld, tlf), and each event handler (rcr,
   rca, rcn, rtr, rta, timeout) does what its column of the state table
   says for each state.
*/

#if INCLUDE_PPP

#include <lwip/dns.h>

// Our own requests (PppMode::want).
#define PPP_WANT_ACCM   0x01
#define PPP_WANT_MAGIC  0x02
#define PPP_WANT_PFC    0x04
#define PPP_WANT_ACFC   0x08
#define PPP_WANT_ADDR   0x10

// Option types: LCP (RFC 1661) and IPCP (RFC 1332, RFC 1877).
#define LCP_OPT_MRU     1
#define LCP_OPT_ACCM    2
#define LCP_OPT_MAGIC   5
#define LCP_OPT_PFC     7
#define LCP_OPT_ACFC    8
#define IPCP_OPT_ADDR   3
#define IPCP_OPT_DNS1   129
#define IPCP_OPT_DNS2   131

// Packet codes (RFC 1661, section 5).
#define CP_CONF_REQ     1
#define CP_CONF_ACK     2
#define CP_CONF_NAK     3
#define CP_CONF_REJ     4
#define CP_TERM_REQ     5
#define CP_TERM_ACK     6
#define CP_CODE_REJ     7
#define LCP_PROTO_REJ   8
#define LCP_ECHO_REQ    9
#define LCP_ECHO_REP    10
#define LCP_DISCARD     11

// checkOption()'s answers.
#define PPP_OPT_ACK 0
#define PPP_OPT_NAK 1
#define PPP_OPT_REJ 2

static uint32_t pppGet32(const uint8_t *b)
{
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
       | ((uint32_t)b[2] << 8) | b[3];
}

static void pppPut32(uint8_t *b, uint32_t v)
{
  b[0] = (uint8_t)(v >> 24);
  b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);
  b[3] = (uint8_t)v;
}

/* A magic number: random and never zero. */
static uint32_t pppNewMagic()
{
  uint32_t m = ESP.random();
  return (m != 0) ? m : 1;
}

/* ---- framing (RFC 1662) ---------------------------------------------- */

/* True if c must be sent escaped: the flag and the escape always, and the
   control characters the control-character map (accm) asks for. */
static inline bool pppMustEscape(uint8_t c, uint32_t accm)
{
  return (c == 0x7E) || (c == 0x7D)
      || ((c < 0x20) && ((accm >> c) & 1));
}

static inline void pppPutEscaped(uint8_t c, uint32_t accm)
{
  if(pppMustEscape(c, accm))
  {
    snifTxPut(0x7D);
    snifTxPut(c ^ 0x20);
  }
  else
    snifTxPut(c);
}

/*
   Send one frame of protocol proto into the serial output buffer: the
   information field is either len bytes at d or the pbuf chain p. LCP
   packets always go with the default control-character map and full
   address, control and protocol fields (RFC 1662, RFC 1661 section 6);
   everything else with what the host asked for, once LCP is open. The
   frame is sent whole or not at all: false if there is no room. Can run
   in the SDK's system context (for an IP packet).
*/
bool PppMode::frameOut(uint16_t proto, const uint8_t *d, int len,
                       struct pbuf *p)
{
  bool lcpFrame = (proto == PPP_LCP);
  uint32_t accm = lcpFrame ? 0xFFFFFFFFUL : txAccm;
  uint8_t hdr[4];
  int hlen = 0;
  if(lcpFrame || !txAcfc)
  {
    hdr[hlen++] = 0xFF;
    hdr[hlen++] = 0x03;
  }
  if(!lcpFrame && txPfc && (proto < 0x100))
    hdr[hlen++] = (uint8_t)proto;
  else
  {
    hdr[hlen++] = (uint8_t)(proto >> 8);
    hdr[hlen++] = (uint8_t)proto;
  }

  // First pass: the frame check sequence, and the exact encoded size.
  uint16_t fcs = 0xFFFF;
  int need = 2;  // the two flags
  for(int i=0; i<hlen; i++)
  {
    fcs = snifFcsByte(fcs, hdr[i]);
    need += pppMustEscape(hdr[i], accm) ? 2 : 1;
  }
  for(struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *b = (const uint8_t *)q->payload;
    for(int i=0; i<q->len; i++)
    {
      fcs = snifFcsByte(fcs, b[i]);
      need += pppMustEscape(b[i], accm) ? 2 : 1;
    }
  }
  for(int i=0; i<len; i++)
  {
    fcs = snifFcsByte(fcs, d[i]);
    need += pppMustEscape(d[i], accm) ? 2 : 1;
  }
  fcs ^= 0xFFFF;
  uint8_t tail[2] = { (uint8_t)fcs, (uint8_t)(fcs >> 8) };  // low byte first
  need += pppMustEscape(tail[0], accm) ? 2 : 1;
  need += pppMustEscape(tail[1], accm) ? 2 : 1;
  if(need > snifTxFree())
    return false;

  // Second pass: the frame itself.
  snifTxPut(0x7E);
  for(int i=0; i<hlen; i++)
    pppPutEscaped(hdr[i], accm);
  for(struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *b = (const uint8_t *)q->payload;
    for(int i=0; i<q->len; i++)
      pppPutEscaped(b[i], accm);
  }
  for(int i=0; i<len; i++)
    pppPutEscaped(d[i], accm);
  pppPutEscaped(tail[0], accm);
  pppPutEscaped(tail[1], accm);
  snifTxPut(0x7E);
  return true;
}

/*
   The mode's output function: an IP packet for the host, passed only
   while IPCP is open, and only if it is no larger than the host's MRU.
   Possibly in the SDK's system context.
*/
bool PppMode::toHost(struct pbuf *p)
{
  if(pppMode.ipcp.state != PPP_OPENED)
    return true;  // no IP yet: not a lack of room
  if(p->tot_len > pppMode.txMru)
  {
    snifCount.big++;
    return true;
  }
  if(!pppMode.frameOut(PPP_IP, NULL, 0, p))
    return false;
  internalLedNetActivity();
  return true;
}

/* Send a control packet: code, identifier, then len bytes at data (cut to
   PPP_CP_MAX). */
void PppMode::cpSend(uint16_t proto, uint8_t code, uint8_t id,
                     const uint8_t *data, int len)
{
  uint8_t b[4 + PPP_CP_MAX];
  if(len > PPP_CP_MAX)
    len = PPP_CP_MAX;
  b[0] = code;
  b[1] = id;
  b[2] = (uint8_t)((4 + len) >> 8);
  b[3] = (uint8_t)(4 + len);
  if(len > 0)
    memcpy(b + 4, data, len);
  frameOut(proto, b, 4 + len, NULL);
}

/* ---- receiving ------------------------------------------------------- */

/* One byte from the host. */
void PppMode::rxByte(uint8_t c)
{
  if(c == 0x7E)
  {
    // The end of a frame (and the start of the next). A frame needs at
    // least a protocol field and the FCS; the FCS of a good frame, its
    // own included, is 0xF0B8. An escape just before the flag aborts it.
    if(!rxDiscard && !rxEsc && (rxLen >= 4) && (rxFcs == 0xF0B8))
      frameIn(rxBuf, rxLen - 2);
    rxLen = 0;
    rxEsc = false;
    rxDiscard = false;
    rxFcs = 0xFFFF;
    return;
  }
  if(rxDiscard)
    return;
  if(c == 0x7D)
  {
    rxEsc = true;
    return;
  }
  // A control character the host was asked to escape, arriving as it is,
  // was put there by the line: dropped (RFC 1662, section 4.2).
  if((c < 0x20) && ((rxAccm >> c) & 1))
    return;
  if(rxEsc)
  {
    rxEsc = false;
    c ^= 0x20;
  }
  if(rxLen >= SNIF_BUF_SIZE)
  {
    rxDiscard = true;  // longer than any frame can be
    return;
  }
  rxBuf[rxLen++] = c;
  rxFcs = snifFcsByte(rxFcs, c);
}

/* A good frame (len bytes at d, without its FCS). */
void PppMode::frameIn(uint8_t *d, int len)
{
  int i = 0;
  if((len >= 2) && (d[0] == 0xFF) && (d[1] == 0x03))
    i = 2;  // address and control (they may be left out)
  if(i >= len)
    return;
  uint16_t proto;
  if(d[i] & 1)
    proto = d[i++];  // a compressed protocol field
  else
  {
    if((i + 1 >= len) || !(d[i+1] & 1))
      return;        // not a valid protocol field
    proto = (uint16_t)((d[i] << 8) | d[i+1]);
    i += 2;
  }
  d += i;
  len -= i;
  // Before LCP is open, everything but LCP is discarded (RFC 1661,
  // section 3.4); IP passes only while IPCP is open.
  switch(proto)
  {
    case PPP_LCP:
      cpInput(lcp, d, len);
      break;
    case PPP_IPCP:
      if(lcp.state == PPP_OPENED)
        cpInput(ipcp, d, len);
      break;
    case PPP_IP:
      if(ipcp.state == PPP_OPENED)
        snifFromHost(d, len);
      break;
    default:
      if(lcp.state == PPP_OPENED)
      {
        // Protocol-Reject: the protocol, then the start of the packet.
        uint8_t b[PPP_CP_MAX];
        int n = (len > PPP_CP_MAX - 2) ? (PPP_CP_MAX - 2) : len;
        b[0] = (uint8_t)(proto >> 8);
        b[1] = (uint8_t)proto;
        memcpy(b + 2, d, n);
        cpSend(PPP_LCP, LCP_PROTO_REJ, ++nextId, b, 2 + n);
      }
      break;
  }
}

/* A control packet (len bytes at d) for LCP or IPCP. */
void PppMode::cpInput(PppCp &cp, uint8_t *d, int len)
{
  if(len < 4)
    return;
  uint8_t code = d[0];
  uint8_t id = d[1];
  int plen = (d[2] << 8) | d[3];
  if((plen < 4) || (plen > len))
    return;  // RFC 1661, section 5: discarded (padding after it is not)
  uint8_t *data = d + 4;
  int dlen = plen - 4;
  switch(code)
  {
    case CP_CONF_REQ:
      rcr(cp, id, data, dlen);
      break;
    case CP_CONF_ACK:
      if(id == cp.reqId)
        rca(cp);
      break;
    case CP_CONF_NAK:
    case CP_CONF_REJ:
      if(id == cp.reqId)
        rcn(cp, code == CP_CONF_REJ, data, dlen);
      break;
    case CP_TERM_REQ:
      rtr(cp, id);
      break;
    case CP_TERM_ACK:
      rta(cp);
      break;
    case CP_CODE_REJ:
      break;  // everything sent here is needed: nothing can be left out
    default:
      if(cp.proto == PPP_LCP)
        lcpOther(code, id, d, plen);
      else
        cpSend(cp.proto, CP_CODE_REJ, ++nextId, d, plen);
      break;
  }
}

/* LCP's own codes: Protocol-Reject, Echo, Discard; others are rejected. */
void PppMode::lcpOther(uint8_t code, uint8_t id, uint8_t *pkt, int len)
{
  switch(code)
  {
    case LCP_PROTO_REJ:
      // The host does not speak IPCP: nothing can pass.
      if((lcp.state == PPP_OPENED) && (len >= 6)
      && (((pkt[4] << 8) | pkt[5]) == PPP_IPCP) && (ipcp.state != PPP_INITIAL))
      {
        ipcp.timing = false;
        ipcp.state = PPP_STOPPED;
        tlf(ipcp);
      }
      break;
    case LCP_ECHO_REQ:
      // Answered in the Opened state only, with our magic number (or zero)
      // and the request's data.
      if((lcp.state == PPP_OPENED) && (len >= 8))
      {
        uint8_t b[PPP_CP_MAX];
        pppPut32(b, (want & PPP_WANT_MAGIC) ? magic : 0);
        int n = len - 8;
        if(n > PPP_CP_MAX - 4)
          n = PPP_CP_MAX - 4;
        memcpy(b + 4, pkt + 8, n);
        cpSend(PPP_LCP, LCP_ECHO_REP, id, b, 4 + n);
      }
      break;
    case LCP_ECHO_REP:
    case LCP_DISCARD:
      break;
    default:
      cpSend(PPP_LCP, CP_CODE_REJ, ++nextId, pkt, len);
      break;
  }
}

/* ---- options --------------------------------------------------------- */

/*
   One option of the host's Configure-Request (o: type, length, value).
   Returns PPP_OPT_ACK, PPP_OPT_REJ, or PPP_OPT_NAK with the value wanted
   written to nak (the whole option, as long as o's).
*/
int PppMode::checkOption(uint16_t proto, uint8_t *o, uint8_t *nak)
{
  uint8_t type = o[0];
  uint8_t len = o[1];
  if(proto == PPP_LCP)
  {
    switch(type)
    {
      case LCP_OPT_MRU:
        if(len != 4)
          return PPP_OPT_REJ;
        if(((o[2] << 8) | o[3]) < 68)  // too small for IP (RFC 791)
        {
          nak[0] = type;
          nak[1] = len;
          nak[2] = (uint8_t)(SNIF_MTU >> 8);
          nak[3] = (uint8_t)SNIF_MTU;
          return PPP_OPT_NAK;
        }
        return PPP_OPT_ACK;
      case LCP_OPT_ACCM:
        return (len == 6) ? PPP_OPT_ACK : PPP_OPT_REJ;
      case LCP_OPT_MAGIC:
      {
        if(len != 6)
          return PPP_OPT_REJ;
        uint32_t m = pppGet32(o + 2);
        if((m == 0) || ((want & PPP_WANT_MAGIC) && (m == magic)))
        {
          // Zero, or ours (a looped-back line?): suggest another.
          nak[0] = type;
          nak[1] = len;
          pppPut32(nak + 2, pppNewMagic());
          return PPP_OPT_NAK;
        }
        return PPP_OPT_ACK;
      }
      case LCP_OPT_PFC:
      case LCP_OPT_ACFC:
        return (len == 2) ? PPP_OPT_ACK : PPP_OPT_REJ;
      default:
        // Authentication (3), quality protocol (4), and anything else.
        return PPP_OPT_REJ;
    }
  }
  switch(type)
  {
    case IPCP_OPT_ADDR:
    {
      // The host's address: the ESP8266's own (rcr() has checked that
      // there is one).
      if(len != 6)
        return PPP_OPT_REJ;
      ip4_addr_t own;
      snifWifiAddr(&own);
      if(memcmp(o + 2, &own, 4) == 0)
        return PPP_OPT_ACK;
      nak[0] = type;
      nak[1] = len;
      memcpy(nak + 2, &own, 4);
      return PPP_OPT_NAK;
    }
    case IPCP_OPT_DNS1:
    case IPCP_OPT_DNS2:
    {
      // The ESP8266's own DNS servers; rejected if it has no such one.
      if(len != 6)
        return PPP_OPT_REJ;
      const ip_addr_t *dns = dns_getserver((type == IPCP_OPT_DNS1) ? 0 : 1);
      if((dns == NULL) || ip_addr_isany(dns))
        return PPP_OPT_REJ;
      if(memcmp(o + 2, ip_2_ip4(dns), 4) == 0)
        return PPP_OPT_ACK;
      nak[0] = type;
      nak[1] = len;
      memcpy(nak + 2, ip_2_ip4(dns), 4);
      return PPP_OPT_NAK;
    }
    default:
      // VJ compression (2), the old IP-Addresses (1), NBNS (130, 132)...
      return PPP_OPT_REJ;
  }
}

/* The host's Configure-Request has been acknowledged: note what it asked
   for (LCP's options take effect when LCP opens). */
void PppMode::applyRequest(PppCp &cp, uint8_t *data, int len)
{
  if(cp.proto != PPP_LCP)
    return;
  peerAccm = 0xFFFFFFFFUL;  // the defaults, for options not asked for
  peerPfc = false;
  peerAcfc = false;
  peerMru = SNIF_MTU;
  for(int i = 0; i + 2 <= len; i += data[i+1])
  {
    uint8_t *o = data + i;
    if(o[1] < 2)
      break;
    if(o[0] == LCP_OPT_ACCM)
      peerAccm = pppGet32(o + 2);
    else
    if(o[0] == LCP_OPT_PFC)
      peerPfc = true;
    else
    if(o[0] == LCP_OPT_ACFC)
      peerAcfc = true;
    else
    if(o[0] == LCP_OPT_MRU)
    {
      peerMru = (uint16_t)((o[2] << 8) | o[3]);
      if(peerMru > SNIF_MTU)
        peerMru = SNIF_MTU;  // nothing larger is ever sent anyway
    }
  }
}

/* The host's Configure-Nak or Configure-Reject of our request: change
   it as asked. */
void PppMode::applyNak(PppCp &cp, bool reject, uint8_t *data, int len)
{
  for(int i = 0; i + 2 <= len; i += data[i+1])
  {
    uint8_t *o = data + i;
    if((o[1] < 2) || (i + o[1] > len))
      break;
    if(cp.proto == PPP_LCP)
    {
      switch(o[0])
      {
        case LCP_OPT_ACCM:
          if(reject || (o[1] != 6))
            want &= ~PPP_WANT_ACCM;
          else
            askAccm = pppGet32(o + 2);
          break;
        case LCP_OPT_MAGIC:
          if(reject)
            want &= ~PPP_WANT_MAGIC;
          else
            magic = pppNewMagic();
          break;
        case LCP_OPT_PFC:
          want &= ~PPP_WANT_PFC;
          break;
        case LCP_OPT_ACFC:
          want &= ~PPP_WANT_ACFC;
          break;
        default:
          break;  // an option suggested that is not wanted here
      }
    }
    else
    if(o[0] == IPCP_OPT_ADDR)
    {
      if(!reject && (o[1] == 6) && (pppGet32(o + 2) != 0))
        memcpy(&linkAddr, o + 2, 4);
      else
        want &= ~PPP_WANT_ADDR;
    }
  }
}

/* ---- the automaton's actions ----------------------------------------- */

/* irc: Initialize-Restart-Count, for configuration. */
void PppMode::irc(PppCp &cp)
{
  cp.restarts = PPP_MAX_CONFIGURE;
}

/* scr: Send-Configure-Request, and (re)start the restart timer. */
void PppMode::scr(PppCp &cp)
{
  uint8_t b[16];
  int n = 0;
  if(cp.proto == PPP_LCP)
  {
    if(want & PPP_WANT_ACCM)
    {
      b[n++] = LCP_OPT_ACCM;
      b[n++] = 6;
      pppPut32(b + n, askAccm);
      n += 4;
    }
    if(want & PPP_WANT_MAGIC)
    {
      b[n++] = LCP_OPT_MAGIC;
      b[n++] = 6;
      pppPut32(b + n, magic);
      n += 4;
    }
    if(want & PPP_WANT_PFC)
    {
      b[n++] = LCP_OPT_PFC;
      b[n++] = 2;
    }
    if(want & PPP_WANT_ACFC)
    {
      b[n++] = LCP_OPT_ACFC;
      b[n++] = 2;
    }
  }
  else
  if(want & PPP_WANT_ADDR)
  {
    b[n++] = IPCP_OPT_ADDR;
    b[n++] = 6;
    memcpy(b + n, &linkAddr, 4);
    n += 4;
  }
  cp.reqId = ++nextId;
  cpSend(cp.proto, CP_CONF_REQ, cp.reqId, b, n);
  if(cp.restarts > 0)
    cp.restarts--;
  cp.timing = true;
  cp.since = millis();
}

/* str: Send-Terminate-Request, and (re)start the restart timer. */
void PppMode::str(PppCp &cp)
{
  cpSend(cp.proto, CP_TERM_REQ, ++nextId, NULL, 0);
  if(cp.restarts > 0)
    cp.restarts--;
  cp.timing = true;
  cp.since = millis();
}

/* tlu: This-Layer-Up. LCP open: the host's options take effect, and IPCP
   starts (RFC 1661's Up event, with IPCP already opened administratively:
   irc, scr). IPCP open: IP passes. */
void PppMode::tlu(PppCp &cp)
{
  cp.timing = false;
  cp.naks = 0;
  if(cp.proto == PPP_LCP)
  {
    txAccm = peerAccm;
    txPfc = peerPfc;
    txAcfc = peerAcfc;
    txMru = peerMru;
    rxAccm = (want & PPP_WANT_ACCM) ? askAccm : 0xFFFFFFFFUL;
    snifLogf(PSTR("PPP: LCP open, MRU %u, map %08lx%s%s"), (unsigned)txMru,
             (unsigned long)txAccm, txPfc ? ", PFC" : "",
             txAcfc ? ", ACFC" : "");
    snifLinkPeer(&linkAddr);
    if(!ip4_addr_isany_val(linkAddr))
      want |= PPP_WANT_ADDR;
    else
      want &= ~PPP_WANT_ADDR;
    ipcp.naks = 0;
    ipcp.naksSent = 0;
    irc(ipcp);
    scr(ipcp);
    ipcp.state = PPP_REQSENT;
  }
  else
  {
    ip4_addr_t own;
    if(snifWifiAddr(&own))
      snifLogf(PSTR("PPP: IPCP open, host address %s"), ip4addr_ntoa(&own));
  }
}

/* tld: This-Layer-Down. LCP down: IPCP goes back to waiting, and the
   default framing applies again. */
void PppMode::tld(PppCp &cp)
{
  if(cp.proto == PPP_LCP)
  {
    ipcp.state = PPP_INITIAL;
    ipcp.timing = false;
    txAccm = 0xFFFFFFFFUL;
    txPfc = false;
    txAcfc = false;
    txMru = SNIF_MTU;
    rxAccm = 0xFFFFFFFFUL;
    snifLogf(PSTR("PPP: LCP down"));
  }
  else
    snifLogf(PSTR("PPP: IPCP down"));
}

/* tlf: This-Layer-Finished. For LCP, the session is over. */
void PppMode::tlf(PppCp &cp)
{
  if(cp.proto == PPP_LCP)
    endSession();
  else
    snifLogf(PSTR("PPP: IPCP gave up"));
}

/* ---- the automaton's events ------------------------------------------ */

/* RCR: the host's Configure-Request (len bytes of options at data). */
void PppMode::rcr(PppCp &cp, uint8_t id, uint8_t *data, int len)
{
  if((cp.state == PPP_INITIAL) || (cp.state == PPP_CLOSING)
  || (cp.state == PPP_STOPPING))
    return;
  // IPCP without a Wi-Fi address to give: no answer yet. The host will
  // ask again, and get one once Wi-Fi is connected.
  ip4_addr_t own;
  if((cp.proto == PPP_IPCP) && !snifWifiAddr(&own))
    return;
  // Every option is checked; the answer is a Configure-Reject of the
  // options that cannot be had, if any, otherwise a Configure-Nak with
  // the values wanted, if any, otherwise a Configure-Ack.
  uint8_t rej[PPP_CP_MAX];
  uint8_t nak[PPP_CP_MAX];
  int rejLen = 0;
  int nakLen = 0;
  for(int i = 0; i < len; )
  {
    if((i + 2 > len) || (data[i+1] < 2) || (i + data[i+1] > len))
      return;  // malformed: discarded
    uint8_t *o = data + i;
    int olen = o[1];
    uint8_t sug[8];
    int r = (olen <= (int)sizeof(sug)) ? checkOption(cp.proto, o, sug)
                                       : PPP_OPT_REJ;
    // After Max-Failure Naks in a row, reject instead (RFC 1661).
    if((r == PPP_OPT_NAK) && (cp.naksSent >= PPP_MAX_FAILURE))
      r = PPP_OPT_REJ;
    if((r == PPP_OPT_REJ) && (rejLen + olen <= PPP_CP_MAX))
    {
      memcpy(rej + rejLen, o, olen);
      rejLen += olen;
    }
    else
    if((r == PPP_OPT_NAK) && (nakLen + olen <= PPP_CP_MAX))
    {
      memcpy(nak + nakLen, sug, olen);
      nakLen += olen;
    }
    i += olen;
  }
  bool good = (rejLen == 0) && (nakLen == 0);

  uint8_t was = cp.state;
  if(was == PPP_STOPPED)
  {
    irc(cp);
    scr(cp);
  }
  else
  if(was == PPP_OPENED)
  {
    tld(cp);
    scr(cp);
  }
  if(rejLen > 0)
    cpSend(cp.proto, CP_CONF_REJ, id, rej, rejLen);
  else
  if(nakLen > 0)
  {
    cpSend(cp.proto, CP_CONF_NAK, id, nak, nakLen);
    cp.naksSent++;
  }
  else
  {
    applyRequest(cp, data, len);
    cpSend(cp.proto, CP_CONF_ACK, id, data, len);
    cp.naksSent = 0;
  }
  switch(was)
  {
    case PPP_ACKRCVD:
      if(good)
      {
        cp.state = PPP_OPENED;
        tlu(cp);
      }
      break;
    case PPP_STOPPED:
    case PPP_REQSENT:
    case PPP_ACKSENT:
    case PPP_OPENED:
      cp.state = good ? PPP_ACKSENT : PPP_REQSENT;
      break;
  }
}

/* RCA: the host has acknowledged our Configure-Request. */
void PppMode::rca(PppCp &cp)
{
  cp.naks = 0;
  switch(cp.state)
  {
    case PPP_STOPPED:
      cpSend(cp.proto, CP_TERM_ACK, ++nextId, NULL, 0);  // sta
      break;
    case PPP_REQSENT:
      irc(cp);
      cp.state = PPP_ACKRCVD;
      break;
    case PPP_ACKRCVD:
      scr(cp);  // crossed requests: start again
      cp.state = PPP_REQSENT;
      break;
    case PPP_ACKSENT:
      irc(cp);
      cp.state = PPP_OPENED;
      tlu(cp);
      break;
    case PPP_OPENED:
      tld(cp);
      scr(cp);
      cp.state = PPP_REQSENT;
      break;
  }
}

/* RCN: the host's Configure-Nak or Configure-Reject of our request. */
void PppMode::rcn(PppCp &cp, bool reject, uint8_t *data, int len)
{
  if((cp.state == PPP_INITIAL) || (cp.state == PPP_CLOSING)
  || (cp.state == PPP_STOPPING))
    return;
  if(cp.state == PPP_STOPPED)
  {
    cpSend(cp.proto, CP_TERM_ACK, ++nextId, NULL, 0);  // sta
    return;
  }
  applyNak(cp, reject, data, len);
  if(++cp.naks > PPP_MAX_NAKS)
  {
    // The two ends cannot agree: given up.
    cp.timing = false;
    cp.state = PPP_STOPPED;
    tlf(cp);
    return;
  }
  switch(cp.state)
  {
    case PPP_REQSENT:
    case PPP_ACKSENT:
      irc(cp);
      scr(cp);
      break;
    case PPP_ACKRCVD:
      scr(cp);
      cp.state = PPP_REQSENT;
      break;
    case PPP_OPENED:
      tld(cp);
      scr(cp);
      cp.state = PPP_REQSENT;
      break;
  }
}

/* RTR: the host's Terminate-Request. */
void PppMode::rtr(PppCp &cp, uint8_t id)
{
  if(cp.state == PPP_INITIAL)
    return;
  cpSend(cp.proto, CP_TERM_ACK, id, NULL, 0);  // sta
  switch(cp.state)
  {
    case PPP_ACKRCVD:
    case PPP_ACKSENT:
      cp.state = PPP_REQSENT;
      break;
    case PPP_OPENED:
      // tld, zrc: wait one restart time (so the host gets the
      // Terminate-Ack, and can repeat its request if it did not) before
      // this layer finishes.
      tld(cp);
      cp.restarts = 0;
      cp.timing = true;
      cp.since = millis();
      cp.state = PPP_STOPPING;
      if(cp.proto == PPP_LCP)
        snifLogf(PSTR("PPP: the host is closing the session"));
      break;
  }
}

/* RTA: the host's Terminate-Ack. */
void PppMode::rta(PppCp &cp)
{
  switch(cp.state)
  {
    case PPP_CLOSING:
    case PPP_STOPPING:
      cp.timing = false;
      cp.state = PPP_STOPPED;
      tlf(cp);
      break;
    case PPP_ACKRCVD:
      cp.state = PPP_REQSENT;
      break;
    case PPP_OPENED:
      tld(cp);
      scr(cp);
      cp.state = PPP_REQSENT;
      break;
  }
}

/* TO+ and TO-: the restart timer has expired. */
void PppMode::timeout(PppCp &cp)
{
  cp.timing = false;
  if(cp.restarts > 0)
  {
    switch(cp.state)
    {
      case PPP_CLOSING:
      case PPP_STOPPING:
        str(cp);
        break;
      case PPP_ACKRCVD:
        scr(cp);
        cp.state = PPP_REQSENT;
        break;
      case PPP_REQSENT:
      case PPP_ACKSENT:
        scr(cp);
        break;
    }
    return;
  }
  switch(cp.state)
  {
    case PPP_CLOSING:
    case PPP_STOPPING:
    case PPP_REQSENT:
    case PPP_ACKRCVD:
    case PPP_ACKSENT:
      cp.state = PPP_STOPPED;
      tlf(cp);
      break;
  }
}

/* The Close event (the escape sequence): end the session properly, with
   an LCP Terminate-Request, unless nothing has been negotiated yet. */
void PppMode::close()
{
  escaped = true;
  switch(lcp.state)
  {
    case PPP_OPENED:
      tld(lcp);
      // fall through
    case PPP_REQSENT:
    case PPP_ACKRCVD:
    case PPP_ACKSENT:
      lcp.restarts = PPP_MAX_TERMINATE;
      str(lcp);
      lcp.state = PPP_CLOSING;
      break;
    case PPP_STOPPING:
      lcp.state = PPP_CLOSING;
      break;
    case PPP_CLOSING:
      break;
    default:
      endSession();
      break;
  }
}

/* ---- the mode -------------------------------------------------------- */

/*
   Set up PPP mode and wait for the host to start negotiating. Returns
   false, leaving the modem in command mode, if the receive buffer or
   lwIP's raw PCBs cannot be had.
*/
bool PppMode::enter()
{
  rxBuf = snifBuffer();
  if(rxBuf == 0)
    return false;
  if(!snifNetStart(PppMode::toHost))
    return false;
  memset(&lcp, 0, sizeof(lcp));
  memset(&ipcp, 0, sizeof(ipcp));
  lcp.proto = PPP_LCP;
  lcp.state = PPP_STOPPED;  // passive: waiting for the host
  ipcp.proto = PPP_IPCP;
  ipcp.state = PPP_INITIAL;
  rxLen = 0;
  rxEsc = false;
  rxDiscard = false;
  rxFcs = 0xFFFF;
  rxAccm = 0xFFFFFFFFUL;
  txAccm = 0xFFFFFFFFUL;
  txPfc = false;
  txAcfc = false;
  txMru = SNIF_MTU;
  peerAccm = 0xFFFFFFFFUL;
  peerPfc = false;
  peerAcfc = false;
  peerMru = SNIF_MTU;
  // Ask for no control characters to be escaped, a magic number, and
  // compressed protocol and address/control fields (3 bytes fewer per
  // packet from the host).
  want = PPP_WANT_ACCM | PPP_WANT_MAGIC | PPP_WANT_PFC | PPP_WANT_ACFC;
  askAccm = 0;
  magic = pppNewMagic();
  ip4_addr_set_zero(&linkAddr);
  escaped = false;
  snifStartOutput();
  snifEscapeStart();
  currMode = this;
  snifLogf(PSTR("PPP mode: waiting for the host"));
  return true;
}

/* Enter PPP mode by command (AT+PPP, ATDT5517546). */
bool PppMode::switchTo()
{
  // As before the rewrite: give the host a moment to finish sending the
  // command line, then discard what is left of it (such as the LF after
  // the CR), so that it does not start the first frame.
  yield();
  delay(999);
  while(HWSerial.available() > 0)
    HWSerial.read();
  return enter();
}

/*
   Enter PPP mode automatically: command mode has received an LCP
   Configure-Request (S66), the len bytes at frame as they arrived, flags
   included. No response is sent, as the host is already speaking PPP; the
   frame is handled as the session's first. It is in snifBuffer(), which
   is also the receive buffer, so it is copied first.
*/
bool PppMode::autoEnter(uint8_t *frame, int len)
{
  uint8_t copy[SNIF_AUTO_PPP_MAX];
  if(len > SNIF_AUTO_PPP_MAX)
    return false;
  memcpy(copy, frame, len);
  if(!enter())
    return false;
  snifLogf(PSTR("PPP mode: entered automatically"));
  for(int i=0; (i<len) && (currMode == this); i++)
    rxByte(copy[i]);
  return true;
}

void PppMode::serialIncoming()
{
  uint8_t chunk[64];
  // A bounded amount per call, so that output keeps moving during a long
  // burst from the host.
  for(int budget = 512; (budget > 0) && (currMode == this); )
  {
    int n = snifRead(chunk, sizeof(chunk));
    if(n <= 0)
      break;
    for(int i=0; (i<n) && (currMode == this); i++)
    {
      snifEscapeByte(chunk[i]);
      rxByte(chunk[i]);
    }
    budget -= n;
  }
}

/* The session is over: back to command mode. */
void PppMode::endSession()
{
  snifNetStop();
  snifAutoReset();
  lcp.state = PPP_STOPPED;
  ipcp.state = PPP_INITIAL;
  lcp.timing = false;
  ipcp.timing = false;
  currMode = &commandMode;
  if(escaped)
  {
    escaped = false;
    snifLogf(PSTR("PPP: session closed by the escape sequence"));
    commandMode.sendOfficialResponse(ZNOCARRIER);
  }
  else
    snifLogf(PSTR("PPP: session over"));
}

void PppMode::loop()
{
  if(snifEscaped() && !escaped)
  {
    snifLogf(PSTR("PPP: escape sequence, closing the session"));
    close();
  }
  unsigned long now = millis();
  if((currMode == this) && lcp.timing
  && (now - lcp.since >= PPP_RESTART_MS))
    timeout(lcp);
  if((currMode == this) && ipcp.timing
  && (now - ipcp.since >= PPP_RESTART_MS))
    timeout(ipcp);
  if(currMode != this)
    return;
  snifTxDrain();
  snifLogCounters();
  logFileLoop();
}

#endif /* INCLUDE_PPP */