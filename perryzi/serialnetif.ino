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

/*
   Set up NAPT the first time either mode is used. lwIP allocates the
   table on the heap and can never free it, so it is created only when it
   is needed, and only once. ERR_BUF means it already exists.
*/
static bool snifNaptInit()
{
  static bool done = false;
  if(!done)
  {
    err_t err = ip_napt_init(SNIF_NAPT_MAX, SNIF_PORTMAP_MAX);
    done = (err == ERR_OK) || (err == ERR_BUF);
  }
  return done;
}

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
   The receive buffer, SNIF_MTU bytes: a SLIP frame being received, or a
   possible frame in command mode (automatic entry). Never both at once, so
   one buffer serves both. Allocated on first use and kept.
*/
static uint8_t *snifBuffer()
{
  static uint8_t *buf = 0;
  if(buf == 0)
    buf = (uint8_t *)malloc(SNIF_MTU);
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

/* ---- entering: a frame in command mode -------------------------------- */

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

/* The FCS-16 of RFC 1662, one byte at a time. */
static uint16_t snifFcsByte(uint16_t fcs, uint8_t c)
{
  fcs ^= c;
  for(int i=0; i<8; i++)
    fcs = (fcs & 1) ? ((fcs >> 1) ^ 0x8408) : (fcs >> 1);
  return fcs;
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
      // on the way; the FCS is checked here (lwIP checks the rest).
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

#endif /* INCLUDE_SLIP || INCLUDE_PPP */
