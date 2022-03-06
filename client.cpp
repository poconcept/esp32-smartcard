#include <cstdlib>
#include <thread>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <string.h>
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "private.hpp"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>


#define ST_ATR 0
#define ST_PPS 1
#define ST_PPS2 2
#define ST_PING 0x42
#define ST_CMD 10
#define ST_SW 11
#define ST_DATA 12
#define ST_SW2 13


unsigned char atr[33]={/* put the ATR here */};
unsigned short atr_size=22; //put the atr size here

bool connected=0;
bool useudp=0; //udp is a bad idea
bool usetcp=1;
bool monitor=1;
bool simulate=1;
bool readonly=0;

bool local=0; //dont connect to remote

// fake ack for tests :
bool fakeack=0;
int fakeackdelay=50;
int sendfakeack=0;
unsigned char fakeackcmd=0;


#define ESP_INTR_FLAG_DEFAULT 0

#define WS_PIN 34
#define CLK_PIN 35
#define RST 32
#define IO_PIN 33

//#define WS_PIN_OUT 25
//#define CLK_PIN_OUT 26

#define OUT_PIN 25

#define ERR_PIN 17
#define ERR_PIN2 16

#define REVERTED 0 //put 1 if the io is reverted 
#define REVERTED_OUTPUT 0 //put 1 if the io_output is reverted 

const i2s_port_t I2S_PORT_RX = I2S_NUM_0;
const i2s_port_t I2S_PORT_TX = I2S_NUM_1;

/***************************************/
/******** arduino like functions *******/
/***************************************/

void delay(int ms)
{
  vTaskDelay(ms / portTICK_PERIOD_MS);
}

void yield() {
  taskYIELD();
}

void digitalWrite(int pin,int lv)
{
  gpio_set_level(gpio_num_t(pin),lv);
}

bool digitalRead(int pin)
{
  return gpio_get_level(gpio_num_t(pin));
}

#define INPUT 1
#define OUTPUT 2

void pinMode(int pin,int mode)
{
  gpio_config_t io_conf;
  memset(&io_conf,0,sizeof(io_conf));
  io_conf.intr_type=GPIO_INTR_DISABLE;
  if(mode==OUTPUT)
    io_conf.mode=GPIO_MODE_OUTPUT;
  else
    io_conf.mode=GPIO_MODE_INPUT;
  io_conf.pin_bit_mask=(uint64_t(1)<<pin);

  gpio_config(&io_conf);
}

/***************************************/
/*********** qlog (for log/debug) ******/
/***************************************/

unsigned char clog[1024];
int clog_pos_end=0;
int clog_pos_st=0;

void qlog(const char *x)
{
  for(int i=0;x[i];i++)
    clog[(clog_pos_end++)&1023]=x[i];
  clog_pos_end&=1023;
}

char i2h(unsigned int i)
{
  if(i<10)
    return i+'0';
  return i+'A'-10;
}

void qhex(unsigned char x)
{
  clog[(clog_pos_end++)&1023]=i2h(x>>4);
  clog[(clog_pos_end++)&1023]=i2h(x&0xf);
  clog_pos_end&=1023;
}

void qprintf(const char *format, ... )
{
  char buffer[256];
  va_list args;
  va_start (args, format);
  vsnprintf (buffer,256,format, args);
  qlog(buffer);
  va_end (args);
}

/***************************************/
/************* Network *****************/
/***************************************/

#define EXAMPLE_ESP_MAXIMUM_RETRY  100

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WIFI";

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config;
    memset(&wifi_config,0,sizeof(wifi_config));

    memcpy(wifi_config.sta.ssid,EXAMPLE_ESP_WIFI_SSID,strlen(EXAMPLE_ESP_WIFI_SSID)+1);
    memcpy(wifi_config.sta.password,EXAMPLE_ESP_WIFI_PASS,strlen(EXAMPLE_ESP_WIFI_PASS)+1);
    
    /* Setting a password implies station will connect to all security modes including WEP/WPA.
     * However these modes are deprecated and not advisable to be used. Incase your Access point
     * doesn't support WPA2, these mode can be enabled by commenting below line */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

void proc_net(unsigned char *bf,int n);

int sock_udp=-1;
int sock_tcp=-1;

void send_udp(unsigned char *payload, int sz) {
  struct sockaddr_in dest_addr;

  dest_addr.sin_addr.s_addr = inet_addr(host);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(Port);

  int err = sendto(sock_udp, payload, sz, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err < 0) {
    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
    return;
  }
  ESP_LOGI(TAG, "Message sent");

  // struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
  // socklen_t socklen = sizeof(source_addr);
  // int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

  // // Error occurred during receiving
  // if (len < 0) {
  //   ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
  //   break;
  // }
  // // Data received
  // else {
  //   rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
  //   ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
  //   ESP_LOGI(TAG, "%s", rx_buffer);
  //   if (strncmp(rx_buffer, "OK: ", 4) == 0) {
  //     ESP_LOGI(TAG, "Received expected message, reconnecting");
  //     break;
  //   }
  // }

}

// if (sock != -1) {
//   ESP_LOGE(TAG, "Shutting down socket and restarting...");
//   shutdown(sock, 0);
//   close(sock);
//  }

void send_tcp(unsigned char *payload, int sz) {
  int err = write(sock_tcp, payload, sz);
  if (err < 0) {
    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
    return;
  }
  ESP_LOGI(TAG, "Message sent");
}

unsigned char bf_rcv[600];

void recv_udp() {
  if(!connected) {
    delay(1000);
    return;
  }
  
  struct sockaddr_in b;

  socklen_t lb=sizeof(b);
  int n=recvfrom(sock_udp,bf_rcv,600,0,(struct sockaddr*)&b,&lb);
  if (n<0) {
    ESP_LOGE(TAG, "recvfrom(sock_udp) failed: errno %d", errno);
    delay(100);
    return;
  }

  proc_net(bf_rcv,n);
}

void recv_tcp() {
  if(!connected) {
    delay(1000);
    return;
  }

  static int n=0;
  
  int n2=read(sock_tcp,bf_rcv+n,600-n);
  if (n2<0) {
    ESP_LOGE(TAG, "read(sock_tcp) failed: errno %d", errno);
    delay(100);
    return;
  }
  n+=n2;
  while(n>4) {
    int ii=(bf_rcv)[0];
    ii+=((int)(bf_rcv[1]))<<8;
    int s=(bf_rcv)[2];
    s+=((int)(bf_rcv[3]))<<8;
    if(n>=4+s) {
      proc_net(bf_rcv,4+s);
      if(n>4+s) {
	memmove(bf_rcv,bf_rcv+4+s,n-4-s);
      }
      n-=4+s;
    } else break;
  }
}

void setup_udp() {

  printf("setup_udp...\r\n");

  int addr_family = 0;
  int ip_protocol = 0;
  addr_family = AF_INET;
  ip_protocol = IPPROTO_IP;
    
  sock_udp = socket(addr_family, SOCK_DGRAM, ip_protocol);
  if (sock_udp < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    return;
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(Port);
    
  int err = bind(sock_udp, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err < 0) {
    ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
  }
    
  useudp=1;
  usetcp=0;
}

void setup_tcp() {

  printf("setup_tcp...\r\n");
  int addr_family = 0;
  int ip_protocol = 0;
  addr_family = AF_INET;
  ip_protocol = IPPROTO_IP;
    
  sock_tcp = socket(addr_family, SOCK_STREAM, ip_protocol);
  if (sock_tcp < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    return;
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = inet_addr(host);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(Port);
    
  int err=connect(sock_tcp,(const struct sockaddr*)&dest_addr,sizeof(dest_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
  }

  usetcp=1;
  useudp=0;
}

void setup_wifi() {
  //Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
  wifi_init_sta();
  connected=1;

  if(useudp)
    setup_udp();
  else
    setup_tcp();
}

unsigned char send_tmp[500];

void send_net(unsigned short head, unsigned char *c,unsigned short s)
{
  if(!connected) return;
  send_tmp[0]=head;
  send_tmp[1]=head>>8;
  send_tmp[2]=s;
  send_tmp[3]=s>>8;
  memcpy(send_tmp+4,c,s);

  //printf("send net packet %d\n",head);

  if(useudp)
    send_udp(send_tmp,s+4);
  else 
    send_tcp(send_tmp,s+4);
}

/***************************************/
/******************* T0  ***************/
/***************************************/


int ddiv=372;
#define MDIV 4

int Fi[16]= {372,372,558,744,1116,1488,1860,0/*RFU*/,0/*RFU*/,512,768,1024,1536,2048,0/*RFU*/,0/*RFU*/};
int Di[16]= {0/*RFU*/,1,2,4,8,16,32,64,12,20,0/*RFU*/,0/*RFU*/,0/*RFU*/,0/*RFU*/,0/*RFU*/,0/*RFU*/};

void error() //for debug
{
  //digitalWrite(ERR_PIN,1);
}

int pps(unsigned char *x,int n) //returns the clock duration for 1 bit (or <0 if the packet is not a complete PPS packet)
{
  int r=372;
  if(n<3 || x[0]!=0xff) return -1;
  unsigned char crc=x[0]^x[1];
  if((x[1]&0xf)!=0) return -2;
  int p=2;
  if(p<n && (x[1]&0x10)) {
    int F=Fi[x[p]>>4];
    int D=Di[x[p]&15];
    r=F/D;
    crc=crc^x[p];
    p++;
  }
  if(p<n && (x[1]&0x20)) {
    crc=crc^x[p];
    p++;
  }
  if(p<n && (x[1]&0x40)) {
    crc=crc^x[p];
    p++;
  }
  if(p>=n) return -4;
  if(crc!=x[p]) return -3;
  
  return r;
}



short ATRDecodeAtr(unsigned char *atr, int dwLength,int dbg=0)
{
  unsigned char Y1i;
  int i = 1;

  if (dwLength <= 2)
    return 0;
  
  if ((atr[0] != 0x3F) && (atr[0] != 0x3B))
    return 0;
  
  Y1i = atr[1] >> 4;	

  int histo=atr[1]&0xf;
  
  if(dbg>1)
    printf("Y1i=%x histo=%d\n",Y1i,histo);
  
  unsigned short p=2;
  
  do {
    short TAi=0x1, TBi, TCi, TDi;
      
    TAi = (Y1i & 0x01) ? atr[p++] : -1;
    if(p==dwLength) return 0;
    TBi = (Y1i & 0x02) ? atr[p++] : -1;
    if(p==dwLength) return 0;
    TCi = (Y1i & 0x04) ? atr[p++] : -1;
    if(p==dwLength) return 0;
    TDi = (Y1i & 0x08) ? atr[p++] : -1;
    if(p==dwLength) return 0;

    if(dbg>2)
      printf("TA%d: %02X, TB%d: %02X, TC%d: %02X, TD%d: %02X\n",i, TAi, i, TBi, i, TCi, i, TDi);

    if (TDi >= 0)
      Y1i = TDi >> 4;
    else
      Y1i = 0;
      
    if(i==1) {
      int F=Fi[TAi>>4];
      int D=Di[TAi&15];
      int r=F/D;
      if(dbg>0)
	printf("TA1=%02x max speed %d/%d = %d\n",TAi,F,D,r);
    }
      
    if (p > 33)
      return 0;

    i++;
  } while (Y1i != 0);

  for(int i=0;i<histo;i++) {
    p++;
    if(p==dwLength) return 0;
  }

  unsigned char x=0;
  for(int i=1;i<dwLength-1;i++)
    x=x^atr[i];
  if(x==atr[p]) {
    if(dbg>0)
      printf("CRC OK\n");
  } else {
    if(dbg>0)
      printf("CRC KO\n");
  }
  
  return 1; 
}


bool checkATR(unsigned char *v,int l)
{
  auto r=ATRDecodeAtr(v,l,0);
  if(r) {
    ATRDecodeAtr(v,l,1);
  }
  
  return r;
}

unsigned char tr_dt[512];
unsigned char tr_cmd[8];
unsigned char tr_sw[8];
unsigned char tr_sw2[8];
unsigned char *tr=tr_dt;
int sr=0;

// unsigned char tr2[512];
// int sr2=0;

char *bb(unsigned char x)
{
  static char r[9];
  r[8]=0;
  for(int i=0;i<8;i++)
    r[i]='0'+(1&(x>>(7-i)));
  return r;
}


int nbr1(unsigned char x)
{
  unsigned char u=(x&0x55)+((x>>1)&0x55);
  unsigned char v=(u&0x33)+((u>>2)&0x33);
  return (v&0xf)+(v>>4);
}

char *bb10(unsigned short x)
{
  static char r[11];
  r[10]=0;
  for(int i=0;i<10;i++)
    r[i]='0'+(1&(x>>(9-i)));
  return r;
}

int stage=0; 

int pr_o=0;
int pr_cnt=0;
int F=372;

void clearsr() {
  sr=0;
  pr_o=0;
  pr_cnt=0;
}

unsigned char cmd;

const char *scmd(unsigned char x)
{
  if(x==0xa4) return "SELECT";
  if(x==0xc0) return "GET RESPONSE";

  if(x==0xb0) return "READ BIN";
  if(x==0xb2) return "READ REC";
  if(x==0xd6) return "UPDATE BIN";
  if(x==0xdc) return "UPDATE REC";

  if(x==0xF2) return "STATUS";

  if(x==0xA2) return "SEEK";
  
  return "UNK";
}

short rxsw=0;
short rxsw2=0;
short rxdt=0;
short datalen=0;
int nbcmd=100;

void affrx(unsigned char *x,int l) {
  for(int i=0;i<l;i++)
    printf("%02x ",x[i]);
  printf("\n");
}

void affrx() {
  printf("rx (%d):\n",nbcmd);
  affrx(tr_cmd,5);
  affrx(tr_sw,rxsw);
  affrx(tr_dt,rxdt);
  affrx(tr_sw2,rxsw2);
}

void affrxshort() {
  printf("[%04d]",nbcmd);
  for(int i=0;i<5;i++)
    printf("%02x",tr_cmd[i]);
  printf("|");
  for(int i=0;i<rxsw;i++)
    printf("%02x",tr_sw[i]);
  printf("|(%d)",rxdt);
  if(rxdt)
    printf("%02x:%02x",tr_sw2[0], tr_sw2[rxdt-1]);
  printf("|");
  for(int i=0;i<rxsw2;i++)
    printf("%02x",tr_sw2[i]);
  printf("\n");
}


void proccmd(unsigned char *t,int n) {
  //Assert(n==5);
  nbcmd++;
  cmd=t[1];
  datalen=t[4];
  //printf("cmd: %s %2x %2x %2x %2x %2x %d\n",scmd(cmd),t[0],t[1],t[2],t[3],t[4],datalen);
  rxsw=rxsw2=rxdt=0;
}

int srmax=300;

void sendrx() { //send whole packet
  if(!connected) return;

  send_tmp[0]=nbcmd;
  send_tmp[1]=nbcmd>>8;
  int s=5+rxsw+rxdt+rxsw2;
  send_tmp[2]=s;
  send_tmp[3]=s>>8;

  memcpy(send_tmp+4,tr_cmd,5);
  memcpy(send_tmp+4+5,tr_sw,rxsw);
  memcpy(send_tmp+4+5+rxsw,tr_dt,rxdt);
  memcpy(send_tmp+4+5+rxsw+rxdt,tr_sw2,rxsw2);

  send_net(nbcmd,send_tmp+4,s);
}

void gotfull() {
  if(monitor) sendrx();
  //affrxshort();
}

int rbf=0;
int rbfb=0;

void sendpps();
unsigned  char ppst[10];
int szpps=4;

void gotostage(int st)
{
  qlog("gotost ");
  qhex(st);
  qlog("\r\n");
  if(st==ST_CMD) {
    tr=tr_cmd;srmax=8;
  }
  if(st==ST_SW) {
    tr=tr_sw;srmax=8;
  }
  if(st==ST_DATA) {
    tr=tr_dt;srmax=300;
  }
  if(st==ST_SW2) {
    tr=tr_sw2;srmax=8;
  }
  stage=st;
  clearsr();
}

int nbrst=0;

void rxbyte(unsigned char x) {
  //if(stage>=10 && rbfb>10000)
  //printf("RX_SM %02x st=%d sr=%3d rbfb=%d rst=%d\n",x,stage,sr,rbfb,nbrst);

  qlog("S");
  qhex(x);
  qlog("\r\n");
  
  if(stage==0 && sr==0 && x!=0x3b) return;
  
  if(sr<srmax)
    tr[sr++]=x;
  
  if(stage==0) {
    if(checkATR(tr,sr)) {
      qlog("ATROK\r\n");
      send_net(0,tr,sr);
      gotostage(ST_CMD);
    }
    return;
  }

  if(stage==1) {
    if((F=pps(tr,sr))>0) {
      //printf("PPS 1 OK F=%d\n",F);
      memcpy(ppst,tr,sr);
      szpps=sr;
      send_net(1,tr,sr);
      stage=2;
      clearsr();
      if(simulate)
	sendpps();
    }
    return;
  }

  if(stage==2) {
    if((F=pps(tr,sr))>0) {
      //printf("PPS 2 OK F=%d\n",F);
      send_net(2,tr,sr);
      ddiv=F;
      stage=10;
      tr=tr_cmd;srmax=8;
      clearsr();
    }
    return;
  }

  if(stage==ST_CMD) {
    if(sr==5) { 
      qlog("CMDOK\r\n");
      proccmd(tr,5);
      if(simulate) send_net(10,tr,5);
      if(fakeack) {
	sendfakeack=fakeackdelay;
	fakeackcmd=cmd;
      }
      gotostage(ST_SW);
    }
    return;
  }
  if(stage==ST_SW) {
    rxsw=sr;
    if(sr==1) {
      if(tr[0]==cmd) { //ACK
	qlog("ACK\r\n");
	if(datalen==0) {
	  gotfull();
	  gotostage(ST_CMD);
	} else {
	  gotostage(ST_DATA);
	}
      } else if(tr[0]==0x60) {
	qlog("SW60\r\n");
	gotfull();
	gotostage(ST_CMD);
      } else if((tr[0]&0xf0)==0x60 || (tr[0]&0xf0)==0x90) {
	//
      } else {
	error();
	printf("ERROR SW\n");
	affrx();
      }
    } else {
      //printf(" SW %02x %02x\n",tr[0],tr[1]);
      qlog("SWOK\r\n");
      gotfull();
      gotostage(ST_CMD);
    }
    return;
  }
  if(stage==ST_DATA) {
    rxdt=sr;
    if(sr==datalen) {
      qlog("DTOK\r\n");
      if(cmd!=0xc0 && cmd!=0xf2&& cmd!=0xb0&& cmd!=0xb2) {
	qlog("sendnet\r\n");
	if(simulate) send_net(ST_DATA,tr,sr);
      }
      gotostage(ST_SW2);
    }
    return;
  }

  if(stage==ST_SW2) {
    rxsw2=sr;
    if(sr==1) {
      if(tr[0]==cmd) { //ACK
	printf(" ERROR ACK\n");
	affrx();
	clearsr();
      } else if(tr[0]==0x60) {
	qlog("Sw60\r\n");
	gotfull();
	gotostage(ST_CMD);
      } else if((tr[0]&0xf0)==0x60 || (tr[0]&0xf0)==0x90) {
	//
      } else {
	error();
	printf("ERROR SW2 %x\n",x);
	affrx();
	clearsr();
      }
    } else {
      //printf(" SW2 %02x %02x\n",tr[0],tr[1]);
      qlog("SwOK\r\n");
      gotfull();
      gotostage(ST_CMD);
    }
    return;
  }
  
}

void proc1(unsigned int d) {
  //assert(d==0 || d==1);
  static int st=0;

  if(st==0 && d==1) return;
  if(st==0) {
    rbfb=rbf;
  }
  st=1;
  static int n=0;
  static int r=0;
  r= r| (d<<n);
  n++;
  if(n==10) {
    //printf("%03x ",r>>1);
    //printfln(bb10(r));
    rxbyte(r>>1);
    n=r=st=0;
  }
}

void procd(char d,int sz)
{
  // if(sr<190)
  //   tr[sr++]=sz/8;
  // return;

  int iz=sz+ddiv/2/MDIV;
  iz/=ddiv/MDIV;
  while(iz>0) {
    proc1(REVERTED^d);
    iz--;
  }

  static char o=2;
  if(d==o) rbf+=sz;
  else rbf=sz;
  o=d;
  
}

//unsigned char reord[16]={7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8};
unsigned char reord[16]={3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12};

unsigned short comp1[256];


int dist(unsigned char a,unsigned char b) {
  return nbr1(a^b);
}

void compute1() {
  unsigned short comp2[256];
  memset(comp2,0,sizeof(comp1));
  for(unsigned int x=0;x<256;x++) {
    char *s=bb(x);
    int a=s[0];
    int b=1;
    while(b<8 && s[b]==a) b++;
    int c=s[7];
    int d=1;
    while(d<8 && s[7-d]==c) d++;
    if(a==c)
      d=0;
    a-='0';
    c-='0';
    if(b+d==8) {
      comp2[x]=(a<<12)+(b<<8)+(c<<4)+d;
      //printf("%s %d %d %d %d %04x\n",s,a,b,c,d,comp2[x]);
    }
  }
  
  for(unsigned int x=0;x<256;x++) {
    int md=100;
    int best=1000;
    for(unsigned int y=0;y<256;y++) {
      if(comp2[y] && dist(x,y)<md) {
	comp1[x]=comp2[y];
	md=dist(x,y);
	best=y;
      }
    }
    //assert(best<1000);
    //printf("%s ",bb(x));
    //printf("%s %04x\n",bb(best),comp1[x]);
  }
}

void proc_rx_8q(unsigned char *tab_,int size)
{

  for(int i=0;i<size;i++) {
    int v=tab_[(i&0xfff0)+reord[i&15]];

    // if(stage>=10) {
    //   if(sr2==0 && v!=0 &&v!=255) {
    // 	  tr2[sr2++]=v;
    //   } else
    // 	if(sr2 && sr2<490)
    // 	  tr2[sr2++]=v;
    // }

    //int v=tab_[i];
    //    unsigned int t=v&1;
    
    int y=comp1[v]; //nbr1(v);
    unsigned char a=y>>12;a=a&1;
    unsigned char b=y>>8;b=b&0xf;
    unsigned char c=y>>4;c=c&1;
    unsigned char d=y;d=d&0xf;

    //unsigned int t=(y>>2)>0;
    if(pr_o!=a) {
      procd(pr_o,pr_cnt);
      pr_o=a;
      pr_cnt=0;
    }
    pr_cnt+=b;
    if(pr_cnt>ddiv*11) {
      procd(pr_o,pr_cnt);
      pr_cnt=0;
    }
    if(d) {
      if(pr_o!=c) {
	procd(pr_o,pr_cnt);
	pr_o=c;
	pr_cnt=0;
      }
      pr_cnt+=d;
      if(pr_cnt>ddiv*11) {
	procd(pr_o,pr_cnt);
	pr_cnt=0;
      }
    }
    
  }
}


#define mmode I2S_MODE_SLAVE

void init_i2s_rx() {
  esp_err_t err;

  // The I2S config as per the example
  const i2s_config_t i2s_config_rx = {
    .mode = i2s_mode_t(mmode /*I2S_MODE_MASTER*/ | I2S_MODE_RX), // Receive, not transfer
    .sample_rate = 24000,                         // 16KHz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // could only get it to work with 32bits
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, //I2S_CHANNEL_FMT_ONLY_RIGHT
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,     // Interrupt level 1
    .dma_buf_count = 2,                           // number of buffers
    .dma_buf_len = 32                              // 8 samples per buffer (minimum)
  };

  // The pin config as per the setup
  const i2s_pin_config_t pin_config_rx = {
    .bck_io_num = CLK_PIN,   // Serial Clock (SCK)
    .ws_io_num = WS_PIN,    // Word Select (WS) //NOT USED
    .data_out_num = I2S_PIN_NO_CHANGE, // not used (only for speakers) //NOT USED
    .data_in_num = IO_PIN   // Serial Data (SD) 
  };

  // Configuring the I2S driver and pins.
  // This function must be called before any I2S driver read/write operations.
  err = i2s_driver_install(I2S_PORT_RX, &i2s_config_rx, 0, NULL);
  if (err != ESP_OK) {
    printf("Failed installing driver rx: %d\n", err);
    while (true);
  }
  err = i2s_set_pin(I2S_PORT_RX, &pin_config_rx);
  if (err != ESP_OK) {
    printf("Failed setting pin rx: %d\n", err);
    while (true);
  }
}

void init_i2s_tx() {
  esp_err_t err;

  // The I2S config as per the example
  const i2s_config_t i2s_config_tx = {
      .mode = i2s_mode_t(mmode | I2S_MODE_TX), // Receive, not transfer
      .sample_rate = 24000,                         // 16KHz
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // could only get it to work with 32bits
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, //I2S_CHANNEL_FMT_ONLY_RIGHT
      .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,     // Interrupt level 1
      .dma_buf_count = 2,                           // number of buffers
      .dma_buf_len = 32                              // 8 samples per buffer (minimum)
  };

  // The pin config as per the setup
  const i2s_pin_config_t pin_config_tx = {
      .bck_io_num = CLK_PIN,   // Serial Clock (SCK)
      .ws_io_num = WS_PIN,    // Word Select (WS) //NOT USED
      .data_out_num = OUT_PIN, //I2S_PIN_NO_CHANGE ,
      .data_in_num =  I2S_PIN_NO_CHANGE , //OUT_PIN, 
  };

  // Configuring the I2S driver and pins.
  // This function must be called before any I2S driver read/write operations.
  err = i2s_driver_install(I2S_PORT_TX, &i2s_config_tx, 0, NULL);
  if (err != ESP_OK) {
    printf("Failed installing driver tx: %d\n", err);
    while (true);
  }
  err = i2s_set_pin(I2S_PORT_TX, &pin_config_tx);
  if (err != ESP_OK) {
    printf("Failed setting pin tx: %d\n", err);
    while (true);
  }
}


short tosend[512];
short pos_send=0;
short pos_end=0;


void IRAM_ATTR send1(unsigned char x)
{
  auto ff=ddiv/MDIV;
  if((pos_end&1)==0)
    tosend[++pos_end]=0;
  tosend[pos_end]+=3*ff;
  tosend[++pos_end]=0;
  tosend[pos_end]+=ff;
  int p=0;
  for(int z=0;z<8;z++) {
    p+=(x>>z)&1;
    if((pos_end&1)!=((x>>z)&1))
      tosend[++pos_end]=0;
    tosend[pos_end]+=ff;
  }
  if((pos_end&1)!=(p&1))
    tosend[++pos_end]=0;
  tosend[pos_end]+=ff;
  if((pos_end&1)==0)
    tosend[++pos_end]=0;
  tosend[pos_end]+=10*ff;
}

unsigned char ctosend[1024];
short ctosend_pos=0;
short ctosend_end=0;

void IRAM_ATTR clearsend()
{
  pos_end=0;
  pos_send=0;
}

void IRAM_ATTR resetsend()
{
  clearsend();
  ctosend_pos=0;
  ctosend_end=0;
}

void IRAM_ATTR send(unsigned char *tab,int sz)
{
  for(int i=0;i<sz;i++) {
    ctosend[ctosend_pos]=tab[i];
    ctosend_pos=(1+ctosend_pos)&1023;
  }
}

void fillsend()
{
  clearsend();
  if(ctosend_end==ctosend_pos) return;
  send1(ctosend[ctosend_end]);
  ctosend_end=(1+ctosend_end)&1023;

  pos_end++;
  tosend[pos_end]=0;  
}

void IRAM_ATTR sendatr()
{
  //printf("send... (%d %d)\n",pos_send,pos_end);
  if(simulate)
    send(atr,atr_size);
}

void sendpps()
{
  if(simulate)
    send(ppst,szpps);
}



void proc_net(unsigned char *bf,int n)
{
  if(n<4) {
    printf("error proc net len=%d\n",n);
    return;
  }
  
  int sz=bf[2]+(((int)bf[3])<<8);
  int cmd=bf[0]+(((int)bf[1])<<8);
  //printf("rx_net %02x %02x %02x %02x cmd=%d len=%d\n",bf[0],bf[1],bf[2],bf[3],cmd,sz);

  if(cmd==ST_PING) {
    bf[4+sz]=0;
    //printf("PING TTL=%d '%s'\n",bf[4],bf+5);
    if(bf[4]) {
      bf[4]--;
      send_net(ST_PING,bf+4,sz);
    }
    return;
  }
  
  if(cmd==ST_ATR ||cmd==ST_SW || cmd==ST_SW2 || cmd==ST_DATA) {

    if(n<sz+4) {
      printf("ERROR RCV too small  %d n=%d !=sz\r\n",cmd,n);
      return;
    }

    // printf("rx_net ");
    // for(int i=0;i<sz;i++)
    //   printf("%02x ",bf[i+4]);
    // printf("\n");
    if(cmd!=ST_ATR) {
      if(stage!=cmd) {
	printf("wrong stage net:%d int:%d\r\n",cmd,stage);
      }
      for(int i=0;i<sz;i++) {
	qlog("N");
	qhex(bf[i+4]);
	qlog("\r\n");
      }
      if(simulate)
	send(bf+4,sz);
	
    } else {
      memcpy(atr,bf+4,sz);
      atr_size=sz;

      printf("new atr of size %d : \n",atr_size);
      for(int i=0;i<atr_size;i++)
	printf("%02x ",atr[i]);
      printf("\n");
    }
      
    return;
  }
  printf("ERROR proc net cmd=%x\r\n",cmd);
}


unsigned char precomp[2][9]={
  {0x00,0x80,0xc0,0xe0,0xf0,0xf8,0xfc,0xfe,0xff},
  {0xff,0x7f,0x3f,0x1f,0x0f,0x07,0x03,0x01,0x00},
};


void onesend() {
  unsigned char base=0xff;
  if(REVERTED_OUTPUT) base=0;

  unsigned char tab_[32];
  for(int i=0;i<32;i++) {
    tab_[(i&0xfff0)+reord[i&15]]=base;
    while(tosend[pos_send]==0 && pos_send<pos_end)
      pos_send++;

    if(pos_send>=pos_end) {
      fillsend();
      while(tosend[pos_send]==0 && pos_send<pos_end)
	pos_send++;
    }

    if(pos_send>=pos_end) {
      //nothing to send
    } else {
      if(tosend[pos_send]>8) {
	tosend[pos_send]-=8;
	if((pos_send&1)==0)
	  tab_[(i&0xfff0)+reord[i&15]]=base^0xff;
	else
	  tab_[(i&0xfff0)+reord[i&15]]=base;
      } else if(pos_send+1==pos_end) {
	if((pos_send&1)==0)
	  tab_[(i&0xfff0)+reord[i&15]]=base^0xff;
	else
	  tab_[(i&0xfff0)+reord[i&15]]=base;
	tosend[pos_send]=0;
	pos_send++;
      } else {
	int r=tosend[pos_send];
	tab_[(i&0xfff0)+reord[i&15]]=0xff^precomp[pos_send&1][r];
	tosend[pos_send]=0;
	pos_send++;
	tosend[pos_send]-=8-r;
	if(tosend[pos_send]<0) tosend[pos_send]=0;
      }
    }

  }
  size_t nr=0;

  bool aff=0;
    
  
  int err = i2s_write(I2S_PORT_TX, tab_, 32,&nr,portMAX_DELAY);

  static int rr=0;
  digitalWrite(ERR_PIN,rr);
  digitalWrite(ERR_PIN2,rr);
  rr=rr^1;

  //assert(err==ESP_OK && nr==32);
  if(err!=ESP_OK || nr!=32) aff=1;
    
  if(aff) {
    for(int i=0;i<32;i++)
      printf("%02x ",tab_[i]);
    printf("write %d %d\r\n",err,nr);
  }
}


void IRAM_ATTR RST_down() {
  nbrst+=1000;
  resetsend();
}



void IRAM_ATTR RST_up() {
  //digitalWrite(ERR_PIN,1);
  sr=0;
  pr_o=0;
  pr_cnt=0;
  stage=0;
  tr=tr_dt;
  srmax=300;
  ddiv=372;
   
  nbrst+=1;
  if(simulate)
    sendatr();
  //digitalWrite(ERR_PIN,0);
}

void IRAM_ATTR RST_change(void*) {
  //printf("RST %d\n",digitalRead(RST));
  if(digitalRead(RST))
    RST_up();
  else
    RST_down();
}

unsigned char sample[64];

void oneread() {
  static int a=0;
  static int aok=0;
  static bool pr=1;

  
  if(a==0) {
    //printf("a=0\r\n");
    pr=1;
    sr=0;
    pr_o=0;
    pr_cnt=0;
  }

  a++;

  size_t nr=0;

  int err = i2s_read(I2S_PORT_RX, sample, 32,&nr,1000);//portMAX_DELAY);

  static int rr=0;
  //digitalWrite(ERR_PIN2,rr);
  rr=rr^1;

  //printf("err=%d nr=%d\n",err,nr);
  if(err==ESP_OK) {
    if(pr && nr==32) {
      aok++;
      proc_rx_8q(sample,nr);
    }
  } //else printfln("error i2s_read");
}


/************** SETUP ***********************************/

void nettask(void*) {
  while(1) {
    if(useudp) 
      recv_udp();
    else
      recv_tcp();
    //delay(1000);
    yield();
  }
}

void sendtask(void*) {
  while(1) {
    onesend();
    yield();
  }
}

void readtask(void*) {
  while(1) {
    oneread();
    yield();
  }
}

int l=0;
void loop() {
  while(clog_pos_st!=clog_pos_end) {
    printf("%c",clog[clog_pos_st]);
    clog_pos_st=(clog_pos_st+1)&1023;
  }
  l++;
  if(l%100==0) {
    printf("#LOOP: %d  #rst: %d rst:%d\r\n",l,nbrst,(digitalRead(RST)));
    
    send_net(ST_PING,(unsigned char *)"\x01TEST",5);
  }
  delay(50);
  if(sendfakeack>0) {
    sendfakeack--;
    if(sendfakeack==0) {
      printf("send fake ack %x\n",fakeackcmd);
      send(&fakeackcmd,1);
    }
  }
}

void looptask(void*) {
  while(1) {
    loop();
    taskYIELD();
  }
}


void setup() {
  for(int i=0;i<4;i++)
    printf("=== SETUP ===\r\n");

  compute1();
  
  if(!local)
    setup_wifi();
  
  pinMode(ERR_PIN,OUTPUT);
  digitalWrite(ERR_PIN,0);
  pinMode(ERR_PIN2,OUTPUT);
  digitalWrite(ERR_PIN2,0);

  init_i2s_rx();
  if(!readonly)
    init_i2s_tx();
  printf("I2S drivers installed.\r\n");

  pinMode(RST,INPUT);

  gpio_set_intr_type(gpio_num_t(RST), GPIO_INTR_ANYEDGE); 
  //gpio_isr_register(gpio_num_t(RST), RST_change, NULL); 
  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
  gpio_isr_handler_add(gpio_num_t(RST), RST_change, NULL);
  gpio_intr_enable(gpio_num_t(RST));
  
  for(int i=0;i<4;i++)
    printf("=== START ===\r\n");

  xTaskCreatePinnedToCore(readtask,"readtask",1024*4, NULL, 2, NULL, 0);
  if(!readonly)
    xTaskCreatePinnedToCore(sendtask,"sendtask",2048,NULL,2,NULL,0);

  xTaskCreatePinnedToCore(nettask,"nettask",2048,NULL,1,NULL,1);
  xTaskCreatePinnedToCore(looptask,"looptask",2048,NULL,1,NULL,1);
}

using namespace std;

extern "C" void app_main(void)
{
  setup();
}
