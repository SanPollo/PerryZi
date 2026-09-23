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

// ***************************************************************************
// Global definitions - USER DEFINABLE OPTIONS START ON LINE 61!

#define PERRYZI_VERSION "1.2.4"
const char compile_date[] = __DATE__ " " __TIME__;
#define DEFAULT_NO_DELAY true
#define null 0
#define ZIMODEM_ESP8266

// Espressif AT command sets implemented by ESPRESSIF_CMDS
#define V1_7_6 176
#define V2_3_0 230

// Predefined Targets
#define GENERIC_TARGET 0
#define AGONLIGHT      1
#define PERRYFI1       2
#define PERRYFI2       3

// ESP LED modes (not available on MOD-WIFI-ESP8266)
#define rxtx            1 // serial traffic between the host and this module
#define network_traffic 2 // socket traffic to and from remote hosts

// PerryFi 1.0 pin configuration (WEMOS D1 mini)
#if defined(ARDUINO_ESP8266_WEMOS_D1MINI)
#define DEFAULT_PIN_RTS 15  // WEMOS D1 Mini: GPIO15 (D8) -> RTS
#define DEFAULT_PIN_CTS 13  // WEMOS D1 Mini: GPIO13 (D7) -> CTS

// PerryFi 2.1 pin configuration (ESP-12F)
#elif defined(ARDUINO_ESP8266_GENERIC)
#define DEFAULT_PIN_RTS 13  // ESP-12F PerryFi: GPIO13 -> RTS
#define DEFAULT_PIN_CTS 4   // ESP-12F PerryFi: GPIO4  -> CTS

// Prevent building on other boards.
#else
#error UNSUPPORTED BOARD: Please select "LOLIN(WEMOS) D1 mini" for the PerryFi 1.0, or "Generic ESP8266 Module" for the PerryFi 2.1 and MOD-WIFI-ESP8266.
#endif

// ***************************************************************************


// ***************************************************************************
// USER DEFINABLE OPTIONS START HERE

// Set TARGET_HOST depending on which module/board you are building for.
// Options: AGONLIGHT, GENERIC, PERRYFI, or PERRYFI2
#define TARGET_HOST PERRYFI2

#define DEFAULT_SERIAL_CONFIG SERIAL_8N1
#define RX_BUFFER_SIZE 4096

#define INCLUDE_IRCC true
#define INCLUDE_PING true
#define INCLUDE_FTP true
#define INCLUDE_SLIP true
#define INCLUDE_PPP true

#define ESPRESSIF_CMDS true // Espressif ESP-AT compatible command set
#define ESPRESSIF_VER V2_3_0 // V1_7_6 or V2_3_0
 
#define INTERNAL_LED rxtx // rxtx, network_traffic, or false (to disable)
#define INTERNAL_LED_HOLD_MS 30 // How long the LED stays lit after activity.

#define NTP_SERVER "uk.pool.ntp.org"

//#define SUPPORT_LED_PINS true // enable if you have the spare gpio pins and leds
#if SUPPORT_LED_PINS
#define DEFAULT_PIN_AA 35
#define DEFAULT_PIN_HS 34
#define DEFAULT_PIN_WIFI 26
#define DEFAULT_HS_BAUD 38400
#define DEFAULT_AA_ACTIVE LOW
#define DEFAULT_AA_INACTIVE HIGH
#define DEFAULT_HS_ACTIVE LOW
#define DEFAULT_HS_INACTIVE HIGH
#define DEFAULT_WIFI_ACTIVE LOW
#define DEFAULT_WIFI_INACTIVE HIGH
#endif

// Pins unused by most boards
#define DEFAULT_PIN_DCD -1
#define DEFAULT_PIN_DSR -1
#define DEFAULT_PIN_DTR -1
#define DEFAULT_PIN_OTH -1
#define DEFAULT_PIN_RI  -1

// END OF USER DEFINABLE OPTIONS (apart from GENERIC_TARGET below)
// ***************************************************************************


/* Target Host Definitions

As PerryZi supports multiple boards, it is necessary to define each target.

At the very least, each target must set:

     DEFAULT_BAUD_RATE  the speed the firmware comes up at
     MAX_BAUD_RATE      the limit for baud rate changes (0 to disable limit)

Other overrides can be set here, such as INTERNAL_LED and DEFAULT_PINs. The
GENERIC_TARGET can be modified to build for unsupported modules. Do not forget
to #undef anything previously defined. */

// ***************************************************************************
// GENERIC_TARGET: For unsupported ESP8266 modules
// ***************************************************************************
#if TARGET_HOST == GENERIC_TARGET
	#define DEFAULT_BAUD_RATE 9600
	#define MAX_BAUD_RATE 0
	#define INTERNAL_LED_PIN 2
// ***************************************************************************

// ***************************************************************************
// AGONLIGHT: MOD-WIFI-ESP8266 module for Agon Light / Neo6502
// ***************************************************************************
#elif TARGET_HOST == AGONLIGHT
	#if !defined(ARDUINO_ESP8266_GENERIC)
		#error AGONLIGHT requires the Generic ESP8266 Module board type
	#endif
	#define DEFAULT_BAUD_RATE 115200
	#define MAX_BAUD_RATE 0
	// Only LED on the MOD-WIFI-ESP8266 is hardwired to TX, and not
	// configurable.
	#undef INTERNAL_LED
	#define INTERNAL_LED false
	#undef INTERNAL_LED_HOLD_MS
	// The stock module on these machines runs the 1.x AT firmware, and the
	// software written for them depends on its command set.
	#undef ESPRESSIF_VER
	#define ESPRESSIF_VER V1_7_6
// ***************************************************************************

// ***************************************************************************
// PERRYFI1: Uses a real Z80 DART and 8253. In tests it was unstable at speeds
// over 9600 baud. However, some sources suggest that 19200 is possible with
// J17CPM3.EMS. In light of this, 9600 is the default, and 19200 is the cap.
// ***************************************************************************
#elif TARGET_HOST == PERRYFI1
	#if !defined(ARDUINO_ESP8266_WEMOS_D1MINI)
		#error PERRYFI1 requires the LOLIN(WEMOS) D1 R2 & mini board type
	#endif
	#define DEFAULT_BAUD_RATE 9600
	#define MAX_BAUD_RATE 19200
	#define INTERNAL_LED_PIN 2
// ***************************************************************************

// ***************************************************************************
// PERRYFI2: PicoDART should be capable of speeds at up to 115200, but only
// 38400 baud has been tested so far, so this is the current default.
// ***************************************************************************
#elif TARGET_HOST == PERRYFI2
	#if !defined(ARDUINO_ESP8266_GENERIC)
		#error PERRYFI2 requires the Generic ESP8266 Module board type
	#endif
	#define DEFAULT_BAUD_RATE 38400
	#define MAX_BAUD_RATE 0
	#define INTERNAL_LED_PIN 2
// ***************************************************************************

// Refuse to build if a valid TARGET_HOST is not specified.
#else
	#error UNSUPPORTED TARGET_HOST: choose GENERIC_TARGET, AGONLIGHT, PERRYFI1 or PERRYFI2
#endif

// Espressif compatibility checked here, so a target overriding ESPRESSIF_VER has already
// done so.
#ifdef ESPRESSIF_CMDS
	#if !defined(ESPRESSIF_VER)
		#error ESPRESSIF_CMDS requires ESPRESSIF_VER to be set to V1_7_6 or V2_3_0
	#elif (ESPRESSIF_VER != V1_7_6) && (ESPRESSIF_VER != V2_3_0)
		#error ESPRESSIF_VER must be V1_7_6 or V2_3_0
	#endif
#endif

/*
   Activity LED: ESP-12F / WEMOS D1 mini

   These modules have a blue LED on GPIO2, wired active LOW, so one code path
   serves both. GPIO2 is a boot strapping pin and must be HIGH at reset; it is
   therefore driven only from setup() onwards and is left HIGH (LED off) when
   idle. PerryZi already keeps GPIO2 out of pinSupport[], so AT&N and the
   S-register pin assignments cannot claim it.
   
   NOTE: GPIO2 is also UART1 TX. With the IDE's Debug port set to "Disabled"
   nothing else uses it, but selecting Serial1 as the debug port would put
   debug output and this LED on the same pin. The preprocessor cannot tell
   which port DEBUG_ESP_PORT names, so this cannot be caught at build time.
*/

#define DEFAULT_FCT FCT_DISABLED

#define debugPrintf doNothing //Serial.printf
#define preEOLN(...)
#define echoEOLN(...) serial.prints(EOLN)

#define DEFAULT_DCD_ACTIVE  LOW
#define DEFAULT_DCD_INACTIVE  HIGH
#define DEFAULT_CTS_ACTIVE  LOW
#define DEFAULT_CTS_INACTIVE  HIGH
#define DEFAULT_RTS_ACTIVE  LOW
#define DEFAULT_RTS_INACTIVE  HIGH
#define DEFAULT_RI_ACTIVE  LOW
#define DEFAULT_RI_INACTIVE  HIGH
#define DEFAULT_DSR_ACTIVE  LOW
#define DEFAULT_DSR_INACTIVE  HIGH
#define DEFAULT_DTR_ACTIVE  LOW
#define DEFAULT_DTR_INACTIVE  HIGH
#define DEFAULT_OTH_ACTIVE  LOW
#define DEFAULT_OTH_INACTIVE  HIGH

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

#if INTERNAL_LED
	// Declaration only (defined below the includes). wificlientnode.h needs
	// it, and it must not be a definition at this point.
	static inline void internalLedActivity();
#endif

// Call sites use the mode-specific names below, so each site reads the same
// whichever mode is selected, and the choice lives in one place.
#if INTERNAL_LED == rxtx
	#define internalLedSerialActivity() internalLedActivity()
#else
	#define internalLedSerialActivity()
#endif
#if INTERNAL_LED == network_traffic
	#define internalLedNetActivity() internalLedActivity()
#else
	#define internalLedNetActivity()
#endif

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
#ifdef ESPRESSIF_CMDS
	#include "espressif.h"
#endif

#if INCLUDE_SLIP
	#include "zslipmode.h"
#endif
#if INCLUDE_PPP
	#include "zpppmode.h"
#endif
#if INCLUDE_IRCC
	#include "zircmode.h"
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
#if INTERNAL_LED
static unsigned long internalLedLitAt = 0;
static volatile bool internalLedFlag = false;
static bool internalLedLit = false;

/* Records activity and nothing else.  This runs in the byte path, so it is
   deliberately a single store: taking millis() here cost a call plus 64-bit
   rollover arithmetic on every character.  The timestamp is read once per
   loop in internalLedTick() instead, which moves the same work from per-byte
   to per-loop.  The only visible difference is that the hold is timed from
   the loop pass rather than the exact byte, which at INTERNAL_LED_HOLD_MS is
   not observable.

   volatile so that the flag remains correct if a future mode ever sets it
   from an interrupt or SDK context. */
static inline void internalLedActivity()
{
  internalLedFlag = true;
}

// Drives the pin.  Unsigned subtraction keeps the elapsed test correct across
// the millis() rollover.
static void internalLedTick()
{
  if(internalLedFlag)
  {
    internalLedFlag = false;
    internalLedLitAt = millis();
    if(!internalLedLit)
    {
      digitalWrite(INTERNAL_LED_PIN, LOW);   // active low: lit
      internalLedLit = true;
    }
    return;
  }
  if(internalLedLit && ((millis() - internalLedLitAt) >= INTERNAL_LED_HOLD_MS))
  {
    digitalWrite(INTERNAL_LED_PIN, HIGH);    // active low: off
    internalLedLit = false;
  }
}
#else
#define internalLedActivity()
#define internalLedTick()
#endif

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
#if INTERNAL_LED
  // Drive the LED off first: GPIO2 must not be pulled low across a reset.
  pinMode(INTERNAL_LED_PIN, OUTPUT);
  digitalWrite(INTERNAL_LED_PIN, HIGH);
#endif
  for(int i=0;i<MAX_PIN_NO;i++)
    pinSupport[i]=false;
  // GPIO2 removed (boot strapping pin; DCD not used).
  // GPIO4 = CTS input, GPIO13 = RTS output (enabled via the 9-16 loop below).
  pinSupport[0]=true;
  if((ESP.getFlashChipRealSize()/1024)>=4096) // Generic ESP8266 module ESP-12F (4MB flash)
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
    internalLedSerialActivity();
    currMode->serialIncoming();
  }
  currMode->loop();
  internalLedTick();
  zclock.tick();
}