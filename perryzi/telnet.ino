/*
   PerryZi - ESP Firmware for the Perryfi
   
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


#define TELNET_BINARY 0 
  /** TELNET CODE: echo */
#define TELNET_ECHO 1 
  /** TELNET CODE: echo */
#define TELNET_LOGOUT 18 
  /** TELNET CODE: supress go ahead*/
#define TELNET_SUPRESS_GO_AHEAD 3 
  /** TELNET CODE: sending terminal type*/
#define TELNET_TERMTYPE 24 
  /** TELNET CODE: Negotiate About Window Size.*/
#define TELNET_NAWS 31 
  /** TELNET CODE: Remote Flow Control.*/
#define TELNET_TOGGLE_FLOW_CONTROL 33 
  /** TELNET CODE: Linemode*/
#define TELNET_LINEMODE 34 
  /** TELNET CODE: MSDP protocol*/
#define TELNET_MSDP 69 
  /** TELNET CODE: MSSP Server Status protocol*/
#define TELNET_MSSP 70 
  /** TELNET CODE: text compression, protocol 1*/
#define TELNET_COMPRESS 85 
  /** TELNET CODE: text compression, protocol 2*/
#define TELNET_COMPRESS2 86 
  /** TELNET CODE: MSP SOund protocol*/
#define TELNET_MSP 90 
  /** TELNET CODE: MXP Extended protocol*/
#define TELNET_MXP 91 
  /** TELNET CODE: AARD protocol*/
#define TELNET_AARD 102 
  /** TELNET CODE: End of subnegotiation parameters*/
#define TELNET_SE 240 
  /** TELNET CODE: Are You There*/
#define TELNET_AYT 246 
  /** TELNET CODE: Erase character*/
#define TELNET_EC 247 
  /** TELNET CODE: ATCP protocol*/
#define TELNET_ATCP 200 
  /** TELNET CODE: GMCP protocol*/
#define TELNET_GMCP 201 
  /** TELNET CODE: Indicates that what follows is subnegotiation of the indicated option*/
#define TELNET_SB 250 
  /** TELNET CODE: Indicates the desire to begin performing, or confirmation that you are now performing, the indicated option*/
#define TELNET_WILL 251 
  /** TELNET CODE: Indicates the refusal to perform, or continue performing, the indicated option*/
#define TELNET_WONT 252 
  /** TELNET CODE: Indicates the request that the other party perform, or confirmation that you are expecting the other party to perform, the indicated option*/
#define TELNET_DO 253 
  /** TELNET CODE: 253 doubles as fake ansi telnet code*/
#define TELNET_ANSI 253 
  /** TELNET CODE: Indicates the demand that the other party stop performing, or confirmation that you are no longer expecting the other party to perform, the indicated option.*/
#define TELNET_DONT 254 
  /** TELNET CODE: Indicates that the other party can go ahead and transmit -- I'm done.*/
#define TELNET_GA 249 
  /** TELNET CODE: Indicates that there is nothing to do?*/
#define TELNET_NOP 241 
  /** TELNET CODE: IAC*/
#define TELNET_IAC 255 


uint8_t streamAvailRead(Stream *stream)
{
  int ct=0;
  while((stream->available()==0)
  &&(ct++)<250)
    delay(1);
  return stream->read();
}

bool handleAsciiIAC(char *c, Stream *stream)
{
  if(*c == 255)
  {
    *c=streamAvailRead(stream);
    logSocketIn(*c);
    if(*c==TELNET_IAC)
    {
      *c = 255;
      return true;
    }
    if(*c==TELNET_WILL)
    {
      char what=streamAvailRead(stream);
      logSocketIn(what);
      uint8_t iacDont[] = {TELNET_IAC, TELNET_DONT, what};
      if(what == TELNET_TERMTYPE)
        iacDont[1] = TELNET_DO;
      for(int i=0;i<3;i++)
        logSocketOut(iacDont[i]);
      stream->write(iacDont,3);
      return false;
    }
    if(*c==TELNET_DONT)
    {
      char what=streamAvailRead(stream);
      logSocketIn(what);
      uint8_t iacWont[] = {TELNET_IAC, TELNET_WONT, what};
      for(int i=0;i<3;i++)
        logSocketOut(iacWont[i]);
      stream->write(iacWont,3);
      return false;
    }
    if(*c==TELNET_WONT)
    {
      char what=streamAvailRead(stream);
      logSocketIn(what);
      return false;
    }
    if(*c==TELNET_DO)
    {
      char what=streamAvailRead(stream);
      logSocketIn(what);
      uint8_t iacWont[] = {TELNET_IAC, TELNET_WONT, what};
      if(what == TELNET_TERMTYPE)
        iacWont[1] = TELNET_WILL;
      for(int i=0;i<3;i++)
        logSocketOut(iacWont[i]);
      stream->write(iacWont,3);
      return false;
    }
    if(*c==TELNET_SB)
    {
      char what=streamAvailRead(stream);
      logSocketIn(what);
      char lastC=*c;
      while(((lastC!=TELNET_IAC)||(*c!=TELNET_SE))&&(*c>=0))
      {
        lastC=*c;
        *c=streamAvailRead(stream);
        logSocketIn(*c);
      }
      if(what == TELNET_TERMTYPE)
      {
        int respLen = termType.length() + 6;
        uint8_t buf[respLen];
        buf[0]=TELNET_IAC;
        buf[1]=TELNET_SB;
        buf[2]=TELNET_TERMTYPE;
        buf[3]=(uint8_t)0;
        sprintf((char *)buf+4,termType.c_str());
        buf[respLen-2]=TELNET_IAC;
        buf[respLen-1]=TELNET_SE;
        for(int i=0;i<respLen;i++)
          logSocketOut(buf[i]);
        stream->write(buf,respLen);
        return false;
      }
    }
    return false;
  }
  return true;
}
