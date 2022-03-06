#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <vector>
#include <map>
#include <list>
#include <string.h>
#include <string>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <stdarg.h>

using namespace std;

#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>


#define ST_ATR 0
#define ST_PPS 1
#define ST_PPS2 2
#define ST_PING 0x42
#define ST_CMD 10
#define ST_SW 11
#define ST_DATA 12
#define ST_SW2 13

bool usesim=0;

bool isresp(unsigned char x)
{
  if(x==0xc0) return 1;
  if(x==0xf2) return 1;
  if(x==0xb0) return 1;
  if(x==0xb2) return 1;

  return 0;
}

void print_current_time_with_ms (FILE *out=stdout)
{
    long            ms; // Milliseconds
    time_t          s;  // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    ms = round(spec.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
    if (ms > 999) {
        s++;
        ms = 0;
    }

    fprintf(out,"[%10" PRIdMAX ".%03ld] ",(intmax_t)s, ms);
}

pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutexsm=PTHREAD_MUTEX_INITIALIZER;
list<unsigned char> rcvsm;

int set_interface_attribs (int fd, int speed, int parity)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
	  perror ("error %d from tcgetattr");
                return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        tty.c_iflag &= ~IGNBRK ;         // disable break processing
	//tty.c_iflag &= ~INLCR;
	//tty.c_iflag &= ~ICRNL;
	tty.c_iflag = 0;                // no remapping, no delays
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays

	tty.c_cc[VMIN]  = 0;            // read doesn't block
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
        {
                perror("from tcsetattr");
                return -1;
        }
        return 0;
}

void
set_blocking (int fd, int should_block)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                perror("from tggetattr");
                return;
        }

        tty.c_cc[VMIN]  = should_block ? 1 : 0;
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
	  perror("setting term attributes");
}

string portname = "/dev/ttyUSB0";

int fd_sm=-1;

void special_set(int flag,bool x) {
  //mprintf("special_set %x to %d\n",flag,x);
  if(x==1) 
    ioctl(fd_sm,TIOCMBIS,&flag);//Set RTS pin
  else
    ioctl(fd_sm,TIOCMBIC,&flag);//Clear RTS pin
}

int spe[6]={
  TIOCM_RTS,//   RTS (request to send) // RTS is connected to RST 
  TIOCM_CTS,//   CTS (clear to send)
  TIOCM_DTR,//   DTR (data terminal ready)
  TIOCM_DSR,//   DSR (data set ready)
  TIOCM_CAR,//   DCD (data carrier detect)
  TIOCM_RNG,//   RNG (ring)        TIOCM_RI    see TIOCM_RNG
};

bool inittty() {
  fd_sm = open (portname.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
  if (fd_sm < 0) {
    perror("error open tty");
    exit(0);
    //return 0;
  }
  
  set_interface_attribs (fd_sm, B9600, PARENB);  // set speed, 8n1 (parity)
  set_blocking (fd_sm, 0);                // set no blocking
  
  return 1;
}


void ex(int i) {
  //mprintf("exit %d\n",i);
  //gzclose(out);
  exit(0);
}

int Fi[16]= {372,372,558,744,1116,1488,1860,0/*RFU*/,0/*RFU*/,512,768,1024,1536,2048,0/*RFU*/,0/*RFU*/};
int Di[16]= {0/*RFU*/,1,2,4,8,16,32,64,12,20,0/*RFU*/,0/*RFU*/,0/*RFU*/,0/*RFU*/,0/*RFU*/,0/*RFU*/};


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
  
  if(dbg>1)    printf("Y1i=%x histo=%d\n",Y1i,histo);
  
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

    if(dbg>2) printf("TA%d: %02X, TB%d: %02X, TC%d: %02X, TD%d: %02X\n",i, TAi, i, TBi, i, TCi, i, TDi);

    if (TDi >= 0)
      Y1i = TDi >> 4;
    else
      Y1i = 0;
      
    if(i==1) {
      int F=Fi[TAi>>4];
      int D=Di[TAi&15];
      int r=F/D;
      if(dbg>0)printf("TA1=%02x max speed %d/%d = %d\n",TAi,F,D,r);
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

void patchATR(unsigned char *x,int l,unsigned char y=0x01) {
  if(l<2) return;
  x[2]=y;
  int crc=0;
  for(int i=1;i<l-1;i++) {
    crc=crc^x[i];
  }
  x[l-1]=crc;
}

bool checkATR(unsigned char *v,int l)
{
  auto r=ATRDecodeAtr(v,l,0);
  if(r) {
    ATRDecodeAtr(v,l,1);
    patchATR(v,l);
    ATRDecodeAtr(v,l,1);
    for(int i=0;i<l;i++)
      printf("0x%02x,",v[i]);
    printf("\nsize=%d\n",l);
  }
  
  return r;
}

int pps(unsigned char *x,int n)
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

void *th_rcv_sm(void*)
{
  while(1) {
    unsigned char tst[100];
    int n=read(fd_sm,tst,100);
    assert(n>=0);

    pthread_mutex_lock(&mutexsm);
    for(int i=0;i<n;i++) {
      rcvsm.push_back(tst[i]);
    }
    pthread_mutex_unlock(&mutexsm);
  }
}

struct ss_t {
  FILE *out=NULL;
  bool exit=0;

  pthread_t tid;
  pthread_t tidss;

  ss_t(int a) {
    fd=a;
    char bf[100];
    if(out) {
      fflush(out);
    }
    snprintf(bf,99,"%Ld.%d.%d.log",time(NULL),rand()%1000,fd);
    out=fopen(bf,"w");

    mprintf("fd=%d...\n",fd);
    
    mprintf("launch threads...\n");
    pthread_create(&tid,NULL,th_rcv,(void*)this);
    pthread_create(&tidss,NULL,th_ss,(void*)this);

    mprintf("threads launched...\n");
  }
  
  
  void write_fd_sm(unsigned char *x,int l)
  {
    //print_current_time_with_ms();
    mprintf("TX_SM: ");
    for(int i=0;i<l;i++)
      mprintf("%02x ",x[i]);
    mprintf("\n");
    write(fd_sm,x,l);
  }

  void mprintf(const char *format, ... )
  {
    char buffer[256];
    va_list args;
    va_start (args, format);
    vsnprintf (buffer,256,format, args);
    fprintf(stdout,buffer);
    if(out) 
      fprintf(out,buffer);
    va_end (args);
  }


  void print_current_time_with_ms ()
  {
    long            ms; // Milliseconds
    time_t          s;  // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    ms = round(spec.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
    if (ms > 999) {
      s++;
      ms = 0;
    }

    mprintf("[%10" PRIdMAX ".%03ld] ",(intmax_t)s, ms);
  }

  vector<unsigned char> rcv_sm;

  volatile int stage=0;

  vector<unsigned char> atr;

  void send_net(int fd,short c,short size,unsigned char *x)
  {
    struct sockaddr_in client;
    print_current_time_with_ms();
    mprintf("send size=%d c=%d fd=%d: ",size,c,fd);
    for(int i=0;i<size;i++)
      mprintf("%02x ",x[i]);
    mprintf("\n");

    if(fd<0) return;
  
    //pthread_mutex_lock(&mutex);
    unsigned char r[1000];
    ((short*)r)[0]=c;
    ((short*)r)[1]=size;
    memcpy(r+4,x,size);

    socklen_t lb=sizeof(client);
    int n=-1;
    n=write(fd,r,size+4);
  
    if(n<0)
      perror("write send_net");
    if(n!=size+4) {
      mprintf("error write send_net n2=%d\n",n);
    }
  }


  unsigned char cmd[5];

  void gotostage(int st)
  {
    rcv_sm.clear();
    stage=st;
  }

  int fd=-1;

  int readsm(unsigned char *tst,int n) {
    int r=0;
    while(1) {
      if(this->exit) {
	mprintf("readsm exit by exit\n");
	return -1;
      }
      pthread_mutex_lock(&mutexsm);
      while(!rcvsm.empty() && r<n) {
	tst[r++]=rcvsm.front();
	rcvsm.pop_front();
      }
      pthread_mutex_unlock(&mutexsm);
      if(r) return r;
      usleep(10);
    }
  }

  static void* th_rcv(void *a_)
  {
    ss_t *a=(ss_t*)a_;
    a->th_rcv_();
    return NULL;
  }

  void th_rcv_()
  {
    while(1) {
      unsigned char tst[100];
      int n=readsm(tst,100);
      if(n<0) break;
      if(n>0) {
	print_current_time_with_ms();
	mprintf("rxsm: ");
	for(int i=0;i<n;i++)
	  mprintf("%02x ",tst[i]);
	mprintf("\n");
	pthread_mutex_lock(&mutex);
	for(int i=0;i<n;i++)
	  rcv_sm.push_back(tst[i]);
	int sr=rcv_sm.size();

	if(stage==ST_ATR) {
	  if(checkATR(rcv_sm.data(),rcv_sm.size())) {
	    mprintf("(sm) got ATR\n");
	    atr=rcv_sm;
	    gotostage(ST_CMD);
	  }
	
	} else 
	  if(stage==ST_CMD) {
	    if(rcv_sm.size()==5) {
	      gotostage(ST_SW);
	    }
	  } else 
	    if(stage==ST_SW) {
	      int datalen=cmd[4];
	      if(isresp(cmd[1]) && datalen==0) datalen=256;
	      if(sr==1) {
		if(rcv_sm[0]==cmd[1]) { //ACK
		  mprintf("(sm) ACK cmd=%02x dalalen=%d\n",cmd[1],datalen);
		  send_net(fd,11,rcv_sm.size(),rcv_sm.data());
		  if(datalen==0)
		    gotostage(ST_CMD);
		  else
		    gotostage(ST_DATA);
		}  else if(rcv_sm[0]==0x60) {
		  send_net(fd,11,rcv_sm.size(),rcv_sm.data());
		  gotostage(ST_CMD);
		} else if((rcv_sm[0]&0xf0)==0x60 || (rcv_sm[0]&0xf0)==0x90) {
		  //
		} else {
		  mprintf("(sm) ERROR SW\n");
		}
	      } else {
		send_net(fd,11,rcv_sm.size(),rcv_sm.data());
		gotostage(ST_CMD);
	      }
	    } else
	      if(stage==ST_DATA) {
		int datalen=cmd[4];
		if(sr==datalen) {
		  mprintf("(sm) END DATA\n");
		  if(isresp(cmd[1]))
		    send_net(fd,12,rcv_sm.size(),rcv_sm.data());
		  gotostage(ST_SW2);
		}
	      } else
		if(stage==ST_SW2) {
		  if(sr==1) {
		    if(rcv_sm[0]==cmd[1]) { //ACK
		      mprintf("(sm) ERROR ACK\n");
		    } else if(rcv_sm[0]==0x60) {
		      send_net(fd,13,rcv_sm.size(),rcv_sm.data());
		      gotostage(ST_CMD);
		    } else if((rcv_sm[0]&0xf0)==0x60 || (rcv_sm[0]&0xf0)==0x90) {
		      //
		    } else {
		      mprintf("(sm) ERROR SW2\n");
		    }
		  } else {
		    send_net(fd,13,rcv_sm.size(),rcv_sm.data());
		    gotostage(ST_CMD);
		  }
		} else {
		  mprintf("(sm) unk stage %d\n",stage);
		}

	pthread_mutex_unlock(&mutex);
      } else
	usleep(10);
    }
    shutdown(fd,2);
    mprintf("end thread rcv\n");
  }

  //unsigned char pps[]={0xff,0x00,0xff};//keep default speed (372 clk per bit)

  //bool atrsent=0;

  void proc_net(unsigned char *bf,int r)
  {
    print_current_time_with_ms();

    mprintf("RXNET: ");
    for(int i=0;i<r;i++)
      mprintf("%02x ",bf[i]);
    mprintf("\n");

    if(r<4) {
      mprintf("too small\n");
      return;
    }
    int ii=(bf)[0];
    ii+=((int)(bf[1]))<<8;
    int s=(bf)[2];
    s+=((int)(bf[3]))<<8;
    //fprintf(stderr,"n2=%d n=%d num=%d s=%d\r",n2,n,ii,s);
    if(r>s+4) {
      mprintf("WARN too big\n");
    }
    if(r>=s+4) {
      if(ii==ST_PING) { //PING
	print_current_time_with_ms();
	bf[4+s]=0;
	mprintf("PING TTL=%d '%s'\n",bf[4],bf+5);
	if(bf[4]) {
	  bf[4]--;
	  send_net(fd,ST_PING,s,bf+4);
	}
      } else 
	if(ii==ST_ATR)  {
	  if(usesim) {
	    mprintf("reset sim...\n");
	    usleep(1000);
	    special_set(TIOCM_RTS,1);
	    stage=0;
	    usleep(1000);
	    special_set(TIOCM_RTS,0);
	    usleep(10000);
	  }
	} else
	  if(ii==ST_CMD) { //CMD
	    assert(s==5);
	    memcpy(cmd,bf+4,5);
	    rcv_sm.clear();
	    if(stage!=ST_CMD) {
	      mprintf("WRONG STAGE %d should be %d\n",stage,ST_CMD);
	    }
	    stage=ST_CMD;
	    write_fd_sm(bf+4,s);
	  } else 
	    if(ii==ST_DATA && !isresp(cmd[1])) { //DATA (except for responses)
	      rcv_sm.clear();
	      if(stage!=ST_DATA) {
		mprintf("WRONG STAGE %d should be %d\n",stage,ST_DATA);
	      }
	      write_fd_sm(bf+4,s);
	    } else {
	      for(int i=0;i<s+4;i++)
		mprintf("%02x ",bf[i]);
	      mprintf("\n");
	    }
    }
    else {
      mprintf("WARN too small\n");
    }
  }

  static void* th_ss(void *a_)
  {
    ss_t *a=(ss_t*)a_;
    a->ss();
    return NULL;
  }

  void ss() {
    if(usesim) {
      mprintf("reset sim...\n");
      usleep(1000);
      special_set(TIOCM_RTS,1);
      stage=0;
      usleep(1000);
      special_set(TIOCM_RTS,0);
      usleep(10000);
    }
  
    if(atr.size()) {
      mprintf("send ATR\n"); 
      send_net(fd,ST_ATR,atr.size(),atr.data());
    }

    int n=0;
    while(1) {
      unsigned char bf[3000];
      int r=read(fd,bf+n,2999-n);
      if(r<0) {
	mprintf("error recv : %s\n",strerror(errno));
	break;
      }
      for(int i=0;i<r;i++) {
	print_current_time_with_ms();
	mprintf("rxnet:%02x\n",bf[n+i]);
      }
      if(this->exit) {
	mprintf("ss: exit by exit\n");
	break;
      }
	
      n+=r;
      while(n>=4) {
	int ii=(bf)[0];
	ii+=((int)(bf[1]))<<8;
	int s=(bf)[2];
	s+=((int)(bf[3]))<<8;
	if(n>=4+s) {
	  proc_net(bf,4+s);
	  if(n>4+s) {
	    memmove(bf,bf+4+s,n-4-s);
	  }
	  n-=4+s;
	} else
	  break;
      }
    }
    shutdown(fd,2);
    mprintf("end thread ss\n");
    pthread_join(tid,NULL);
    mprintf("threads joineds. END\n");
    fclose(out);
    out=NULL;
  }
};

list<ss_t*> olds;
  
void launchthss(int fd) {
  ss_t *ss=new ss_t(fd);
  for(auto &it:olds)
    it->exit=1;
  usleep(100000);
  olds.push_back(ss);
}

void main_tcp() {
  unsigned char bf[512];
  struct sockaddr_in a,b;
  socklen_t lb;
  int oii=0;

  int fd2=-1;

  fd2=socket(AF_INET,SOCK_STREAM,0);
  if(fd2<0)
    fprintf(stderr,"error socket : %s\n",strerror(errno));

  int en=1;
  if (setsockopt(fd2, SOL_SOCKET, SO_REUSEADDR, &en, sizeof(int)) < 0)
    fprintf(stderr,"setsockopt(SO_REUSEADDR) failed");
  
  a.sin_family=AF_INET;
  a.sin_port=htons(3333);
  //a.sin_port=htons(atoi(av[1]));
  a.sin_addr.s_addr = htonl (INADDR_ANY);
  
  int r=bind(fd2,(struct sockaddr*)&a,sizeof(a));
  if(r<0)
    fprintf(stderr,"error bind : %s\n",strerror(errno));
  if(r<0)
    perror("bind");

  r=listen(fd2,10);
  if(r<0)
    fprintf(stderr,"error listen : %s\n",strerror(errno));

  int fd=-1;
  while(1) {
    socklen_t lb=sizeof(b);
    printf("accept...\n");
    while((fd=accept(fd2,(struct sockaddr*)&b,&lb))) {
      if(fd<0) {
	fprintf(stderr,"error accept : %s\n",strerror(errno));
	usleep(100000);
	continue;
      }
      printf("new client... fd= %d\n",fd);

      launchthss(fd);
      
    }
  }
  close(fd2);
}

int main(int ac, char **av) {

  signal(2,ex);

  if(ac>1) {
    usesim=1;
    portname=av[1];
  
    inittty();

    pthread_t id;
    pthread_create(&id,NULL,th_rcv_sm,NULL);
  }  

  
  print_current_time_with_ms();
  printf("ok to process\n");

  main_tcp();


  ex(-1);

  return 0;
}
