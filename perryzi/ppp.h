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
   a PPP server, using lwIP's own PPP implementation (pppos) for framing and
   negotiation (LCP, IPCP with VJ header compression, DNS addresses). The
   host is given SNIF_PEER_ADDR, the modem takes SNIF_LOCAL_ADDR, and the
   DNS servers offered are the ones the ESP8266 uses on Wi-Fi. No
   authentication is asked for. The mode ends, back to command mode, when
   the PPP session does (normally when the host sends an LCP
   Terminate-Request).
*/

#ifndef PERRYZI_PPP_H
#define PERRYZI_PPP_H

#if INCLUDE_PPP

#include <netif/ppp/ppp.h>
#include <netif/ppp/pppos.h>

class PppMode : public ZMode
{
  private:
    ppp_pcb *ppp = 0;           // lwIP's PPP session
    struct netif nif;           // the serial line's lwIP interface
    netif_output_fn pppOutput = 0; // lwIP's own output function for nif
    volatile bool linkUp = false;
    volatile bool sessionOver = false;
    uint32_t dropped = 0;       // packets for the host that did not fit
    uint32_t droppedLogged = 0;

    void endSession();

    static u32_t serialOutput(ppp_pcb *pcb, u8_t *data, u32_t len,
                              void *ctx);
    static void linkStatus(ppp_pcb *pcb, int err, void *ctx);
    static err_t netifOutput(struct netif *netif, struct pbuf *p,
                             const ip4_addr_t *ipaddr);

  public:
    bool switchTo();
    void serialIncoming();
    void loop();
};

#endif /* INCLUDE_PPP */
#endif /* PERRYZI_PPP_H */
