/* Host test for l2rewrite.c:
 *   cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/l2t l2rewrite_test.c ../main/l2rewrite.c && /tmp/l2t
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "l2rewrite.h"
static const uint8_t STA[6]={0x24,0x6f,0x28,1,2,3}, DEV[6]={0x00,0x11,0x22,0x33,0x44,0x55}, OTHER[6]={0x00,0x11,0x22,0x33,0x44,0x66};
static uint16_t ref_csum(const uint8_t*f){ /* independent UDP checksum over a byte copy */
  const uint8_t*ip=f+14,*u=ip+20; size_t ul=(u[4]<<8)|u[5]; uint8_t b[700]; size_t n=0;
  memcpy(b,ip+12,8); n=8; b[n++]=0; b[n++]=17; b[n++]=u[4]; b[n++]=u[5];
  memcpy(b+n,u,ul); b[n+6]=b[n+7]=0; n+=ul; if(n&1) b[n++]=0;
  uint32_t s=0; for(size_t i=0;i<n;i+=2) s+=(b[i]<<8)|b[i+1]; while(s>>16) s=(s&0xffff)+(s>>16);
  uint16_t c=~s&0xffff; return c?c:0xffff; }
static void dump(const char*n,const uint8_t*f,size_t l){(void)n;(void)l; const uint8_t*u=f+34; assert(((u[6]<<8)|u[7])==ref_csum(f));}
static size_t dhcp(uint8_t*f,const uint8_t*src,const uint8_t*dst,const uint8_t*ch,int client,int msgtype){
  memset(f,0,600); memcpy(f,dst,6); memcpy(f+6,src,6); f[12]=8;f[13]=0;
  uint8_t*ip=f+14; ip[0]=0x45; ip[8]=64; ip[9]=17;
  if(client){ memset(ip+16,0xff,4);} else { uint8_t s[4]={192,168,1,1},d[4]={192,168,1,50}; memcpy(ip+12,s,4); memcpy(ip+16,d,4);}
  uint8_t*u=ip+20; u[0]=0;u[1]=client?68:67;u[2]=0;u[3]=client?67:68;
  uint8_t*b=u+8; b[0]=client?1:2; b[1]=1;b[2]=6; memcpy(b+28,ch,6);
  b[236]=0x63;b[237]=0x82;b[238]=0x53;b[239]=0x63;
  uint8_t*o=b+240; *o++=53;*o++=1;*o++=msgtype; *o++=0; /*pad*/ *o++=61;*o++=7;*o++=1; memcpy(o,ch,6); o+=6; *o++=12;*o++=3;*o++='p';*o++='r';*o++='n'; *o++=255;
  size_t ulen=o-u; u[4]=ulen>>8;u[5]=ulen&255; u[6]=0x12;u[7]=0x34; /* bogus nonzero checksum to force recompute */
  size_t iplen=20+ulen; ip[2]=iplen>>8; ip[3]=iplen&255;
  return 14+iplen;
}
int main(void){
  l2rw_t st; l2rw_init(&st,STA,false); uint8_t f[600]; size_t n;
  uint8_t bc[6]={0xff,0xff,0xff,0xff,0xff,0xff};
  /* 1. DHCP discover from device */
  n=dhcp(f,DEV,bc,DEV,1,1);
  assert(l2rw_from_wired(&st,f,n)==L2RW_FORWARD);
  assert(st.dev_known && !memcmp(st.dev_mac,DEV,6));
  assert(!memcmp(f+6,STA,6));
  assert(!memcmp(f+14+20+8+28,STA,6));
  assert(!memcmp(f+14+20+8+240+4+3,STA,6)); /* opt61 after pad */
  assert(st.dhcp_rewrites==1);
  dump("discover.bin",f,n);
  /* 2. DHCP offer to station MAC, with echoed opt61 */
  n=dhcp(f,(uint8_t[]){0xaa,0xbb,0xcc,0,0,1},STA,STA,0,2);
  assert(l2rw_to_wired(&st,f,n)==L2RW_FORWARD);
  assert(!memcmp(f,DEV,6)); assert(!memcmp(f+14+20+8+28,DEV,6)); assert(!memcmp(f+14+20+8+240+4+3,DEV,6));
  assert(st.dhcp_rewrites==2);
  dump("offer.bin",f,n);
  /* 2b. DHCP ACK: lease (address, mask, router, DNS) is remembered */
  n=dhcp(f,(uint8_t[]){0xaa,0xbb,0xcc,0,0,1},STA,STA,0,5);
  { uint8_t *u=f+34, *b=u+8; uint8_t yi[4]={192,168,1,50}; memcpy(b+16,yi,4);
    uint8_t *e=b+240; while(*e!=255){ if(*e==0){e++;continue;} e+=2+e[1]; }
    const uint8_t extra[]={1,4,255,255,255,0, 3,4,192,168,1,1, 6,8,192,168,1,1,8,8,8,8, 255};
    memcpy(e,extra,sizeof extra); e+=sizeof extra;
    size_t ulen=e-u; u[4]=ulen>>8;u[5]=ulen&255; f[16]=(20+ulen)>>8; f[17]=(20+ulen)&255; n=14+20+ulen; }
  assert(l2rw_to_wired(&st,f,n)==L2RW_FORWARD);
  assert(st.lease_ip==(uint32_t)(192|168<<8|1<<16|50<<24));
  assert(st.lease_mask==(uint32_t)(255|255<<8|255<<16|0<<24));
  assert(st.lease_gw==(uint32_t)(192|168<<8|1<<16|1<<24));
  assert(st.lease_dns==(uint32_t)(192|168<<8|1<<16|1<<24));
  dump("ack.bin",f,n);
  /* an OFFER does not overwrite the lease */
  n=dhcp(f,(uint8_t[]){0xaa,0xbb,0xcc,0,0,1},STA,STA,0,2);
  assert(l2rw_to_wired(&st,f,n)==L2RW_FORWARD); assert(st.lease_ip==(uint32_t)(192|168<<8|1<<16|50<<24));
  /* 3. ARP request from device */
  memset(f,0,64); memcpy(f,bc,6); memcpy(f+6,DEV,6); f[12]=8;f[13]=6;
  uint8_t*a=f+14; a[1]=1;a[2]=8;a[4]=6;a[5]=4;a[7]=1; memcpy(a+8,DEV,6); a[14]=192;a[15]=168;a[16]=1;a[17]=50;
  assert(l2rw_from_wired(&st,f,60)==L2RW_FORWARD); assert(!memcmp(a+8,STA,6)); assert(!memcmp(f+6,STA,6));
  assert(st.dev_ip==(uint32_t)(192|168<<8|1<<16|50<<24));
  /* 4. ARP reply to STA */
  memset(f,0,64); memcpy(f,STA,6); memcpy(f+6,OTHER,6); f[12]=8;f[13]=6; a=f+14; a[7]=2; memcpy(a+8,OTHER,6); memcpy(a+18,STA,6);
  assert(l2rw_to_wired(&st,f,60)==L2RW_FORWARD); assert(!memcmp(f,DEV,6)); assert(!memcmp(a+18,DEV,6));
  /* 5. second device dropped */
  memset(f,0,64); memcpy(f,bc,6); memcpy(f+6,OTHER,6); f[12]=8;f[13]=6;
  assert(l2rw_from_wired(&st,f,60)==L2RW_DROP); assert(st.foreign_frames==1);
  /* 6. IPv6 dropped both ways */
  memset(f,0,64); memcpy(f+6,DEV,6); f[12]=0x86;f[13]=0xdd;
  assert(l2rw_from_wired(&st,f,60)==L2RW_DROP); assert(l2rw_to_wired(&st,f,60)==L2RW_DROP); assert(st.ipv6_dropped==2);
  /* 7. multicast source dropped, forget works */
  memset(f,0,64); f[6]=1; assert(l2rw_from_wired(&st,f,60)==L2RW_DROP);
  l2rw_forget(&st); assert(!st.dev_known); assert(st.lease_ip==0);
  /* 8. fuzz: random & truncated frames */
  srand(1); uint8_t *buf=malloc(700);
  for(int i=0;i<200000;i++){
    size_t len=rand()%700; uint8_t*t=malloc(len?len:1);
    if(i%3==0){ size_t m=dhcp(f,DEV,bc,DEV,rand()&1,1); if(len>m)len=m; memcpy(t,f,len); for(int k=0;k<4;k++) if(len) t[rand()%len]^=rand(); }
    else for(size_t k=0;k<len;k++) t[k]=rand();
    if(rand()&1) l2rw_from_wired(&st,t,len); else l2rw_to_wired(&st,t,len);
    free(t);
  }
  free(buf);
  puts("all l2rewrite tests passed");
  return 0;
}
