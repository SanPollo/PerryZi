/*
   PerryZi - ESP8266 Firmware for the PerryFi

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

#include "znist.h"
extern RealTimeClock zclock;

String znistGetDateTimeUTC()
{
  DateTimeClock &c=zclock.getCurrentTime();
  if(c.getYear() < 2020)
    return String("NO TIME");
  char buf[32];
  sprintf(buf,"%02d-%02d-%02d %02d:%02d:%02d",
          (int)(c.getYear()%100),
          (int)c.getMonth(),
          (int)c.getDay(),
          (int)c.getHour(),
          (int)c.getMinute(),
          (int)c.getSecond());
  return String(buf);
}
