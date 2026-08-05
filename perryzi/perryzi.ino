/*
   PerryZi - ESP8266 Firmware for the PerryFi

   Copyright 2016-2026 Bo Zimmerman
   Copyright 2026 Nick J. Date

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. 
*/
//#define TCP_SND_BUF                     4 * TCP_MSS
#define PERRYZI_VERSION "1.0.0"
const char compile_date[] = __DATE__ " " __TIME__;
#define DEFAULT_NO_DELAY true
#define null 0

#define NTP_SERVER "uk.pool.ntp.org"

#define INCLUDE_IRCC true
#define INCLUDE_PING true
#define INCLUDE_FTP true
#define INCLUDE_SLIP true
#define INCLUDE_PPP true
//#define SUPPORT_LED_PINS true // enable if you have the spare gpio pins and leds

#define ZIMODEM_ESP8266

#if SUPPORT_LED_PINS
# define DEFAULT_PIN_AA 35
# define DEFAULT_PIN_HS 34
# define DEFAULT_PIN_WIFI 26
# define DEFAULT_HS_BAUD 38400
# define DEFAULT_AA_ACTIVE LOW
# define DEFAULT_AA_INACTIVE HIGH
# define DEFAULT_HS_ACTIVE LOW
# define DEFAULT_HS_INACTIVE HIGH
# define DEFAULT_WIFI_ACTIVE LOW
# define DEFAULT_WIFI_INACTIVE HIGH
#endif

#define DEFAULT_BAUD_RATE 19200
#define DEFAULT_SERIAL_CONFIG SERIAL_8N1
#define RX_BUFFER_SIZE 4096

// PerryFi 1.0 pin configuration (WeMos D1 mini)
#if defined(ARDUINO_ESP8266_WEMOS_D1MINI)
#define DEFAULT_PIN_RTS 15  // WeMos D1 Mini: GPIO15 (D8) -> RTS
#define DEFAULT_PIN_CTS 13  // WeMos D1 Mini: GPIO13 (D7) -> CTS

// PerryFi 2.1 pin configuration (ESP-12F)
#elif defined(ARDUINO_ESP8266_GENERIC)
#define DEFAULT_PIN_RTS 13  // ESP-12F PerryFi: GPIO13 -> RTS
#define DEFAULT_PIN_CTS 4   // ESP-12F PerryFi: GPIO4  -> CTS

// Prevent building on other boards.
#else
#error UNSUPPORTED BOARD: Please select LOLIN(WEMOS) D1 mini for the PerryFi 1.0 or Generic ESP8266 Module for the PerryFi 2.1
#endif

#define DEFAULT_FCT FCT_DISABLED

// Unused pins
#define DEFAULT_PIN_DSR -1
#define DEFAULT_PIN_DTR -1
#define DEFAULT_PIN_RI  -1
#define DEFAULT_PIN_DCD -1
#define DEFAULT_PIN_OTH -1

#define debugPrintf doNothing //Serial.printf
#define preEOLN(...)
#define echoEOLN(...) serial.prints(EOLN)

# define DEFAULT_DCD_ACTIVE  LOW
# define DEFAULT_DCD_INACTIVE  HIGH
# define DEFAULT_CTS_ACTIVE  LOW
# define DEFAULT_CTS_INACTIVE  HIGH
# define DEFAULT_RTS_ACTIVE  LOW
# define DEFAULT_RTS_INACTIVE  HIGH
# define DEFAULT_RI_ACTIVE  LOW
# define DEFAULT_RI_INACTIVE  HIGH
# define DEFAULT_DSR_ACTIVE  LOW
# define DEFAULT_DSR_INACTIVE  HIGH
# define DEFAULT_DTR_ACTIVE  LOW
# define DEFAULT_DTR_INACTIVE  HIGH
# define DEFAULT_OTH_ACTIVE  LOW
# define DEFAULT_OTH_INACTIVE  HIGH

#define MAX_PIN_NO 50
#define INTERNAL_FLOW_CONTROL_DIV 380
#define DEFAULT_RECONNECT_DELAY 60000
#define MAX_RECONNECT_DELAY 1800000

class ZMode
{
  public:
    virtual void serialIncoming();
    virtual void loop();
};

#include "common.h"
#include "rt_clock.h"
#include "filelog.h"
#include "serout.h"
#include "connSettings.h"
#include "wificlientnode.h"
#include "stringstream.h"
#include "phonebook.h"
#include "wifiservernode.h"
#include "zstream.h"
#include "proto_http.h"
#include "proto_ftp.h"
#include "zconfigmode.h"
#include "zcommand.h"
#include "zprint.h"

#if INCLUDE_SLIP
#  include "zslipmode.h"
#endif
#if INCLUDE_PPP
#  include "zpppmode.h"
#endif
#if INCLUDE_IRCC
#  include "zircmode.h"
#endif

static WiFiClientNode *conns = null;
static WiFiServerNode *servs = null;
static PhoneBookEntry *phonebook = null;
static bool pinSupport[MAX_PIN_NO];
static int pinCache[MAX_PIN_NO];
static String termType = DEFAULT_TERMTYPE;
static String busyMsg = DEFAULT_BUSYMSG;
static bool debugUart = false;

static ZMode *currMode = null;
static ZStream streamMode;
static ZCommand commandMode;
static ZPrint printMode;
static ZConfig configMode;
static RealTimeClock zclock(0);
#if INCLUDE_SLIP
   static ZSLIPMode slipMode;
#endif
#if INCLUDE_PPP
   static ZPPPMode pppMode;
#endif
#if INCLUDE_IRCC
   static ZIRCMode ircMode;
#endif

enum BaudState
{
  BS_NORMAL,
  BS_SWITCH_TEMP_NEXT,
  BS_SWITCHED_TEMP,
  BS_SWITCH_NORMAL_NEXT
};

static String wifiSSI;
static String wifiPW;
static String hostname;
static IPAddress *staticIP = null;
static IPAddress *staticDNS = null;
static IPAddress *staticGW = null;
static IPAddress *staticSN = null;
static unsigned long lastConnectAttempt = 0;
static unsigned long nextReconnectDelay = 0; // zero means don't attempt reconnects
static SerialConfig serialConfig = DEFAULT_SERIAL_CONFIG;
static int baudRate=DEFAULT_BAUD_RATE;
static int dequeSize=1+(DEFAULT_BAUD_RATE/INTERNAL_FLOW_CONTROL_DIV);
static BaudState baudState = BS_NORMAL; 
static unsigned long resetPushTimer=0;
static int tempBaud = -1; // -1 do nothing
static unsigned int plussesInARow = 0;
static unsigned long lastInputTimeMs = 0;
static int dcdStatus = DEFAULT_DCD_INACTIVE;
static int pinDCD = DEFAULT_PIN_DCD;
static int pinCTS = DEFAULT_PIN_CTS;
static int pinRTS = DEFAULT_PIN_RTS;
static int pinDSR = DEFAULT_PIN_DSR;
static int pinDTR = DEFAULT_PIN_DTR;
static int pinOTH = DEFAULT_PIN_OTH;
static int pinRI = DEFAULT_PIN_RI;
static int dcdActive = DEFAULT_DCD_ACTIVE;
static int dcdInactive = DEFAULT_DCD_INACTIVE;
static int ctsActive = DEFAULT_CTS_ACTIVE;
static int ctsInactive = DEFAULT_CTS_INACTIVE;
static int rtsActive = DEFAULT_RTS_ACTIVE;
static int rtsInactive = DEFAULT_RTS_INACTIVE;
static int riActive = DEFAULT_RI_ACTIVE;
static int riInactive = DEFAULT_RI_INACTIVE;
static int dtrActive = DEFAULT_DTR_ACTIVE;
static int dtrInactive = DEFAULT_DTR_INACTIVE;
static int dsrActive = DEFAULT_DSR_ACTIVE;
static int dsrInactive = DEFAULT_DSR_INACTIVE;
static int othActive = DEFAULT_OTH_ACTIVE;
static int othInactive = DEFAULT_OTH_INACTIVE;

static int getDefaultCtsPin()
{
  return DEFAULT_PIN_CTS;
}

static void doNothing(const char* format, ...) 
{
}

static void s_pinWrite(uint8_t pinNo, uint8_t value)
{
  if(pinSupport[pinNo])
  {
    pinCache[pinNo] = value;
    digitalWrite(pinNo, value);
  }
}

static void setHostName(const char *hname)
{
  WiFi.hostname(hname);
}

static void setNewStaticIPs(IPAddress *ip, IPAddress *dns, IPAddress *gateWay, IPAddress *subNet)
{
  if(staticIP != null)
    free(staticIP);
  staticIP = ip;
  if(staticDNS != null)
    free(staticDNS);
  staticDNS = dns;
  if(staticGW != null)
    free(staticGW);
  staticGW = gateWay;
  if(staticSN != null)
    free(staticSN);
  staticSN = subNet;
}

static bool connectWifi(const char* ssid, const char* password, IPAddress *ip, IPAddress *dns, IPAddress *gateWay, IPAddress *subNet)
{
  while(WiFi.status() == WL_CONNECTED)
  {
    WiFi.disconnect();
    delay(100);
    yield();
  }
  if(hostname.length() > 0)
    setHostName(hostname.c_str());
  WiFi.mode(WIFI_STA);
  if((ip != null)&&(gateWay != null)&&(dns != null)&&(subNet!=null))
  {
    if(!WiFi.config(*ip,*gateWay,*subNet,*dns))
      return false;
  }
  WiFi.begin(ssid, password);
  if(hostname.length() > 0)
    setHostName(hostname.c_str());
  bool amConnected = (WiFi.status() == WL_CONNECTED) && (strcmp(WiFi.localIP().toString().c_str(), "0.0.0.0")!=0);
  int WiFiCounter = 0;
  while ((!amConnected) && (WiFiCounter < 20))
  {
    WiFiCounter++;
    if(!amConnected)
      delay(500);
    amConnected = (WiFi.status() == WL_CONNECTED) && (strcmp(WiFi.localIP().toString().c_str(), "0.0.0.0")!=0);
  }
  lastConnectAttempt = millis();
  if(lastConnectAttempt == 0)  // it IS possible for millis() to be 0, but we need to ignore it.
    lastConnectAttempt = 1; // 0 is a special case, so skip it

  if(!amConnected)
  {
    nextReconnectDelay = 0; // assume no retry is desired.. let the caller set it up, as it could be bad PW
    WiFi.disconnect();
  }
  else
    nextReconnectDelay = DEFAULT_RECONNECT_DELAY; // if connected, we always want to try reconns in the future

#if SUPPORT_LED_PINS
  s_pinWrite(DEFAULT_PIN_WIFI,(WiFi.status() == WL_CONNECTED)?DEFAULT_WIFI_ACTIVE:DEFAULT_WIFI_INACTIVE);
#endif
  if(WiFi.status() == WL_CONNECTED)
    debugPrintf("Connected to %s with IP %s.\r\n",ssid,WiFi.localIP().toString().c_str());
  return (WiFi.status() == WL_CONNECTED);
}

static void checkBaudChange()
{
  switch(baudState)
  {
    case BS_SWITCH_TEMP_NEXT:
      changeBaudRate(tempBaud);
      baudState = BS_SWITCHED_TEMP;
      break;
    case BS_SWITCH_NORMAL_NEXT:
      changeBaudRate(baudRate);
      baudState = BS_NORMAL;
      break;
    default:
      break;
  }
}

static void changeBaudRate(int baudRate)
{
  flushSerial(); // blocking, but very very necessary
  delay(500); // give the client half a sec to catch up
  logPrintfln("Baud change to %d.",baudRate);
  dequeSize=1+(baudRate/INTERNAL_FLOW_CONTROL_DIV);
  debugPrintf("Baud %d, Deque constant now: %d\r\n",baudRate,dequeSize);
  HWSerial.begin(baudRate, serialConfig);  //Change baud rate
#if SUPPORT_LED_PINS
  s_pinWrite(DEFAULT_PIN_HS,(baudRate>=DEFAULT_HS_BAUD)?DEFAULT_HS_ACTIVE:DEFAULT_HS_INACTIVE);
#endif  
}

static void changeSerialConfig(SerialConfig conf)
{
  flushSerial(); // blocking, but very very necessary
  delay(500); // give the client half a sec to catch up
  debugPrintf("Config changing to %dbps, %d.\r\n",baudRate,(int)conf);
  dequeSize=1+(baudRate/INTERNAL_FLOW_CONTROL_DIV);
  debugPrintf("Deque constant now: %d\r\n",dequeSize);
  HWSerial.begin(baudRate, conf);  //Change baud rate
  debugPrintf("Config changed.\r\n");
}

static int checkOpenConnections()
{
  int num=WiFiClientNode::getNumOpenWiFiConnections();
  if(num == 0)
  {
    if((dcdStatus == dcdActive)
    &&(dcdStatus != dcdInactive))
    {
      dcdStatus = dcdInactive;
      s_pinWrite(pinDCD,dcdStatus);
      if(baudState == BS_SWITCHED_TEMP)
        baudState = BS_SWITCH_NORMAL_NEXT;
      if(currMode == &commandMode)
        clearSerialOutBuffer();
    }
  }
  else
  {
    if((dcdStatus == dcdInactive)
    &&(dcdStatus != dcdActive))
    {
      dcdStatus = dcdActive;
      s_pinWrite(pinDCD,dcdStatus);
      if((tempBaud > 0) && (baudState == BS_NORMAL))
        baudState = BS_SWITCH_TEMP_NEXT;
    }
  }
  return num;
}

static int processPlusPlusPlus(uint8_t c)
{
  if(c<0)
    return 0;
  int plusOut = 0;
  if(c == commandMode.EC)
  {
    bool timeout = (millis()-lastInputTimeMs)>900;
    if(plussesInARow==0)
    {
      if(timeout)
         plussesInARow=1; // it begins!
      // else got a +, but too quick after last char, so keep at 0
    }
    else
    if(!timeout) // quick PLUS
    {
      if(plussesInARow<3)
        plussesInARow++;
      else
      {
        plusOut = plussesInARow; // sur-plus, so reject
        plussesInARow=0; // spamming plusses clears!
      }
    }
    else // plus long after timeout
    {
      plusOut = plussesInARow;
      plussesInARow=1;
    }
  }
  else
  if(plussesInARow>0)
  {
      plusOut = plussesInARow;
      plussesInARow=0;
  }
  lastInputTimeMs = millis();
  return plusOut;
}

static bool checkPlusPlusPlusEscape()
{
  if((plussesInARow == 3) && ((millis()-lastInputTimeMs)>900))
  {
    plussesInARow = 0;
    return true;
  }
  return false;
}

void setup() 
{
  for(int i=0;i<MAX_PIN_NO;i++)
    pinSupport[i]=false;
  // PerryFi: GPIO2 removed (boot strapping pin; DCD not used).
  // GPIO5 removed (old CTS location; not connected on PerryFi).
  // GPIO4 = CTS input, GPIO13 = RTS output (enabled via the 9-16 loop below).
  pinSupport[0]=true;
  if((ESP.getFlashChipRealSize()/1024)>=4096) // PerryFi uses ESP-12F (4MB flash)
  {
    pinSupport[4]=true;
    for(int i=9;i<=16;i++)
      pinSupport[i]=true;
    pinSupport[11]=false;
  }
#ifdef DEFAULT_PIN_OPB
  static bool OPB_stat=0;
  pinMode(DEFAULT_PIN_OPB, INPUT);
  OPB_stat=digitalRead(DEFAULT_PIN_OPB);
#endif

  debugPrintf("PerryZi %s firmware starting initialisation\r\n",PERRYZI_VERSION);
  currMode = &commandMode;
  if(!SPIFFS.begin())
  {
    SPIFFS.format();
    SPIFFS.begin();
    debugPrintf("SPIFFS Formatted.\r\n");
  }
  HWSerial.begin(DEFAULT_BAUD_RATE, DEFAULT_SERIAL_CONFIG);  //Start Serial
  HWSerial.setRxBufferSize(RX_BUFFER_SIZE);
  commandMode.loadConfig();
  PhoneBookEntry::loadPhonebook();
  dcdStatus = dcdInactive;
  s_pinWrite(pinDCD,dcdStatus);
  flushSerial();
#if SUPPORT_LED_PINS
  s_pinWrite(DEFAULT_PIN_WIFI,(WiFi.status() == WL_CONNECTED)?DEFAULT_WIFI_ACTIVE:DEFAULT_WIFI_INACTIVE);
  s_pinWrite(DEFAULT_PIN_HS,(baudRate>=DEFAULT_HS_BAUD)?DEFAULT_HS_ACTIVE:DEFAULT_HS_INACTIVE);
#endif
}

void checkReconnect()
{
  if((WiFi.status() != WL_CONNECTED)
  &&(nextReconnectDelay>0)
  &&(lastConnectAttempt>0)
  &&(wifiSSI.length()>0))
  {
     unsigned long now=millis();
     if(lastConnectAttempt > now)
       lastConnectAttempt=1;
     if(now > lastConnectAttempt + nextReconnectDelay)
     {
        debugPrintf("Attempting Reconnect to %s\r\n",wifiSSI.c_str());
        unsigned long oldReconnectDelay = nextReconnectDelay;
        if(!connectWifi(wifiSSI.c_str(),wifiPW.c_str(),staticIP,staticDNS,staticGW,staticSN))
          debugPrintf("Unable to reconnect to %s.\r\n",wifiSSI.c_str());
        nextReconnectDelay = oldReconnectDelay * 2;
        if(nextReconnectDelay > MAX_RECONNECT_DELAY)
          nextReconnectDelay = DEFAULT_RECONNECT_DELAY;
     }
  }
}

void checkFactoryReset()
{
}

void loop() 
{
  checkFactoryReset();
  checkReconnect();
  if(HWSerial.available())
  {
    currMode->serialIncoming();
  }
  currMode->loop();
  zclock.tick();
}
