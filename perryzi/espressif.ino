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
   Espressif ESP-AT compatibility layer implementation. See espressif.h for
   provenance, target version, and scope.
*/

#ifdef ESPRESSIF_CMDS

static ZSerial espSerial;

/* ---- link state ------------------------------------------------- */

static EspAtLink espLinkType     = ESPAT_LINK_NONE;
static WiFiClient *espTcp        = null;
static WiFiUDP *espUdp           = null;
static String espRemoteHost      = "";
static int espRemotePort         = 0;
static int espLocalPort          = 0;
static bool espUdpHasPacket      = false;
static int espUdpPending         = 0;

/* ---- mode flags (settable via AT) -------------------------------- */

static int espRecvMode           = 0;     /* AT+CIPRECVMODE  */
static int espDInfo              = 0;     /* AT+CIPDINFO     */
static int espTransMode          = 0;     /* AT+CIPMODE      */
static int espSysStore           = 1;     /* AT+SYSSTORE     */
static int espDnsManual          = 0;     /* AT+CIPDNS       */
static int espServerMaxConn      = 1;     /* AT+CIPSERVERMAXCONN */
static int espServerTimeout      = 180;   /* AT+CIPSTO       */
static bool espIpdNotified       = false; /* passive-mode +IPD latch */

/* ---- AT+CIPSEND raw capture state -------------------------------- */

static bool espSendActive        = false;
static bool espSendSkipLF        = false;
static unsigned long espSendLastByte = 0;
static IPAddress espRemoteIP;             /* resolved once at CIPSTART */
static bool espRemoteIPValid     = false;
static uint8_t *espSendBuf       = null;
static int espSendLen            = 0;
static int espSendGot            = 0;

/* ================================================================== */
/* helpers                                                            */
/* ================================================================== */

static void espEol()
{
  espSerial.prints("\r\n");
}

static void espLine(const char *s)
{
  espSerial.prints(s);
  espEol();
}

/* ESP-AT precedes its terminal status with a blank line. */
static void espOk()
{
  espEol();
  espLine("OK");
}

/* Case-insensitive test of whether buf starts with pfx. */
static bool espStarts(const char *buf, const char *pfx)
{
  while(*pfx != 0)
  {
    if(*buf == 0)
      return false;
    if(lc(*buf) != lc(*pfx))
      return false;
    buf++;
    pfx++;
  }
  return true;
}

/* Exact case-insensitive match. */
static bool espIs(const char *buf, const char *cmd)
{
  return espStarts(buf, cmd) && (strlen(buf) == strlen(cmd));
}

/* Strip one layer of surrounding double quotes, in place. */
static void espUnquote(char *s)
{
  int len = strlen(s);
  if((len >= 2) && (s[0] == '"') && (s[len-1] == '"'))
  {
    memmove(s, s+1, len-2);
    s[len-2] = 0;
  }
}

/* Split a comma separated argument list, honouring double quotes so that
   a comma inside a quoted string is not treated as a separator. Returns
   the number of fields found; each is left quoted exactly as supplied. */
static int espSplit(char *args, char *field[], int maxFields)
{
  int n = 0;
  bool inQuote = false;
  /* Always give the caller a usable field[0], even when there is nothing
     to split. Two callers pass f[0] on without checking the count, and
     an empty argument - "AT+CIPDOMAIN=" with nothing after the '=' -
     would otherwise hand them an uninitialised pointer. */
  if(maxFields > 0)
    field[0] = args;
  if((args == null) || (*args == 0))
    return 0;
  field[n++] = args;
  for(char *p = args; *p != 0; p++)
  {
    if(*p == '"')
      inQuote = !inQuote;
    else
    if((*p == ',') && (!inQuote))
    {
      *p = 0;
      if(n < maxFields)
        field[n++] = p+1;
      else
        return n;
    }
  }
  return n;
}

static void espCloseLink(bool announce)
{
  if(espTcp != null)
  {
    espTcp->stop();
    delete espTcp;
    espTcp = null;
  }
  if(espUdp != null)
  {
    espUdp->stop();
    delete espUdp;
    espUdp = null;
  }
  if((espLinkType != ESPAT_LINK_NONE) && announce)
    espLine("CLOSED");
  espLinkType = ESPAT_LINK_NONE;
  espRemoteHost = "";
  espRemotePort = 0;
  espLocalPort = 0;
  espIpdNotified = false;
  espUdpHasPacket = false;
  espUdpPending = 0;
}

static bool espLinkUp()
{
  if(espLinkType == ESPAT_LINK_TCP)
    return (espTcp != null) && (espTcp->connected() || (espTcp->available() > 0));
  if(espLinkType == ESPAT_LINK_UDP)
    return (espUdp != null);
  return false;
}

/* Bytes readable from the CURRENT received packet, for TCP.

   Stock ESP-AT is driven from lwIP's receive callback, so every packet
   that arrives becomes its own "+IPD" block. Polling available() from
   the main loop does not reproduce that: by the time we look, lwIP has
   merged consecutive packets into one receive buffer and we would emit
   a single larger block instead.

   That difference is not cosmetic. A host that strips an HTTP header
   out of the first block and then resumes hunting for the next "+IPD"
   marker finds nothing, because the body it wants is inside the block
   it has already walked past. Small replies - where the header and
   the body arrive close enough together to be merged - are lost
   entirely, while large ones survive because later packets still carry
   their own markers.

   peekAvailable() returns what is left of the head pbuf, which is one
   received packet, so sizing each block by it restores stock framing.
   available() is used as a fallback in case the two ever disagree. */
static int espAvailable();

static int espPacketAvailable()
{
  if(espLinkType != ESPAT_LINK_TCP)
    return espAvailable();
  if(espTcp == null)
    return 0;
  int pkt = (int)espTcp->peekAvailable();
  if(pkt > 0)
    return pkt;
  return espTcp->available();
}

/* Bytes currently readable on the link, across all buffered packets. */
static int espAvailable()
{
  if(espLinkType == ESPAT_LINK_TCP)
    return (espTcp == null) ? 0 : espTcp->available();
  if(espLinkType == ESPAT_LINK_UDP)
  {
    if(espUdp == null)
      return 0;
    if(!espUdpHasPacket)
    {
      int sz = espUdp->parsePacket();
      if(sz > 0)
      {
        espUdpHasPacket = true;
        espUdpPending = sz;
      }
    }
    return espUdpHasPacket ? espUdpPending : 0;
  }
  return 0;
}

/* Stream exactly howMany bytes from the link to the host. */
static int espStreamOut(int howMany)
{
  uint8_t chunk[ESPAT_IO_CHUNK];
  int sent = 0;
  while(sent < howMany)
  {
    int want = howMany - sent;
    if(want > ESPAT_IO_CHUNK)
      want = ESPAT_IO_CHUNK;
    int got = 0;
    if(espLinkType == ESPAT_LINK_TCP)
      got = espTcp->read(chunk, want);
    else
    if(espLinkType == ESPAT_LINK_UDP)
      got = espUdp->read(chunk, want);
    if(got <= 0)
      break;
    internalLedNetActivity();
    espSerial.write(chunk, got);
    sent += got;
    if(espLinkType == ESPAT_LINK_UDP)
    {
      espUdpPending -= got;
      if(espUdpPending <= 0)
      {
        espUdpHasPacket = false;
        espUdpPending = 0;
      }
    }
    yield();
  }
  return sent;
}

/* "+IPD,<len>" / "+IPD,<len>,"<ip>",<port>" prefix, per AT+CIPDINFO. */
static void espIpdHeader(int len, bool withColon)
{
  espSerial.printf("\r\n+IPD,%d", len);
  if(espDInfo == 1)
    espSerial.printf(",\"%s\",%d", espRemoteHost.c_str(), espRemotePort);
  if(withColon)
    espSerial.prints(":");
  else
    espEol();
}

/* ================================================================== */
/* AT+CIPSTART                                                        */
/* ================================================================== */

static ZResult espCipStart(char *args)
{
  char *f[6];
  int n = espSplit(args, f, 6);
  if(n < 3)
    return ZERROR;

  espUnquote(f[0]);
  espUnquote(f[1]);

  /* Reject anything we cannot actually do rather than pretending. */
  if(espStarts(f[0], "SSL"))
    return ZERROR;
  bool isUdp = espStarts(f[0], "UDP");
  if((!isUdp) && (!espStarts(f[0], "TCP")))
    return ZERROR;
  /* v6 variants cannot work: lwIP is built with LWIP_IPV6=0 here. */
  if(strlen(f[0]) > 3)
    return ZERROR;

  if(espLinkUp())
  {
    espLine("ALREADY CONNECTED");
    return ZERROR;
  }
  espCloseLink(false);

  const char *host = f[1];
  int port = atoi(f[2]);
  if((strlen(host) == 0) || (port <= 0) || (port > 65535))
    return ZERROR;

  if(isUdp)
  {
    /* AT+CIPSTART="UDP",<host>,<remote port>[,<local port>[,<mode>]] */
    int lport = (n >= 4) ? atoi(f[3]) : (random(32768) + 32768);
    if((lport <= 0) || (lport > 65535))
      return ZERROR;
    espUdp = new WiFiUDP();
    if(!espUdp->begin(lport))
    {
      delete espUdp;
      espUdp = null;
      return ZERROR;
    }
    espLinkType = ESPAT_LINK_UDP;
    espLocalPort = lport;
  }
  else
  {
    int keepAlive = (n >= 4) ? atoi(f[3]) : 0;
    espTcp = new WiFiClient();
    if(!espTcp->connect(host, port))
    {
      delete espTcp;
      espTcp = null;
      return ZERROR;
    }
    espTcp->setNoDelay(DEFAULT_NO_DELAY);
    if(keepAlive > 0)
      espTcp->keepAlive(keepAlive, 1, 3);
    espLinkType = ESPAT_LINK_TCP;
    espLocalPort = espTcp->localPort();
  }

  espRemoteHost = host;
  espRemotePort = port;
  /* Resolved here so that sending does not repeat the lookup. A UDP
     host that streams datagrams would otherwise take a blocking DNS
     query on every single send. */
  espRemoteIPValid = WiFi.hostByName(host, espRemoteIP);
  espIpdNotified = false;

  /* Both ntpsync and ping key off this word, not off OK. */
  espLine("CONNECT");
  return ZOK;
}

/* ================================================================== */
/* AT+CIPSEND                                                         */
/* ================================================================== */

static ZResult espCipSend(char *args)
{
  if(!espLinkUp())
    return ZERROR;

  char *f[4];
  int n = espSplit(args, f, 4);
  if(n < 1)
    return ZERROR;

  int len = atoi(f[0]);
  if((len <= 0) || (len > ESPAT_MAX_SEND))
    return ZERROR;

  /* UDP may override the destination for this datagram. */
  if((espLinkType == ESPAT_LINK_UDP) && (n >= 3))
  {
    espUnquote(f[1]);
    espRemoteHost = f[1];
    espRemotePort = atoi(f[2]);
    espRemoteIPValid = false;   /* retargeted: the cached address is stale */
  }

  espSendBuf = (uint8_t *)malloc(len);
  if(espSendBuf == null)
    return ZERROR;

  espSendLen = len;
  espSendGot = 0;
  espSendActive = true;
  /* The command reader stops at the CR of the terminating CRLF and
     leaves the LF in the UART buffer. Without this, that LF would be
     captured as the first payload byte, shifting the whole packet by
     one and dropping its last byte. Only a single LF arriving before
     any payload is discarded. */
  espSendSkipLF = true;
  espSendLastByte = millis();

  /* "OK" then the prompt. Everything from here to espSendLen bytes is
     raw payload and must not reach the line-based command reader. */
  espOk();
  espEol();
  espSerial.prints("> ");
  espSerial.flush();
  return ZIGNORE;
}

/* Push the captured payload and report, mirroring stock firmware. */
static void espSendComplete()
{
  bool ok = false;
  if(espLinkType == ESPAT_LINK_TCP)
  {
    if((espTcp != null) && espTcp->connected())
      ok = (espTcp->write(espSendBuf, espSendLen) == (size_t)espSendLen);
  }
  else
  if(espLinkType == ESPAT_LINK_UDP)
  {
    if(espUdp != null)
    {
      IPAddress rip;
      bool haveIp = espRemoteIPValid;
      if(haveIp)
        rip = espRemoteIP;
      else
        haveIp = WiFi.hostByName(espRemoteHost.c_str(), rip);
      if(haveIp)
      {
        if(espUdp->beginPacket(rip, espRemotePort) == 1)
        {
          espUdp->write(espSendBuf, espSendLen);
          ok = (espUdp->endPacket() == 1);
        }
      }
    }
  }

  if(ok)
  {
    internalLedNetActivity();
  }
  espSerial.printf("\r\nRecv %d bytes\r\n", espSendGot);
  espEol();
  espLine(ok ? "SEND OK" : "SEND FAIL");

  free(espSendBuf);
  espSendBuf = null;
  espSendActive = false;
  espSendSkipLF = false;
  espSendLen = 0;
  espSendGot = 0;
}

static bool espAtRawCapture()
{
  if(!espSendActive)
    return false;

  while((HWSerial.available() > 0) && (espSendGot < espSendLen))
  {
    uint8_t c = (uint8_t)HWSerial.read();
    if(espSendSkipLF)
    {
      espSendSkipLF = false;
      if(c == '\n')
        continue;
    }
    espSendBuf[espSendGot++] = c;
    espSendLastByte = millis();
  }
  if(espSendGot >= espSendLen)
    espSendComplete();
  return true;
}

/* ================================================================== */
/* AT+CIPRECVDATA                                                     */
/* ================================================================== */

static ZResult espCipRecvData(char *args)
{
  char *f[3];
  int n = espSplit(args, f, 3);
  if(n < 1)
    return ZERROR;

  int want = atoi(f[0]);
  if(want <= 0)
    return ZERROR;

  int have = espAvailable();

  /* Nothing buffered is an ERROR, not a zero-length block. Confirmed
     against stock firmware, which errors here even with the connection
     still open.

     This matters more than it looks. Host software polls this command
     until it errors: Snail's receive loop has exactly one exit, the
     ERROR branch, so a zero-length reply leaves it polling forever
     with the page already fetched. Passive mode announces arrival
     with +IPD,<len> first, so a host only reads once there is data. */
  if(have == 0)
    return ZERROR;

  int send = (have < want) ? have : want;

#if ESPRESSIF_VER == V1_7_6
  /* v1.x: name, comma, length, colon, then the data. */
  espSerial.printf("\r\n+CIPRECVDATA,%d", send);
  if(espDInfo == 1)
    espSerial.printf(",\"%s\",%d", espRemoteHost.c_str(), espRemotePort);
  espSerial.prints(":");
#else
  /* v2.x: name, colon, length, comma, then the data. The order of the
     two separators is the reverse of v1.x -- not a typo. */
  espSerial.printf("\r\n+CIPRECVDATA:%d", send);
  if(espDInfo == 1)
    espSerial.printf(",\"%s\",%d", espRemoteHost.c_str(), espRemotePort);
  espSerial.prints(",");
#endif
  if(send > 0)
    espStreamOut(send);

  /* Re-arm the passive notification once the buffer has been drained. */
  if(espAvailable() == 0)
    espIpdNotified = false;

  return ZOK;
}

/* ================================================================== */
/* socket pump                                                        */
/* ================================================================== */

/* Abandon a half-finished AT+CIPSEND.

   While a send is open espAtRawCapture() consumes every byte that
   arrives, so the command parser never runs. A host that promises
   more bytes than it delivers - because it crashed, reset, or
   miscounted - would otherwise wedge the firmware permanently, with
   the payload buffer leaked and no way out but a power cycle. */
static void espSendAbort()
{
  if(espSendBuf != null)
    free(espSendBuf);
  espSendBuf = null;
  espSendActive = false;
  espSendSkipLF = false;
  espSendLen = 0;
  espSendGot = 0;
  espEol();
  espLine("SEND FAIL");
}

static void espAtLoop()
{
  if(espSendActive)
  {
    if((millis() - espSendLastByte) >= ESPAT_SEND_TIMEOUT_MS)
      espSendAbort();
    return;
  }
  if(espLinkType == ESPAT_LINK_NONE)
    return;

  int avail = espAvailable();

  if(avail > 0)
  {
    if(espRecvMode == 1)
    {
      /* Passive: announce once, then wait to be read with
         AT+CIPRECVDATA. Stock firmware does not repeat the
         notification until the previous one has been consumed. The
         count reported is the total buffered rather than one packet,
         because that is what the following AT+CIPRECVDATA can return. */
      if(!espIpdNotified)
      {
        espIpdHeader(avail, false);
        espIpdNotified = true;
      }
    }
    else
    {
      /* Active: push one received packet per +IPD block, matching the
         framing stock produces. See espPacketAvailable(). */
      int send = espPacketAvailable();
      if(send <= 0)
        send = avail;
      if(send > ESPAT_IPD_MAX)
        send = ESPAT_IPD_MAX;
      espIpdHeader(send, true);
      espStreamOut(send);
    }
    return;
  }

  /* Nothing buffered. A TCP link that has dropped is reported once. */
  if((espLinkType == ESPAT_LINK_TCP)
  &&(espTcp != null)
  &&(!espTcp->connected()))
    espCloseLink(true);
}

/* ================================================================== */
/* informational / query commands                                     */
/* ================================================================== */

static void espGmr()
{
  espSerial.printf("AT version:%s\r\n", ESPAT_AT_VERSION);
  espSerial.printf("SDK version:%s\r\n", ESP.getSdkVersion());
  espSerial.printf("compile time:%s\r\n", compile_date);
  espSerial.printf("Bin version:%s(PerryZi)\r\n", PERRYZI_VERSION);
}

static void espCifsr()
{
  espSerial.printf("+CIFSR:STAIP,\"%s\"\r\n", WiFi.localIP().toString().c_str());
  espSerial.printf("+CIFSR:STAMAC,\"%s\"\r\n", WiFi.macAddress().c_str());
}

static void espCipStaQuery()
{
  espSerial.printf("+CIPSTA:ip:\"%s\"\r\n", WiFi.localIP().toString().c_str());
  espSerial.printf("+CIPSTA:gateway:\"%s\"\r\n", WiFi.gatewayIP().toString().c_str());
  espSerial.printf("+CIPSTA:netmask:\"%s\"\r\n", WiFi.subnetMask().toString().c_str());
}

/* Connection status, shared by AT+CIPSTATUS and AT+CIPSTATE. */
static void espStatusLine(const char *tag)
{
  if(espLinkType == ESPAT_LINK_NONE)
    return;
  espSerial.printf("%s:0,\"%s\",\"%s\",%d,%d,0\r\n",
                   tag,
                   (espLinkType == ESPAT_LINK_UDP) ? "UDP" : "TCP",
                   espRemoteHost.c_str(),
                   espRemotePort,
                   espLocalPort);
}

static int espStatusCode()
{
  if(WiFi.status() != WL_CONNECTED)
    return 5;
  if(espLinkUp())
    return 3;
  if(espLinkType != ESPAT_LINK_NONE)
    return 4;
  return 2;
}

static ZResult espCipDomain(char *args)
{
  espUnquote(args);
  if(strlen(args) == 0)
    return ZERROR;
  IPAddress ip;
  if(!WiFi.hostByName(args, ip))
    return ZERROR;
#if ESPRESSIF_VER == V1_7_6
  espSerial.printf("+CIPDOMAIN:%s\r\n", ip.toString().c_str());   /* bare */
#else
  espSerial.printf("+CIPDOMAIN:\"%s\"\r\n", ip.toString().c_str()); /* quoted */
#endif
  return ZOK;
}

static ZResult espCwLap()
{
  int n = WiFi.scanNetworks();
  if(n < 0)
    return ZERROR;
  for(int i=0; i<n; i++)
  {
    espSerial.printf("+CWLAP:(%d,\"%s\",%d,\"%s\",%d)\r\n",
                     (int)WiFi.encryptionType(i),
                     WiFi.SSID(i).c_str(),
                     WiFi.RSSI(i),
                     WiFi.BSSIDstr(i).c_str(),
                     WiFi.channel(i));
    yield();
  }
  WiFi.scanDelete();
  return ZOK;
}

static ZResult espCwJap(char *args)
{
  char *f[4];
  int n = espSplit(args, f, 4);
  if(n < 1)
    return ZERROR;
  espUnquote(f[0]);
  if(n >= 2)
    espUnquote(f[1]);
  if(strlen(f[0]) == 0)
    return ZERROR;

  const char *pw = (n >= 2) ? f[1] : "";
  if(!connectWifi(f[0], pw, staticIP, staticDNS, staticGW, staticSN))
  {
    /* 4 == "connection failed" in the stock error table. */
    espLine("+CWJAP:4");
    return ZIGNORE_SPECIAL;
  }

  /* Matches what ATW does: the live globals are updated, but the change
     is not written to storage. AT&W persists it, as it always has. */
  wifiSSI = f[0];
  wifiPW = pw;

  espLine("WIFI CONNECTED");
  espLine("WIFI GOT IP");
  return ZOK;
}

static ZResult espPing(char *args)
{
#if INCLUDE_PING
  espUnquote(args);
  if(strlen(args) == 0)
    return ZERROR;
  unsigned long start = millis();
  int res = ping(args);
  if(res < 0)
  {
    espLine("+PING:TIMEOUT");
    return ZERROR;
  }
  espSerial.printf("+PING:%lu\r\n", (unsigned long)(millis() - start));
  return ZOK;
#else
  return ZERROR;
#endif
}

/* ================================================================== */
/* dispatch                                                           */
/* ================================================================== */

/*
   Commands are tested longest-first where one name is a prefix of
   another (CIPSTAMAC before CIPSTA, CIPRECVDATA/CIPRECVMODE/CIPRECVLEN
   before each other, CIPSTARTEX before CIPSTART), because a careless
   ordering would let the shorter name swallow the longer one.
*/
static bool espAtCommand(uint8_t *vbuf, int vlen, ZResult *result)
{
  if((vbuf == null) || (vlen <= 0))
    return false;

  char *cmd = (char *)vbuf;
  char *eq = strchr(cmd, '=');
  char *args = (eq == null) ? null : (eq + 1);
  int nameLen = (eq == null) ? (int)strlen(cmd) : (int)(eq - cmd);

  bool isQuery = false;
  if((nameLen > 0) && (cmd[nameLen-1] == '?'))
  {
    isQuery = true;
    nameLen--;
  }

  /* Isolate the command name so the matchers can use plain comparisons. */
  char name[32];
  if(nameLen >= (int)sizeof(name))
    return false;
  memcpy(name, cmd, nameLen);
  name[nameLen] = 0;

#if ESPRESSIF_VER == V1_7_6
  /* v1.x spells many commands with a _CUR (apply now) or _DEF (apply
     and persist) suffix - AT+CWMODE_DEF, AT+CWJAP_CUR, AT+CIPSTA_DEF
     and so on. Both map onto the one handler here: PerryZi keeps a
     single live configuration and persists it with AT&W, so there is
     nothing for the distinction to select between. Stripping the
     suffix in this one place avoids duplicating every handler.

     A side effect is that AT+CIPDNS and AT+CWCOUNTRY, which exist only
     in suffixed form in v1.x, also answer to their bare names. That
     is a superset, not a conflict. */
  if(nameLen > 4)
  {
    char *tail = name + nameLen - 4;
    if((strcasecmp(tail, "_DEF") == 0) || (strcasecmp(tail, "_CUR") == 0))
      *tail = 0;
  }
#endif

  /* ---- basic ---------------------------------------------------- */

  if(espIs(name, "RST"))
  {
    /* Deliberately a reset of the AT layer, not ESP.restart(). A real
       reboot would drop the host's serial settings and stall the link
       for seconds; nothing this serves needs that, and everything that
       issues AT+RST simply waits then flushes. */
    espCloseLink(false);
    espRecvMode = 0;
    espDInfo = 0;
    espTransMode = 0;
    espOk();
    espLine("ready");
    *result = ZIGNORE_SPECIAL;
    return true;
  }
  if(espIs(name, "GMR"))
  {
    espGmr();
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CMD"))
  {
    espLine("+CMD:AT,ATE,AT+RST,AT+GMR,AT+CMD,AT+SYSRAM,AT+SYSSTORE,AT+RESTORE");
    espLine("+CMD:AT+CWMODE,AT+CWJAP,AT+CWQAP,AT+CWLAP,AT+CWAUTOCONN,AT+CWSTATE");
    espLine("+CMD:AT+CWHOSTNAME,AT+CWRECONNCFG,AT+CWCOUNTRY,AT+CIFSR,AT+CIPSTA");
    espLine("+CMD:AT+CIPSTAMAC,AT+CIPDNS,AT+CIPDOMAIN,AT+CIPMUX,AT+CIPMODE");
    espLine("+CMD:AT+CIPDINFO,AT+CIPSTART,AT+CIPSEND,AT+CIPCLOSE,AT+CIPSTATUS");
    espLine("+CMD:AT+CIPSTATE,AT+CIPRECVMODE,AT+CIPRECVDATA,AT+CIPRECVLEN");
    espLine("+CMD:AT+CIPSERVER,AT+CIPSERVERMAXCONN,AT+CIPSTO,AT+CIPTCPOPT");
    espLine("+CMD:AT+CIPSNTPCFG,AT+CIPSNTPTIME,AT+SYSTIMESTAMP,AT+PING");
    espLine("+CMD:AT+RFPOWER,AT+SLEEP,AT+GSLP");
    *result = ZOK;
    return true;
  }
  if(espIs(name, "SYSRAM"))
  {
    espSerial.printf("+SYSRAM:%d\r\n", (int)ESP.getFreeHeap());
    *result = ZOK;
    return true;
  }
  if(espIs(name, "SYSSTORE"))
  {
    if(isQuery)
      espSerial.printf("+SYSSTORE:%d\r\n", espSysStore);
    else
    if(args != null)
      espSysStore = (atoi(args) != 0) ? 1 : 0;
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "RESTORE"))
  {
    espCloseLink(false);
    espRecvMode = 0;
    espDInfo = 0;
    espTransMode = 0;
    espOk();
    espLine("ready");
    *result = ZIGNORE_SPECIAL;
    return true;
  }
  if(espIs(name, "GSLP"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    long ms = atol(args);
    espOk();
    espSerial.flush();
    ESP.deepSleep((uint64_t)ms * 1000ULL);
    *result = ZIGNORE_SPECIAL;
    return true;
  }

  /* ---- Wi-Fi ---------------------------------------------------- */

  if(espIs(name, "CWMODE"))
  {
    if(isQuery)
      espSerial.prints("+CWMODE:1\r\n");
    else
    if((args == null) || (atoi(args) != 1))
      { *result = ZERROR; return true; }  /* station only */
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CWJAP"))
  {
    if(isQuery)
    {
      if(WiFi.status() != WL_CONNECTED)
        espLine("No AP");
      else
        espSerial.printf("+CWJAP:\"%s\",\"%s\",%d,%d\r\n",
                         WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                         WiFi.channel(), WiFi.RSSI());
      *result = ZOK;
      return true;
    }
    if(args == null)
      { *result = ZERROR; return true; }
    *result = espCwJap(args);
    if(*result == ZIGNORE_SPECIAL)
    {
      /* "+CWJAP:<error code>" is followed by FAIL in v1.x and by ERROR
         in v2.x. */
      espEol();
#if ESPRESSIF_VER == V1_7_6
      espLine("FAIL");
#else
      espLine("ERROR");
#endif
    }
    return true;
  }
  if(espIs(name, "CWQAP"))
  {
    espCloseLink(false);
    WiFi.disconnect();
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CWLAP"))
  {
    *result = espCwLap();
    return true;
  }
  if(espIs(name, "CWAUTOCONN"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    /* PerryZi reconnects on its own schedule; 0 disables that. */
    nextReconnectDelay = (atoi(args) != 0) ? DEFAULT_RECONNECT_DELAY : 0;
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CWSTATE"))
  {
    int st = (WiFi.status() == WL_CONNECTED) ? 2 : 0;
    espSerial.printf("+CWSTATE:%d,\"%s\"\r\n", st,
                     (st == 2) ? WiFi.SSID().c_str() : "");
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CWHOSTNAME"))
  {
    if(isQuery)
      espSerial.printf("+CWHOSTNAME:%s\r\n", WiFi.hostname().c_str());
    else
    if(args != null)
    {
      espUnquote(args);
      hostname = args;
      setHostName(args);
    }
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CWRECONNCFG"))
  {
    if(isQuery)
      espSerial.printf("+CWRECONNCFG:%d,0\r\n",
                       (int)(nextReconnectDelay / 1000));
    else
    if(args != null)
    {
      char *f[3];
      int n = espSplit(args, f, 3);
      if(n < 1)
        { *result = ZERROR; return true; }
      long secs = atol(f[0]);
      nextReconnectDelay = (secs <= 0) ? 0 : (unsigned long)secs * 1000UL;
      if(nextReconnectDelay > MAX_RECONNECT_DELAY)
        nextReconnectDelay = MAX_RECONNECT_DELAY;
    }
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CWCOUNTRY"))
  {
    wifi_country_t ctry;
    if(isQuery)
    {
      if(!wifi_get_country(&ctry))
        { *result = ZERROR; return true; }
      char cc[4];
      memcpy(cc, ctry.cc, 3);   /* cc[] is not guaranteed terminated */
      cc[3] = 0;
      espSerial.printf("+CWCOUNTRY:%d,\"%s\",%d,%d\r\n",
                       (int)ctry.policy, cc,
                       (int)ctry.schan, (int)ctry.nchan);
      *result = ZOK;
      return true;
    }
    if(args == null)
      { *result = ZERROR; return true; }
    char *f[5];
    int n = espSplit(args, f, 5);
    if(n < 4)
      { *result = ZERROR; return true; }
    espUnquote(f[1]);
    if(strlen(f[1]) < 2)
      { *result = ZERROR; return true; }
    memset(&ctry, 0, sizeof(ctry));
    ctry.policy = (atoi(f[0]) != 0) ? WIFI_COUNTRY_POLICY_MANUAL
                                    : WIFI_COUNTRY_POLICY_AUTO;
    memcpy(ctry.cc, f[1], 3);
    ctry.schan = (uint8_t)atoi(f[2]);
    ctry.nchan = (uint8_t)atoi(f[3]);
    *result = wifi_set_country(&ctry) ? ZOK : ZERROR;
    return true;
  }
  if(espIs(name, "RFPOWER"))
  {
    if(isQuery)
      espSerial.prints("+RFPOWER:82\r\n");
    else
    if(args != null)
    {
      /* Stock units are 0.25 dBm; the Arduino core takes dBm. */
      float dbm = ((float)atoi(args)) / 4.0f;
      if(dbm < 0)
        dbm = 0;
      if(dbm > 20.5f)
        dbm = 20.5f;
      WiFi.setOutputPower(dbm);
    }
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "SLEEP"))
  {
    if(isQuery)
      espSerial.prints("+SLEEP:0\r\n");
    else
    if(args != null)
    {
      switch(atoi(args))
      {
        case 0:  WiFi.setSleepMode(WIFI_NONE_SLEEP);  break;
        case 1:  WiFi.setSleepMode(WIFI_LIGHT_SLEEP); break;
        case 2:  WiFi.setSleepMode(WIFI_MODEM_SLEEP); break;
        default: *result = ZERROR; return true;
      }
    }
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }

  /* ---- addressing ----------------------------------------------- */

  if(espIs(name, "CIFSR"))
  {
    espCifsr();
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSTAMAC"))      /* before CIPSTA */
  {
    if(isQuery)
      espSerial.printf("+CIPSTAMAC:\"%s\"\r\n", WiFi.macAddress().c_str());
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSTA"))
  {
    if(isQuery)
    {
      espCipStaQuery();
      *result = ZOK;
      return true;
    }
    if(args == null)
      { *result = ZERROR; return true; }
    char *f[4];
    int n = espSplit(args, f, 4);
    IPAddress ip, gw, sn;
    espUnquote(f[0]);
    if((n < 1) || (!ip.fromString(f[0])))
      { *result = ZERROR; return true; }
    if(n >= 2)
    {
      espUnquote(f[1]);
      if(!gw.fromString(f[1]))
        { *result = ZERROR; return true; }
    }
    else
      gw = ip;
    if(n >= 3)
    {
      espUnquote(f[2]);
      if(!sn.fromString(f[2]))
        { *result = ZERROR; return true; }
    }
    else
      sn = IPAddress(255,255,255,0);
    *result = WiFi.config(ip, gw, sn) ? ZOK : ZERROR;
    return true;
  }
  if(espIs(name, "CIPDNS"))
  {
    if(isQuery)
    {
      espSerial.printf("+CIPDNS:%d,\"%s\"", espDnsManual,
                       WiFi.dnsIP(0).toString().c_str());
      if(WiFi.dnsIP(1).isSet())
        espSerial.printf(",\"%s\"", WiFi.dnsIP(1).toString().c_str());
      espEol();
      *result = ZOK;
      return true;
    }
    if(args == null)
      { *result = ZERROR; return true; }
    char *f[4];
    int n = espSplit(args, f, 4);
    if(n < 1)
      { *result = ZERROR; return true; }
    /* <enable> 0 hands DNS back to DHCP, which lwIP will only do on a
       fresh lease. Refused rather than silently accepted. */
    if(atoi(f[0]) == 0)
      { *result = ZERROR; return true; }
    if(n < 2)
      { *result = ZERROR; return true; }
    IPAddress d1, d2;
    espUnquote(f[1]);
    if(!d1.fromString(f[1]))
      { *result = ZERROR; return true; }
    dns_setserver(0, d1);
    if(n >= 3)
    {
      espUnquote(f[2]);
      if(!d2.fromString(f[2]))
        { *result = ZERROR; return true; }
      dns_setserver(1, d2);
    }
    espDnsManual = 1;
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPDOMAIN"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    char *f[3];
    espSplit(args, f, 3);
    *result = espCipDomain(f[0]);
    return true;
  }

  /* ---- connection ----------------------------------------------- */

  if(espIs(name, "CIPMUX"))
  {
    if(isQuery)
      espSerial.prints("+CIPMUX:0\r\n");
    else
    if((args == null) || (atoi(args) != 0))
      { *result = ZERROR; return true; }  /* single connection only */
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPMODE"))
  {
    if(isQuery)
      espSerial.printf("+CIPMODE:%d\r\n", espTransMode);
    else
    if((args == null) || (atoi(args) != 0))
      { *result = ZERROR; return true; }  /* no passthrough mode */
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPDINFO"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    espDInfo = (atoi(args) != 0) ? 1 : 0;
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPRECVMODE"))    /* before CIPRECVDATA/LEN matching */
  {
    if(isQuery)
      espSerial.printf("+CIPRECVMODE:%d\r\n", espRecvMode);
    else
    if(args == null)
      { *result = ZERROR; return true; }
    else
    {
      espRecvMode = (atoi(args) != 0) ? 1 : 0;
      espIpdNotified = false;
    }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPRECVDATA"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    *result = espCipRecvData(args);
    return true;
  }
  if(espIs(name, "CIPRECVLEN"))
  {
    espSerial.printf("+CIPRECVLEN:%d,0,0,0,0\r\n", espAvailable());
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSTARTEX") || espIs(name, "CIPSTART"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    *result = espCipStart(args);
    return true;
  }
  if(espIs(name, "CIPSEND"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    *result = espCipSend(args);
    return true;
  }
  if(espIs(name, "CIPCLOSE"))
  {
    /* The reference gives the response as OK, with no error case for
       "nothing was open". Returning ERROR here is wrong, and actively
       misleads host software that scans the stream for ERROR as a
       failure marker. CLOSED is emitted only when a link really was
       open, which is how it behaves as an unsolicited message. */
    espCloseLink(true);
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSTATUS"))
  {
    espSerial.printf("STATUS:%d\r\n", espStatusCode());
    espStatusLine("+CIPSTATUS");
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSTATE"))
  {
    espStatusLine("+CIPSTATE");
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSTO"))
  {
    if(isQuery)
      espSerial.printf("+CIPSTO:%d\r\n", espServerTimeout);
    else
    if(args != null)
      espServerTimeout = atoi(args);
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSERVERMAXCONN"))   /* before CIPSERVER */
  {
    if(isQuery)
      espSerial.printf("+CIPSERVERMAXCONN:%d\r\n", espServerMaxConn);
    else
    if(args != null)
      espServerMaxConn = atoi(args);
    else
      { *result = ZERROR; return true; }
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSERVER"))
  {
    /* A server needs AT+CIPMUX=1, which this layer does not offer. */
    *result = ZERROR;
    return true;
  }
  if(espIs(name, "CIPTCPOPT"))
  {
    if(isQuery)
      /* <link_id>,<so_linger>,<tcp_nodelay>,<so_sndtimeo> - nodelay is
         the third field, not the fourth. */
      espSerial.printf("+CIPTCPOPT:0,0,%d,0\r\n", DEFAULT_NO_DELAY ? 1 : 0);
    *result = ZOK;
    return true;
  }

  /* ---- time ------------------------------------------------------ */

  if(espIs(name, "CIPSNTPCFG"))
  {
    if(isQuery)
    {
      espSerial.printf("+CIPSNTPCFG:%d,%d,\"%s\"\r\n",
                       zclock.isDisabled() ? 0 : 1,
                       zclock.getTimeZoneCode(),
                       zclock.getNtpServerHost().c_str());
      *result = ZOK;
      return true;
    }
    if(args == null)
      { *result = ZERROR; return true; }
    char *f[4];
    int n = espSplit(args, f, 4);
    if(n < 1)
      { *result = ZERROR; return true; }
    bool enable = (atoi(f[0]) != 0);
    if(n >= 2)
      zclock.setTimeZoneCode(atoi(f[1]));
    if(n >= 3)
    {
      espUnquote(f[2]);
      if(strlen(f[2]) > 0)
        zclock.setNtpServerHost(f[2]);
    }
    zclock.setDisabled(!enable);
    if(enable)
      zclock.forceUpdate();
    *result = ZOK;
    return true;
  }
  if(espIs(name, "CIPSNTPTIME"))
  {
    if(!zclock.isTimeSet())
      { *result = ZERROR; return true; }
    static const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    DateTimeClock &now = zclock.getCurrentTime();
    int mi = now.getMonth() - 1;
    if((mi < 0) || (mi > 11))
      mi = 0;
    espSerial.printf("+CIPSNTPTIME:%s %s %d %02d:%02d:%02d %d\r\n",
                     now.getDoW(), mon[mi],
                     now.getDay(), now.getHour(),
                     now.getMinute(), now.getSecond(),
                     now.getYear());
    *result = ZOK;
    return true;
  }
  if(espIs(name, "SYSTIMESTAMP"))
  {
    if(!zclock.isTimeSet())
      { *result = ZERROR; return true; }
    espSerial.printf("+SYSTIMESTAMP:%lu\r\n",
                     (unsigned long)zclock.getCurrentTime().getUnixEpoch());
    *result = ZOK;
    return true;
  }

  /* ---- diagnostics ----------------------------------------------- */

  if(espIs(name, "PING"))
  {
    if(args == null)
      { *result = ZERROR; return true; }
    char *f[3];
    espSplit(args, f, 3);
    *result = espPing(f[0]);
    return true;
  }

  return false;
}

#endif /* ESPRESSIF_CMDS */
