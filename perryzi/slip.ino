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

/*
   The mode's output function: an IP packet for the host. This may run in
   the SDK's system context (a packet from Wi-Fi), so it only encodes the
   packet into the serial output buffer, which loop() empties. The caller
   keeps p. Returns false, sending nothing, if the packet does not fit
   whole.
*/
bool SlipMode::toHost(struct pbuf *p)
{
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
    return false;

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
  return true;
}

/*
   Set up and start SLIP mode. Returns false, leaving the modem in command
   mode, if the receive buffer or lwIP's raw PCBs cannot be had.
*/
bool SlipMode::enter()
{
  rxBuf = snifBuffer();
  if(rxBuf == 0)
    return false;
  if(!snifNetStart(SlipMode::toHost))
    return false;
  rxLen = 0;
  rxEscaped = false;
  rxDiscard = false;
  snifStartOutput();
  snifEscapeStart();
  currMode = this;
  ip4_addr_t own;
  if(snifWifiAddr(&own))
    snifLogf(PSTR("SLIP mode: host address %s"), ip4addr_ntoa(&own));
  else
    snifLogf(PSTR("SLIP mode: no Wi-Fi address yet"));
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
  // lwIP handles everything itself again; nothing more for the host.
  snifNetStop();
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
   A complete frame from the host: an IPv4 packet, checked and sent on
   (see snifFromHost()).
*/
void SlipMode::deliver()
{
  snifFromHost(rxBuf, rxLen);
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
  snifLogCounters();
  logFileLoop();
}

#endif /* INCLUDE_SLIP */