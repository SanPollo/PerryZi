#if INCLUDE_PING
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
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/ip.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/inet.h"


#include "lwip/raw.h"
static uint8_t ping_recv(void *pingtm, struct raw_pcb *pcb, pbuf *packet, const ip_addr_t *addr)
{
  unsigned long *tm = (unsigned long *)pingtm;
  *tm = millis();
  return false;
}

static int ping(char *host)
{
  IPAddress hostIp((uint32_t)0);
  if(!WiFi.hostByName(host, hostIp))
    return -1;
  int icmp_len = sizeof(struct icmp_echo_hdr);
  struct pbuf * packet = pbuf_alloc(PBUF_IP, 32 + icmp_len, PBUF_RAM);
  if(packet == nullptr)
    return 1;
  struct icmp_echo_hdr * pingRequest = (struct icmp_echo_hdr *)packet->payload;
  ICMPH_TYPE_SET(pingRequest, ICMP_ECHO);
  ICMPH_CODE_SET(pingRequest, 0);
  pingRequest->chksum = 0;
  pingRequest->id = 0x0100;
  pingRequest->seqno = htons(0);
  char dataByte = 'a';
  for(size_t i=0; i<32; i++)
  {
    ((char*)pingRequest)[icmp_len + i] = dataByte;
    if(++dataByte > 'w')
      dataByte = 'a';
  }
  pingRequest->chksum = inet_chksum(pingRequest,32+icmp_len);
  ip_addr_t dest_addr;
  dest_addr.addr = hostIp;
  struct raw_pcb *ping_pcb = raw_new(IP_PROTO_ICMP);

  unsigned long startTm = millis();
  unsigned long tm = 0;
  raw_recv(ping_pcb, ping_recv, (void *)&tm);
  raw_bind(ping_pcb, IP_ADDR_ANY);

  raw_sendto(ping_pcb, packet, &dest_addr);
  while((millis()-startTm < 1500) && (tm == 0))
    delay(1);
  pbuf_free(packet);
  raw_remove(ping_pcb);
  return (tm > 0)? (int)(millis()-tm) : -1;
}
#endif
