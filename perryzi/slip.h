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
   SLIP mode (RFC 1055): IP packets over the serial line, framed with END
   and escaped with ESC. Entered with AT+SLIP or ATDT5517545, or by the
   host sending a SLIP frame in command mode (S65=1), and left with the
   escape sequence, which answers NO CARRIER. The host shares the
   ESP8266's Wi-Fi address, which it can learn with DHCP (see
   serialnetif.h).
*/

#ifndef PERRYZI_SLIP_H
#define PERRYZI_SLIP_H

#if INCLUDE_SLIP

#define SLIP_END      0xC0
#define SLIP_ESC      0xDB
#define SLIP_ESC_END  0xDC
#define SLIP_ESC_ESC  0xDD

class SlipMode : public ZMode
{
  private:
    uint8_t *rxBuf = 0;        // the packet being received: snifBuffer()
    int rxLen = 0;
    bool rxEscaped = false;    // the previous byte was ESC
    bool rxDiscard = false;    // skip to the next END (bad frame)

    bool enter();
    void leave();
    void receiveByte(uint8_t c);
    void deliver();

    static bool toHost(struct pbuf *p);

  public:
    bool switchTo();
    bool autoEnter(int len);
    void serialIncoming();
    void loop();
};

#endif /* INCLUDE_SLIP */
#endif /* PERRYZI_SLIP_H */