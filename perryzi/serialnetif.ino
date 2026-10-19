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
