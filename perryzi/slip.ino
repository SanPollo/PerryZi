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
   SLIP mode. See slip.h, and serialnetif.h for the design it shares with
   PPP mode.
*/

#if INCLUDE_SLIP

/* lwIP calls this from netif_add() to set the interface up. */
err_t SlipMode::netifInit(struct netif *netif)
{
  netif->name[0] = 's';
  netif->name[1] = 'l';
  netif->mtu = SNIF_MTU;
  // A point-to-point line: no broadcast, no ARP, no link-layer header.
  netif->flags = 0;
  netif->output = SlipMode::netifOutput;
  return ERR_OK;
}

/*
   lwIP's output function for the interface: a packet for the host. This
   may run in the SDK's system context (a packet forwarded from Wi-Fi), so
   it only encodes the packet into the serial output buffer, which loop()
   empties. lwIP keeps ownership of p. A packet that does not fit whole is
   dropped.
*/
err_t SlipMode::netifOutput(struct netif *netif, struct pbuf *p,
                            const ip4_addr_t *ipaddr)
{
  (void)ipaddr;
  SlipMode *mode = (SlipMode *)netif->state;

  // The exact encoded size: END at each end, and every END or ESC byte in
  // the packet becomes two bytes.
  int need = p->tot_len + 2;
  for(struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *d = (const uint8_t *)q->payload;
    for(int i=0; i<q->len; i++)
    {
      if((d[i] == SLIP_END) || (d[i] == SLIP_ESC))
        need++;
    }
  }
  if(need > snifTxFree())
  {
    mode->dropped++;
    return ERR_OK;
  }

  // An END first as well as last, as RFC 1055 suggests, so that any line
  // noise before the packet ends up in a frame of its own and is
  // discarded.
  snifTxPut(SLIP_END);
  for(struct pbuf *q = p; q != NULL; q = q->next)
  {
    const uint8_t *d = (const uint8_t *)q->payload;
    for(int i=0; i<q->len; i++)
    {
      uint8_t c = d[i];
      if(c == SLIP_END)
      {
        snifTxPut(SLIP_ESC);
        snifTxPut(SLIP_ESC_END);
      }
      else
      if(c == SLIP_ESC)
      {
        snifTxPut(SLIP_ESC);
        snifTxPut(SLIP_ESC_ESC);
      }
      else
        snifTxPut(c);
    }
  }
  snifTxPut(SLIP_END);
  internalLedNetActivity();
  return ERR_OK;
}

/*
   Set up and start SLIP mode. Returns false, leaving the modem in command
   mode, if the receive buffer, the NAPT table or the interface cannot be
   set up.
*/
bool SlipMode::enter()
{
  rxBuf = snifBuffer();
  if(rxBuf == 0)
    return false;
  if(!snifNaptInit())
    return false;
  if(!netifAdded)
  {
    ip4_addr_t addr, mask, gw;
    SNIF_LOCAL_ADDR(&addr);
    SNIF_NETMASK(&mask);
    ip4_addr_set_zero(&gw);
    if(netif_add(&nif, &addr, &mask, &gw, this, SlipMode::netifInit,
                 ip_input) == NULL)
      return false;
    netifAdded = true;
    // NAPT applies to packets that arrive on this interface and are
    // forwarded to another (Wi-Fi).
    ip_napt_enable_no(nif.num, 1);
  }
  netif_set_link_up(&nif);
  netif_set_up(&nif);

  rxLen = 0;
  rxEscaped = false;
  rxDiscard = false;
  snifStartOutput();
  snifEscapeStart();
  currMode = this;
  snifLogf(PSTR("SLIP mode: interface %c%c%d up"), nif.name[0], nif.name[1],
           nif.num);
  return true;
}

/* Enter SLIP mode by command (AT+SLIP, ATDT5517545). */
bool SlipMode::switchTo()
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
   Enter SLIP mode automatically: command mode has received a complete
   SLIP frame of len bytes (S65), now in snifBuffer(), which is this mode's
   receive buffer. No response is sent, as the host is already speaking
   SLIP; the frame is handled as the first packet.
*/
bool SlipMode::autoEnter(int len)
{
  if(!enter())
    return false;
  snifLogf(PSTR("SLIP mode: entered automatically"));
  rxLen = len;
  deliver();
  rxLen = 0;
  return true;
}

/* Leave SLIP mode (after the escape sequence), back to command mode. */
void SlipMode::leave()
{
  // Down, so that nothing more is routed to the host; up again on entry.
  netif_set_down(&nif);
  netif_set_link_down(&nif);
  rxLen = 0;
  rxEscaped = false;
  rxDiscard = false;
  snifAutoReset();
  currMode = &commandMode;
  snifLogf(PSTR("SLIP mode: left by the escape sequence"));
  commandMode.sendOfficialResponse(ZNOCARRIER);
}

/* One byte from the host. */
void SlipMode::receiveByte(uint8_t c)
{
  if(c == SLIP_END)
  {
    if((!rxDiscard) && (rxLen > 0))
      deliver();
    rxLen = 0;
    rxEscaped = false;
    rxDiscard = false;
    return;
  }
  if(rxDiscard)
    return;
  if(rxEscaped)
  {
    rxEscaped = false;
    if(c == SLIP_ESC_END)
      c = SLIP_END;
    else
    if(c == SLIP_ESC_ESC)
      c = SLIP_ESC;
    else
    {
      // A protocol error: the frame is bad, so skip to the next END.
      rxDiscard = true;
      return;
    }
  }
  else
  if(c == SLIP_ESC)
  {
    rxEscaped = true;
    return;
  }
  if(rxLen >= SNIF_MTU)
  {
    // Larger than any packet can be: skip to the next END.
    rxDiscard = true;
    return;
  }
  rxBuf[rxLen++] = c;
}

/*
   A complete frame from the host. If it holds a plausible IPv4 packet,
   pass it to lwIP, which routes it (and checks the header checksum).
*/
void SlipMode::deliver()
{
  if(rxLen < 20)
    return;
  if((rxBuf[0] >> 4) != 4)
    return;
  int headerLen = (rxBuf[0] & 0x0F) * 4;
  int totalLen = (rxBuf[2] << 8) | rxBuf[3];
  if((headerLen < 20) || (totalLen < headerLen) || (totalLen > rxLen))
    return;
  // PBUF_LINK leaves room in front for the Wi-Fi link-layer header, which
  // is added when lwIP forwards the packet.
  struct pbuf *p = pbuf_alloc(PBUF_LINK, totalLen, PBUF_RAM);
  if(p == NULL)
    return;
  memcpy(p->payload, rxBuf, totalLen);
  // ip_input() takes ownership of p unless it reports an error.
  if(nif.input(p, &nif) != ERR_OK)
    pbuf_free(p);
  internalLedNetActivity();
}

void SlipMode::serialIncoming()
{
  uint8_t chunk[64];
  // A bounded amount per call, so that output keeps moving during a long
  // burst from the host.
  for(int budget = 512; budget > 0; )
  {
    int n = snifRead(chunk, sizeof(chunk));
    if(n <= 0)
      break;
    for(int i=0; i<n; i++)
    {
      snifEscapeByte(chunk[i]);
      receiveByte(chunk[i]);
    }
    budget -= n;
  }
}

void SlipMode::loop()
{
  if(snifEscaped())
  {
    leave();
    return;
  }
  snifTxDrain();
  if(dropped != droppedLogged)
  {
    snifLogf(PSTR("SLIP: %lu packets for the host dropped (output full)"),
             (unsigned long)dropped);
    droppedLogged = dropped;
  }
  logFileLoop();
}

#endif /* INCLUDE_SLIP */
