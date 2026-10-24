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
   PPP mode, as glue over lwIP's PPP server. See ppp.h, and serialnetif.h
   for the design it shares with SLIP mode.
*/

#if INCLUDE_PPP

#include <lwip/dns.h>

/*
   pppos hands over each encoded piece of a frame here (several per frame
   for a large packet), possibly in the SDK's system context, so it only
   copies into the serial output buffer, which loop() empties. A piece that
   does not fit is refused whole; lwIP then abandons the frame, and the
   host discards what it got of it by its checksum. netifOutput() makes
   sure that IP packets always fit, so this only happens to a control
   packet when the buffer is nearly full.
*/
u32_t PppMode::serialOutput(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx)
{
  (void)pcb;
  (void)ctx;
  if((int)len > snifTxFree())
    return 0;
  for(u32_t i=0; i<len; i++)
    snifTxPut(data[i]);
  return len;
}

/*
   The status of the PPP session has changed: up (PPPERR_NONE), or over
   for some reason. This can run in the SDK's system context (from a
   timer), so it only records the change for loop().
*/
void PppMode::linkStatus(ppp_pcb *pcb, int err, void *ctx)
{
  (void)pcb;
  PppMode *mode = (PppMode *)ctx;
  if(err == PPPERR_NONE)
    mode->linkUp = true;
  else
    mode->sessionOver = true;
}

/*
   Installed in place of lwIP's own output function for the interface: a
   packet for the host. lwIP's function encodes it at once, through
   serialOutput(), so the packet is passed on only if it is sure to fit
   whole in the serial output buffer; otherwise it is dropped, as a router
   would. lwIP keeps ownership of p either way. (netif->state belongs to
   lwIP's PPP code here, so the mode is reached through pppMode.)
*/
err_t PppMode::netifOutput(struct netif *netif, struct pbuf *p,
                           const ip4_addr_t *ipaddr)
{
  // An upper bound for the encoded frame: two flags, plus the address,
  // control, protocol and checksum fields (6 bytes) and every byte of the
  // packet that may need escaping (below 0x20, 0x7D or 0x7E), each of
  // which can take two bytes.
  int need = p->tot_len + 14;
  for(struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *d = (const uint8_t *)q->payload;
    for(int i=0; i<q->len; i++)
    {
      uint8_t c = d[i];
      if((c < 0x20) || (c == 0x7D) || (c == 0x7E))
        need++;
    }
  }
  if(need > snifTxFree())
  {
    pppMode.dropped++;
    return ERR_OK;
  }
  return pppMode.pppOutput(netif, p, ipaddr);
}

/*
   Set up PPP mode and wait for the host to start negotiating. Returns
   false, leaving the modem in command mode, if the NAPT table or the PPP
   session cannot be set up.
*/
bool PppMode::enter()
{
  if(!snifNaptInit())
    return false;
  ppp = pppos_create(&nif, PppMode::serialOutput, PppMode::linkStatus, this);
  if(ppp == 0)
    return false;

  ip4_addr_t addr;
  SNIF_LOCAL_ADDR(&addr);
  ppp_set_ipcp_ouraddr(ppp, &addr);
  SNIF_PEER_ADDR(&addr);
  ppp_set_ipcp_hisaddr(ppp, &addr);
  // Offer the DNS servers the ESP8266 itself uses, if the host asks.
  for(int i=0; i<2; i++)
  {
    const ip_addr_t *dns = dns_getserver(i);
    if((dns != 0) && !ip_addr_isany(dns))
      ppp_set_ipcp_dnsaddr(ppp, i, ip_2_ip4(dns));
  }
  // A server: answer the host's negotiation rather than start it.
  ppp_set_silent(ppp, 1);

  pppOutput = nif.output;
  nif.output = PppMode::netifOutput;
  // NAPT applies to packets that arrive on this interface and are
  // forwarded to another (Wi-Fi).
  ip_napt_enable_no(nif.num, 1);

  linkUp = false;
  sessionOver = false;
  escaped = false;
  snifStartOutput();

  if(ppp_listen(ppp) != ERR_OK)
  {
    ppp_free(ppp);
    ppp = 0;
    return false;
  }
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
   frame is passed to lwIP, which answers it.
*/
bool PppMode::autoEnter(uint8_t *frame, int len)
{
  if(!enter())
    return false;
  snifLogf(PSTR("PPP mode: entered automatically"));
  pppos_input(ppp, frame, len);
  return true;
}

void PppMode::serialIncoming()
{
  if(ppp == 0)
    return;
  uint8_t chunk[64];
  // A bounded amount per call, so that output keeps moving during a long
  // burst from the host.
  for(int budget = 512; budget > 0; )
  {
    int n = snifRead(chunk, sizeof(chunk));
    if(n <= 0)
      break;
    for(int i=0; i<n; i++)
      snifEscapeByte(chunk[i]);
    pppos_input(ppp, chunk, n);
    budget -= n;
  }
}

/*
   The session is over: release it (which also removes the interface) and
   return to command mode. lwIP only allows this once the session is
   fully closed, so it is retried on the next pass until then.
*/
void PppMode::endSession()
{
  if(ppp != 0)
  {
    if(ppp_free(ppp) != ERR_OK)
      return;
    ppp = 0;
  }
  sessionOver = false;
  snifAutoReset();
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
  if(snifEscaped() && !escaped && (ppp != 0))
  {
    // Close the session properly: lwIP sends the host an LCP
    // Terminate-Request and reports the session over once it is
    // acknowledged (or after its retries), and endSession() follows.
    escaped = true;
    snifLogf(PSTR("PPP: escape sequence, closing the session"));
    ppp_close(ppp, 0);
  }
  snifTxDrain();
  if(linkUp)
  {
    linkUp = false;
    snifLogf(PSTR("PPP: link up, host %s"), ip4addr_ntoa(netif_ip4_gw(&nif)));
  }
  if(dropped != droppedLogged)
  {
    snifLogf(PSTR("PPP: %lu packets for the host dropped (output full)"),
             (unsigned long)dropped);
    droppedLogged = dropped;
  }
  if(sessionOver)
    endSession();
  logFileLoop();
}

#endif /* INCLUDE_PPP */
