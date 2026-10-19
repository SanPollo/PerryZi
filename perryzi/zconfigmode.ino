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

void ZConfig::switchTo()
{
  debugPrintf("\r\nMode:Config\r\n");
  currMode=&configMode;
  serial.setFlowControlType(commandMode.serial.getFlowControlType());
  savedEcho=commandMode.doEcho;
  newListen=commandMode.preserveListeners;
  commandMode.doEcho=true;
  serverSpec.port=6502;
  serverSpec.flagsBitmap=commandMode.getConfigFlagBitmap() & (~FLAG_ECHO);
  if(servs)
    serverSpec = *servs;
  serial.setXON(true);
  showMenu=true;
  EOLN=commandMode.EOLN;
  EOLNC=EOLN.c_str();
  currState = ZCFGMENU_MAIN;
  lastNumber=0;
  lastAddress="";
  lastOptions="";
  settingsChanged=false;
  lastNumNetworks=0;
  lastIP=(staticIP != null)?*staticIP:WiFi.localIP();
  lastDNS=(staticDNS != null)?*staticDNS:((WiFi.dnsIP()==IPAddress(0,0,0,0))?IPAddress(192,168,0,1):WiFi.dnsIP());
  lastGW=(staticGW != null)?*staticGW:((WiFi.gatewayIP()==IPAddress(0,0,0,0))?IPAddress(192,168,0,1):WiFi.gatewayIP());
  lastSN=(staticSN != null)?*staticSN:((WiFi.subnetMask()==IPAddress(0,0,0,0))?IPAddress(255,255,255,0):WiFi.subnetMask());
}

void ZConfig::serialIncoming()
{
  bool crReceived=commandMode.readSerialStream();
  if(crReceived)
  {
    doModeCommand();
  }
}

void ZConfig::switchBackToCommandMode()
{
  debugPrintf("\r\nMode:Command\r\n");
  commandMode.doEcho=savedEcho;
  currMode = &commandMode;
}

void ZConfig::doModeCommand()
{
  String cmd = commandMode.getNextSerialCommand();
  char c='?';
  char sc='?';
  for(int i=0;i<cmd.length();i++)
  {
    if(cmd[i]>32)
    {
      c=lc(cmd[i]);
      if((i<cmd.length()-1)
      &&(cmd[i+1]>32))
        sc=lc(cmd[i+1]);
      break;
    }
  }
  switch(currState)
  {
    case ZCFGMENU_MAIN:
    {
      if((c=='q')||(cmd.length()==0))
      {
        if(settingsChanged)
        {
          currState=ZCFGMENU_WICONFIRM;
          showMenu=true;
        }
        else
        {
          commandMode.showInitMessage();
          switchBackToCommandMode();
          return;
        }
      }
      else
      if(c=='a') // add to phonebook
      {
        currState=ZCFGMENU_NUM;
        showMenu=true;
      }
      else
      if(c=='w') // wifi
      {
        currState=ZCFGMENU_WIMENU;
        showMenu=true;
      }
      else
      if(c=='h') // host
      {
        currState=ZCFGMENU_NEWHOST;
        showMenu=true;
      }
      else
      if(c=='f') // flow control
      {
        currState=ZCFGMENU_FLOW;
        showMenu=true;
      }
      else
      if((c=='p')&&(sc=='r')) // print spec
      {
        currState=ZCFGMENU_NEWPRINT;
        showMenu=true;
      }
      else
      if(c=='e') // echo
      {
        savedEcho = !savedEcho;
        settingsChanged=true;
        showMenu=true;
      }
      else
      if(c=='b') // bbs
      {
        currState=ZCFGMENU_BBSMENU;
        showMenu=true;
      }
      else
      if(c>47 && c<58) // its a phonebook entry!
      {
        PhoneBookEntry *pb=PhoneBookEntry::findPhonebookEntry(cmd);
        if(pb == null)
        {
          serial.printf(F("%s%sPhone number not found: '%s'.%s%s"),EOLNC,EOLNC,cmd.c_str(),EOLNC,EOLNC);
          currState=ZCFGMENU_MAIN;
          showMenu=true;
        }
        else
        {
          lastNumber = pb->number;
          lastAddress = pb->address;
          lastOptions = pb->modifiers;
          lastNotes = pb->notes;
          currState=ZCFGMENU_ADDRESS;
          showMenu=true;
        }
      }
      else
      {
        showMenu=true; // re-show the menu
      }
      break;
    }
    case ZCFGMENU_WICONFIRM:
    {
      if((cmd.length()==0)||(c=='n'))
      {
        commandMode.showInitMessage();
        switchBackToCommandMode();
        return;
      }
      else
      if(c=='y')
      {
        if(newListen != commandMode.preserveListeners)
        {
          commandMode.preserveListeners=newListen;
          if(!newListen)
          {
            SPIFFS.remove("/zlisteners.txt");
            WiFiServerNode::DestroyAllServers();
          }
          else
          {
            commandMode.ringCounter=1;
            commandMode.autoStreamMode=true;
            WiFiServerNode *s=WiFiServerNode::FindServer(serverSpec.port);
            if(s != null)
              delete s;
            s = new WiFiServerNode(serverSpec.port,serverSpec.flagsBitmap);
            WiFiServerNode::SaveWiFiServers();
          }
        }
        else
        if(commandMode.preserveListeners)
        {
          WiFiServerNode *s = WiFiServerNode::FindServer(serverSpec.port);
          if( s != null)
          {
            if(s->flagsBitmap != serverSpec.flagsBitmap)
            {
              s->flagsBitmap = serverSpec.flagsBitmap;
            }
          }
          else
          {
            WiFiServerNode::DestroyAllServers();
            s = new WiFiServerNode(serverSpec.port,serverSpec.flagsBitmap);
            WiFiServerNode::SaveWiFiServers();
            commandMode.updateAutoAnswer();
          }
        }
        if(!commandMode.reSaveConfig(3))
          serial.printf(F("%sFailed to save Settings.%s"),EOLNC,EOLNC);
        else
          serial.printf(F("%sSettings saved.%s"),EOLNC,EOLNC);
        commandMode.showInitMessage();
        WiFiServerNode::SaveWiFiServers();
        switchBackToCommandMode();
        return;
      }
      else
        showMenu=true;
      break;
    }
    case ZCFGMENU_NUM:
    {
      PhoneBookEntry *pb=PhoneBookEntry::findPhonebookEntry(cmd);
      if(pb != null)
      {
        serial.printf(F("%s%sNumber already exists '%s'.%s%s"),EOLNC,EOLNC,cmd.c_str(),EOLNC,EOLNC);
        currState=ZCFGMENU_MAIN;
        showMenu=true;
      }
      else
      if(cmd.length()==0)
      {
        currState=ZCFGMENU_MAIN;
        showMenu=true;
      }
      else
      {
        lastNumber = atol((char *)cmd.c_str());
        lastAddress = "";
        int flagsBitmap = commandMode.getConfigFlagBitmap();
        flagsBitmap = flagsBitmap & (~FLAG_ECHO);
        ConnSettings flags(flagsBitmap);
        lastOptions = flags.getFlagString();
        lastNotes = "";
        currState=ZCFGMENU_ADDRESS;
        showMenu=true;
      }
      break;
    }
    case ZCFGMENU_NEWPORT:
    {
      if(cmd.length()>0)
      {
        serverSpec.port = atoi((char *)cmd.c_str());
        settingsChanged=true;
      }
      currState=ZCFGMENU_BBSMENU;
      showMenu=true;
      break;
    }
    case ZCFGMENU_ADDRESS:
    {
      PhoneBookEntry *entry = PhoneBookEntry::findPhonebookEntry(lastNumber);
      if(cmd.equalsIgnoreCase("delete") && (entry != null))
      {
        delete entry;
        currState=ZCFGMENU_MAIN;
        serial.printf(F("%sPhonebook entry deleted.%s%s"),EOLNC,EOLNC,EOLNC);
      }
      else
      if((cmd.length()==0) && (entry != null))
          currState=ZCFGMENU_NOTES; // just keep old values
      else
      {
        if(!validateHostInfo((uint8_t *)cmd.c_str()))
          serial.printf(F("%sInvalid address format (hostname:port) or (user:pass@hostname:port) for '%s'.%s%s"),EOLNC,cmd.c_str(),EOLNC,EOLNC);
        else
        {
          lastAddress = cmd;
          currState=ZCFGMENU_NOTES;
          if(lastAddress.indexOf("@")>0)
          {
            int x = lastOptions.indexOf("S"); 
            if(x<0)
              lastOptions += "S";
            else
              lastOptions = lastOptions.substring(0,x) + lastOptions.substring(x+1);
          }
        }
      }
      showMenu=true; // re-show the menu
      break;
    }
    case ZCFGMENU_BBSMENU:
    {
      if(cmd.length()==0)
        currState=ZCFGMENU_MAIN;
      else
      {
        ConnSettings flags(serverSpec.flagsBitmap);
        switch(c)
        {
        case 'h':
          currState=ZCFGMENU_NEWPORT;
          break;
        case 'd':
          newListen=false;
          settingsChanged=true;
          break;
        case 't': 
          if(newListen)
          {
            flags.telnet=!flags.telnet;
            settingsChanged=true;
          }
          break;
        case 'e':
          if(newListen)
            flags.echo=!flags.echo;
          else
            newListen=true;
          settingsChanged=true;
          break;
        case 'f':
          if(newListen)
          {
            if(flags.xonxoff)
            {
              flags.xonxoff=false;
              flags.rtscts=true; 
            }
            else
            if(flags.rtscts)
              flags.rtscts=false;
            else
              flags.xonxoff=true;
            settingsChanged=true;
          }
          break;
        case 's': 
          if(newListen)
          {
            flags.secure=!flags.secure;
            settingsChanged=true;
          }
          break;
         
        default:
          serial.printf(F("%sInvalid option '%s'.%s%s"),EOLNC,cmd.c_str(),EOLNC,EOLNC);
          break;
        }
        settingsChanged=true;
        serverSpec.flagsBitmap = flags.getBitmap();
      }
      showMenu=true;
      break;
    }
    case ZCFGMENU_SUBNET:
    {
      if(cmd.length()==0)
        currState=ZCFGMENU_NETMENU;
      else
      {
        IPAddress *newaddr = ConnSettings::parseIP(cmd.c_str());
        if(newaddr==null)
          serial.printf(F("%sBad address (%s).%s"),EOLNC,cmd.c_str(),EOLNC);
        else
        {
          currState=ZCFGMENU_NETMENU;
          switch(netOpt)
          {
          case 'i': lastIP = *newaddr; break;
          case 'd': lastDNS = *newaddr; break;
          case 'g': lastGW = *newaddr; break;
          case 's': lastSN = *newaddr; break;
          }
          delete newaddr;
        }
      }
      showMenu=true;
      break;
    }
    case ZCFGMENU_NETMENU:
    {
      if(cmd.length()==0)
      {
        currState=ZCFGMENU_MAIN;
        if((staticIP==null)&&(useDHCP))
        {}
        else
        if(useDHCP)
        {
          setNewStaticIPs(null,null,null,null);
          if(!connectWifi(wifiSSI.c_str(),wifiPW.c_str(),staticIP,staticDNS,staticGW,staticSN))
            serial.printf(F("%sUnable to connect to %s. :(%s"),EOLNC,wifiSSI.c_str(),EOLNC);
          settingsChanged=true;
        }
        else
        if((staticIP==null)
        ||(*staticIP != lastIP)
        ||(*staticDNS != lastDNS)
        ||(*staticGW != lastGW)
        ||(*staticSN != lastSN))
        {
          IPAddress *ip=new IPAddress();
          *ip = lastIP;
          IPAddress *dns=new IPAddress();
          *dns = lastDNS;
          IPAddress *gw=new IPAddress();
          *gw = lastGW;
          IPAddress *sn=new IPAddress();
          *sn = lastSN;
          setNewStaticIPs(ip,dns,gw,sn);
          if(!connectWifi(wifiSSI.c_str(),wifiPW.c_str(),staticIP,staticDNS,staticGW,staticSN))
            serial.printf(F("%sUnable to connect to %s. :(%s"),EOLNC,wifiSSI.c_str(),EOLNC);
          settingsChanged=true;
        }
      }
      else
      if(useDHCP)
      {
        if(c=='d')
          useDHCP=false;
      }
      else
      {
        switch(c)
        {
        case 'e':
          useDHCP=true;
          break;
        case 'i':
        case 'd': 
        case 'g':
        case 's':
          netOpt=c;
          currState=ZCFGMENU_SUBNET;
          break;
        default:
          serial.printf(F("%sInvalid option '%s'.%s%s"),EOLNC,cmd.c_str(),EOLNC,EOLNC);
          break;
        }
      }
      showMenu=true;
      break;
    }
    case ZCFGMENU_NOTES:
    {
      if(cmd.length()>0)
        lastNotes=cmd;
      currState=ZCFGMENU_OPTIONS;
      showMenu=true; // re-show the menu
      break;
    }
    case ZCFGMENU_OPTIONS:
    {
      if(cmd.length()==0)
      {
        PhoneBookEntry *entry = PhoneBookEntry::findPhonebookEntry(lastNumber);
        if(entry != null)
        {
          serial.printf(F("%sPhonebook entry updated.%s%s"),EOLNC,EOLNC,EOLNC);
          delete entry;
        }
        else
          serial.printf(F("%sPhonebook entry added.%s%s"),EOLNC,EOLNC,EOLNC);
        entry = new PhoneBookEntry(lastNumber,lastAddress.c_str(),lastOptions.c_str(),lastNotes.c_str());
        PhoneBookEntry::savePhonebook();
        currState=ZCFGMENU_MAIN;
      }
      else
      {
        ConnSettings flags(lastOptions.c_str());
        switch(c)
        {
          case 't': 
            flags.telnet=!flags.telnet;
            break;
          case 'e': 
            flags.echo=!flags.echo;
            break;
          case 'f':
            if(flags.xonxoff)
            {
              flags.xonxoff=false;
              flags.rtscts=true; 
            }
            else
            if(flags.rtscts)
              flags.rtscts=false;
            else
              flags.xonxoff=true;
            break;
          case 's': 
            flags.secure=!flags.secure;
            break;
          default:
            serial.printf(F("%sInvalid toggle option '%s'.%s%s"),EOLNC,cmd.c_str(),EOLNC,EOLNC);
            break;
        }
        lastOptions = flags.getFlagString();
      }
      showMenu=true; // re-show the menu
      break;
    }
    case ZCFGMENU_WIMENU:
    {
      if(cmd.length()==0)
      {
        currState=ZCFGMENU_MAIN;
        showMenu=true;
      }
      else
      {
        int num=atoi(cmd.c_str());
        if((num<=0)||(num>lastNumNetworks))
            serial.printf(F("%sInvalid number.  Try again.%s"),EOLNC,EOLNC);
        else
        if(WiFi.encryptionType(num-1) == ENC_TYPE_NONE)
        {
          if(!connectWifi(WiFi.SSID(num-1).c_str(),"",null,null,null,null))
          {
            serial.printf(F("%sUnable to connect to %s. :(%s"),EOLNC,WiFi.SSID(num-1).c_str(),EOLNC);
          }
          else
          {
            wifiSSI=WiFi.SSID(num-1);
            wifiPW="";
            settingsChanged=true;
            serial.printf(F("%sConnected!%s"),EOLNC,EOLNC);
            currState=ZCFGMENU_NETMENU;
          }
          showMenu=true;
        }
        else
        {
          lastNumber=num-1;
          currState=ZCFGMENU_WIFIPW;
          showMenu=true;
        }
      }
      break;
    }
    case ZCFGMENU_NEWHOST:
      if(cmd.length()==0)
      {
        // Show the menu being moved to; it was entered without being shown.
        currState=ZCFGMENU_WIMENU;
        showMenu=true;
      }
      else
      {
        hostname=cmd;
        hostname.replace(',','.');
        if((wifiSSI.length() > 0) && (WiFi.status()==WL_CONNECTED))
        {
            if(!connectWifi(wifiSSI.c_str(),wifiPW.c_str(),staticIP,staticDNS,staticGW,staticSN))
              serial.printf(F("%sUnable to connect to %s. :(%s"),EOLNC,wifiSSI.c_str(),EOLNC);
            settingsChanged=true;
        }
        currState=ZCFGMENU_MAIN;
        showMenu=true;
      }
      break;
    case ZCFGMENU_NEWPRINT:
      if(cmd.length()>0)
      {
        if(!printMode.testPrinterSpec(cmd.c_str(),cmd.length()))
        {
          serial.printf(F("%sBad format. Try ?:<host>:<port>/<path>.%s"),EOLNC,EOLNC);
          serial.printf(F("? = A)scii or R)aw.%s"),EOLNC);
          serial.printf(F("Example: R:192.168.1.71:631/ipp/printer%s"),EOLNC);
        }
        else
        {
          printMode.setLastPrinterSpec(cmd.c_str());
          settingsChanged=true;
        }
      }
      currState=ZCFGMENU_MAIN;
      showMenu=true;
      break;
    case ZCFGMENU_WIFIPW:
      if(cmd.length()==0)
      {
        currState=ZCFGMENU_WIMENU;
        showMenu=true;
      }
      else
      {
          for(int i=0;i<500;i++)
          {
            if(serial.isSerialOut())
              serialOutDeque();
            delay(1);
          }
          if(!connectWifi(WiFi.SSID(lastNumber).c_str(),cmd.c_str(),null,null,null,null))
            serial.printf(F("%sUnable to connect to %s.%s"),EOLNC,WiFi.SSID(lastNumber).c_str(),EOLNC);
          else
          {
            //setNewStaticIPs(null,null,null,null);
            useDHCP=(staticIP==null);
            lastIP=(staticIP != null)?*staticIP:WiFi.localIP();
            lastDNS=(staticDNS != null)?*staticDNS:((WiFi.dnsIP()==IPAddress(0,0,0,0))?IPAddress(192,168,0,1):WiFi.dnsIP());
            lastGW=(staticGW != null)?*staticGW:((WiFi.gatewayIP()==IPAddress(0,0,0,0))?IPAddress(192,168,0,1):WiFi.gatewayIP());
            lastSN=(staticSN != null)?*staticSN:((WiFi.subnetMask()==IPAddress(0,0,0,0))?IPAddress(255,255,255,0):WiFi.subnetMask());
            wifiSSI=WiFi.SSID(lastNumber);
            wifiPW=cmd;
            settingsChanged=true;
            currState=ZCFGMENU_NETMENU;
          }
          showMenu=true;
      }
      break;
    case ZCFGMENU_FLOW:
      if(cmd.length()==0)
      {
        currState=ZCFGMENU_WIMENU;
        showMenu=true;
      }
      else
      {
        currState=ZCFGMENU_MAIN;
        showMenu=true;
        if(c=='x')
          commandMode.serial.setFlowControlType(FCT_NORMAL);
        else
        if(c=='r')
          commandMode.serial.setFlowControlType(FCT_RTSCTS);
        else
        if(c=='d')
          commandMode.serial.setFlowControlType(FCT_DISABLED);
        else
        {
          serial.printf(F("%sUnknown flow control type '%s'.  Try again.%s"),EOLNC,cmd.c_str(),EOLNC);
          currState=ZCFGMENU_FLOW;
        }
        settingsChanged = settingsChanged || (currState ==ZCFGMENU_MAIN);
        serial.setFlowControlType(commandMode.serial.getFlowControlType());
        serial.setXON(true);
      }
      break;
  }
}

void ZConfig::loop()
{
  if(showMenu)
  {
    showMenu=false;
    switch(currState)
    {
      case ZCFGMENU_MAIN:
      {
        serial.printf(F("%sMain Menu%s"),EOLNC,EOLNC);
        serial.printf(F("[HOST] name: %s%s"),hostname.c_str(),EOLNC);
        serial.printf(F("[WIFI] connection: %s%s"),(WiFi.status() == WL_CONNECTED)?wifiSSI.c_str():"Not connected",EOLNC);
        String flowName;
        switch(commandMode.serial.getFlowControlType())
        {
          case FCT_NORMAL:
            flowName = "XON/XOFF";
            break;
          case FCT_RTSCTS:
            flowName = "RTS/CTS";
            break;
          case FCT_DISABLED:
            flowName = "DISABLED";
            break;
          default:
            flowName = "OTHER";
            break;
        }
        String bbsMode = "DISABLED";
        if(newListen)
        {
          bbsMode = "Port ";
          bbsMode += serverSpec.port;
        }
        serial.printf(F("[FLOW] control: %s%s"),flowName.c_str(),EOLNC);
        serial.printf(F("[ECHO] keystrokes: %s%s"),savedEcho?"ON":"OFF",EOLNC);
        serial.printf(F("[BBS] host: %s%s"),bbsMode.c_str(),EOLNC);
        serial.printf(F("[PRINT] spec: %s%s"),printMode.getLastPrinterSpec(),EOLNC);
        serial.printf(F("[ADD] new phonebook entry%s"),EOLNC);
        PhoneBookEntry *p = phonebook;
        if(p != null)
        {
          serial.printf(F("Phonebook entries:%s"),EOLNC);
          while(p != null)
          {
            if(strlen(p->notes)>0)
              serial.printf(F("  [%lu] %s (%s)%s"),p->number, p->address, p->notes, EOLNC);
            else
              serial.printf(F("  [%lu] %s%s"),p->number, p->address, EOLNC);
            p=p->next;
          }
        }
        serial.printf(F("%sEnter command or entry or ENTER to exit: "),EOLNC,EOLNC);
        break;
      }
      case ZCFGMENU_NUM:
        serial.printf(F("%sEnter a new fake phone number (digits ONLY)%s: "),EOLNC,EOLNC);
        break;
      case ZCFGMENU_NEWPORT:
        serial.printf(F("%sEnter a port number to listen on%s: "),EOLNC,EOLNC);
        break;
      case ZCFGMENU_ADDRESS:
      {
        PhoneBookEntry *lastEntry = PhoneBookEntry::findPhonebookEntry(lastNumber);
        if(lastEntry == null)
          serial.printf(F("%sEnter hostname:port, or user:pass@hostname:port for SSH%s: "),EOLNC,EOLNC);
        else
          serial.printf(F("%sModify address, or enter DELETE (%s)%s: "),EOLNC,lastAddress.c_str(),EOLNC);
        break;
      }
      case ZCFGMENU_OPTIONS:
      {
        ConnSettings flags(lastOptions.c_str());
        serial.printf(F("%sConnection Options:%s"),EOLNC,EOLNC);
        serial.printf(F("[TELNET] Translation: %s%s"),flags.telnet?"ON":"OFF",EOLNC);
        serial.printf(F("[ECHO]: %s%s"),flags.echo?"ON":"OFF",EOLNC);
        serial.printf(F("[FLOW] Control: %s%s"),flags.xonxoff?"XON/XOFF":flags.rtscts?"RTS/CTS":"DISABLED",EOLNC);
        serial.printf(F("[SSH/SSL] Security: %s%s"),flags.secure?"ON":"OFF",EOLNC);
        serial.printf(F("%sEnter option to toggle or ENTER to exit%s: "),EOLNC,EOLNC);
        break;
      }
      case ZCFGMENU_NOTES:
      {
        serial.printf(F("%sEnter some notes for this entry (%s)%s: "),EOLNC,lastNotes.c_str(),EOLNC);
        break;
      }
      case ZCFGMENU_BBSMENU:
      {
        serial.printf(F("%sBBS host settings:%s"),EOLNC,EOLNC);
        if(newListen)
        {
          ConnSettings flags(serverSpec.flagsBitmap);
          serial.printf(F("%s[HOST] Listener Port: %d%s"),EOLNC,serverSpec.port,EOLNC);
          serial.printf(F("[TELNET] Translation: %s%s"),flags.telnet?"ON":"OFF",EOLNC);
          serial.printf(F("[ECHO]: %s%s"),flags.echo?"ON":"OFF",EOLNC);
          serial.printf(F("[FLOW] Control: %s%s"),flags.xonxoff?"XON/XOFF":flags.rtscts?"RTS/CTS":"DISABLED",EOLNC);
          serial.printf(F("[DISABLE] BBS host listener%s"),EOLNC);
        }
        else
          serial.printf(F("%s[ENABLE] BBS host listener%s"),EOLNC,EOLNC);
        serial.printf(F("%sEnter option to toggle or ENTER to exit%s: "),EOLNC,EOLNC);
        break;
      }
      case ZCFGMENU_SUBNET:
      {
        String str;
        switch(netOpt)
        {
        case 'i':
        {
          ConnSettings::IPtoStr(&lastIP,str);
          serial.printf(F("%sIP Address (%s)%s: %s"),EOLNC,str.c_str(),EOLNC,EOLNC);
          break;
        }
        case 'g':
        {
          ConnSettings::IPtoStr(&lastGW,str);
          serial.printf(F("%sGateway Address (%s)%s: %s"),EOLNC,str.c_str(),EOLNC,EOLNC);
          break;
        }
        case 's':
        {
          ConnSettings::IPtoStr(&lastSN,str);
          serial.printf(F("%sSubnet Mask Address (%s)%s: %s"),EOLNC,str.c_str(),EOLNC,EOLNC);
          break;
        }
        case 'd':
        {
          ConnSettings::IPtoStr(&lastDNS,str);
          serial.printf(F("%sDNS Address (%s)%s: %s"),EOLNC,str.c_str(),EOLNC,EOLNC);
          break;
        }
        }
        break;
      }
      case ZCFGMENU_NETMENU:
      {
        serial.printf(F("%sNetwork settings:%s"),EOLNC,EOLNC);
        if(!useDHCP)
        {
          String str;
          ConnSettings::IPtoStr(&lastIP,str);
          serial.printf(F("%s[IP]: %s%s"),EOLNC,str.c_str(),EOLNC);
          ConnSettings::IPtoStr(&lastSN,str);
          serial.printf(F("[SUBNET]: %s%s"),str.c_str(),EOLNC);
          ConnSettings::IPtoStr(&lastGW,str);
          serial.printf(F("[GATEWAY]: %s%s"),str.c_str(),EOLNC);
          ConnSettings::IPtoStr(&lastDNS,str);
          serial.printf(F("[DNS]: %s%s"),str.c_str(),EOLNC);
          serial.printf(F("[ENABLE] Enable DHCP%s"),EOLNC);
        }
        else
          serial.printf(F("%s[DISABLE] DHCP%s"),EOLNC,EOLNC);
        serial.printf(F("%sEnter option to toggle or ENTER to exit%s: "),EOLNC,EOLNC);
        break;
      }
      case ZCFGMENU_WIMENU:
      {
        int n = WiFi.scanNetworks();
        if(n>20)
          n=20;
        serial.printf(F("%sWiFi Networks:%s"),EOLNC,EOLNC);
        lastNumNetworks=n;
        for (int i = 0; i < n; ++i)
        {
          serial.printf(F("[%d] "),(i+1));
          serial.prints(WiFi.SSID(i).c_str());
          serial.prints(F(" ("));
          serial.printi(WiFi.RSSI(i));
          serial.prints(F(")"));
          serial.prints((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" ":"*");
          serial.prints(EOLN.c_str());
          serial.flush();
          delay(10);
        }
        serial.printf(F("%sEnter number to connect, or ENTER: "),EOLNC);
        break;
      }
      case ZCFGMENU_NEWHOST:
      {
        serial.printf(F("%sEnter a new hostname: "),EOLNC);
        break;
      }
      case ZCFGMENU_NEWPRINT:
      {
        serial.printf(F("%sEnter ipp printer spec (?:<host>:<port>/path)%s: "),EOLNC,EOLNC);
        break;
      }
      case ZCFGMENU_WIFIPW:
      {
        serial.printf(F("%sEnter your WiFi Password: "),EOLNC);
        break;
      }
      case ZCFGMENU_FLOW:
      {
        serial.printf(F("%sEnter RTS/CTS, XON/XOFF, or DISABLE flow control%s: "),EOLNC,EOLNC);
        break;
      }
      case ZCFGMENU_WICONFIRM:
      {
        serial.printf(F("%sYour settings changed. Save (y/N)?"),EOLNC);
        break;
      }
    }
  }
  if(checkPlusPlusPlusEscape())
  {
    switchBackToCommandMode();
  }
  else
  if(serial.isSerialOut())
  {
    serialOutDeque();
  }
  logFileLoop();
}

