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
   PPP mode: the host dials in (AT+PPP or ATDT5517546) and the modem acts as
   a PPP server, with PerryZi's own small PPP implementation: HDLC-like
   framing (RFC 1662), LCP (RFC 1661) and IPCP (RFC 1332, with the DNS
   addresses of RFC 1877). The host is given the ESP8266's own Wi-Fi
   address, which it then shares (see serialnetif.h); the modem gives the
   Wi-Fi gateway's address as its own, and offers the DNS servers the
   ESP8266 uses. No authentication is asked for, and VJ header compression
   is refused. The mode is also entered when the host sends an LCP
   Configure-Request in command mode (S66=1). It ends, back to command
   mode, when the host closes the session (LCP Terminate-Request), when
   negotiation fails, or after the escape sequence, which closes the
   session from this side and answers NO CARRIER.

   The modem waits for the host to start LCP (as a server, it is passive),
   then starts IPCP itself once LCP is open. IP packets pass only while
   IPCP is open.
*/

#ifndef PERRYZI_PPP_H
#define PERRYZI_PPP_H

#if INCLUDE_PPP

// Protocol numbers.
#define PPP_IP    0x0021
#define PPP_IPCP  0x8021
#define PPP_LCP   0xC021

// RFC 1661's timer and counters (its suggested values).
#define PPP_RESTART_MS     3000
#define PPP_MAX_CONFIGURE  10
#define PPP_MAX_TERMINATE  2
// Configure-Naks and Configure-Rejects of our requests in a row before the
// negotiation is given up (RFC 1661 leaves this to the implementation).
#define PPP_MAX_NAKS       10
// Configure-Naks sent in a row, after which the options are rejected
// instead (RFC 1661's Max-Failure).
#define PPP_MAX_FAILURE    5
// The longest control packet sent, after its 4-byte header: longer option
// lists and copies of rejected packets are cut to whole options or to
// this length.
#define PPP_CP_MAX         128

// The states of RFC 1661's option negotiation automaton that this server
// uses. INITIAL stands for IPCP's Initial and Starting (LCP not open).
enum PppState
{
  PPP_INITIAL,
  PPP_STOPPED,
  PPP_CLOSING,
  PPP_STOPPING,
  PPP_REQSENT,
  PPP_ACKRCVD,
  PPP_ACKSENT,
  PPP_OPENED
};

// One control protocol's automaton (LCP or IPCP).
struct PppCp
{
  uint16_t proto;      // PPP_LCP or PPP_IPCP
  uint8_t state;       // a PppState
  uint8_t reqId;       // identifier of our last Configure-Request
  uint8_t restarts;    // restart counter
  uint8_t naks;        // Configure-Naks/Rejects of ours in a row
  uint8_t naksSent;    // Configure-Naks sent in a row
  bool timing;         // the restart timer is running
  unsigned long since; // when it was started
};

class PppMode : public ZMode
{
  private:
    PppCp lcp;
    PppCp ipcp;
    uint8_t nextId = 0;          // identifier for our next request

    // Receiving.
    uint8_t *rxBuf = 0;          // the frame being received: snifBuffer()
    int rxLen = 0;
    bool rxEsc = false;          // the previous byte was 0x7D
    bool rxDiscard = false;      // skip to the next flag (too long)
    uint16_t rxFcs = 0xFFFF;
    uint32_t rxAccm = 0;         // control characters dropped if unescaped

    // What the host asked for in the request last acknowledged...
    uint32_t peerAccm = 0;
    bool peerPfc = false;
    bool peerAcfc = false;
    uint16_t peerMru = 0;

    // ...and in force while LCP is open (read by toHost(), possibly in the
    // SDK's system context).
    uint32_t txAccm = 0;
    bool txPfc = false;
    bool txAcfc = false;
    uint16_t txMru = 0;

    // Our own requests: which options, and their values.
    uint8_t want = 0;            // PPP_WANT_* bits
    uint32_t askAccm = 0;
    uint32_t magic = 0;
    ip4_addr_t linkAddr;         // the modem's address in IPCP

    bool escaped = false;        // the escape sequence is closing it

    bool enter();
    void endSession();
    void rxByte(uint8_t c);
    void frameIn(uint8_t *d, int len);
    bool frameOut(uint16_t proto, const uint8_t *d, int len, struct pbuf *p);
    void cpSend(uint16_t proto, uint8_t code, uint8_t id, const uint8_t *data,
                int len);
    void cpInput(PppCp &cp, uint8_t *d, int len);
    void lcpOther(uint8_t code, uint8_t id, uint8_t *pkt, int len);
    int checkOption(uint16_t proto, uint8_t *o, uint8_t *nak);
    void applyRequest(PppCp &cp, uint8_t *data, int len);
    void applyNak(PppCp &cp, bool reject, uint8_t *data, int len);
    void rcr(PppCp &cp, uint8_t id, uint8_t *data, int len);
    void rca(PppCp &cp);
    void rcn(PppCp &cp, bool reject, uint8_t *data, int len);
    void rtr(PppCp &cp, uint8_t id);
    void rta(PppCp &cp);
    void timeout(PppCp &cp);
    void close();
    void irc(PppCp &cp);
    void scr(PppCp &cp);
    void str(PppCp &cp);
    void tlu(PppCp &cp);
    void tld(PppCp &cp);
    void tlf(PppCp &cp);

    static bool toHost(struct pbuf *p);

  public:
    bool switchTo();
    bool autoEnter(uint8_t *frame, int len);
    void serialIncoming();
    void loop();
};

#endif /* INCLUDE_PPP */
#endif /* PERRYZI_PPP_H */