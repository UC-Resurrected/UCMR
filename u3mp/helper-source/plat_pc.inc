/* plat_pc.inc - PC backend of plat.h, for the UCMR runtime on Windows and Linux. Included from u3_lan_profiles.c
 * at the "LAN x001" spot (inside the .text.ext section pragma) when U3_PLAT_PS4 is NOT defined.
 *
 * v7.267 (docs/WINDOWS-DEPENDENCIES.md section 4.10, phase 5): every OS call goes through the runtime's host function table
 * (gow3_pc/src/uncharted_host.h, runtime_host_api.cpp). u3x_host_bind (u3x_fixes.cpp) reads its address at kind 44
 * through the runtime's own sceKernelOpen / sceKernelRead of /dev/uncharted_host. Up to v7.266 this file found the
 * Windows DLLs itself (a PEB walk to ws2_32 / kernel32 / kernelbase / advapi32 / ucrtbase / user32 / gdi32 and ms_abi
 * calls), which only works on Windows; the table makes those same calls on Windows and the matching ones on Linux,
 * with Winsock's results on both. The functions keep their v7.266 behaviour, including when each one starts to work:
 *
 *   plat_init             nothing (the builder's IMPORTS/NET_IMPORTS/KIMPORTS are relocated by the runtime)
 *   plat_net_pick         x001 probe: when sceNetSocket answers 0x80410133 (ENETUNREACH), X.net[] is pointed at the
 *                         host-socket shims below
 *   plat_sock_*           the table's sockets (raw handle, Winsock error numbers)
 *   plat_resolve          resolve                plat_tls_*     BearSSL, shared with PS4 (plat_tls.inc, part C)
 *   plat_random           random                 plat_alloc / plat_thread / plat_sleep_ms / plat_utc
 *   plat_textbox_open     the runtime's box (a window on Windows, zenity / kdialog on Linux)
 *   plat_handoff          env_take (U3MP_HANDOFF, U3MP_RELAY)        plat_diag_append   diag_append
 *   alloc, thread, resolve and the box bind on first use (plat_sys_bind); sleep, UTC, random and plat_thread_ready
 *   answer only after a bind, as when they were looked up there.
 *
 * Diagnostics: X.room[0] 1 native / 2 host sockets / 3 not available, [1] probe result, [2] startup result, [3] local
 * IPv4, [4] sends, [5] receives, [6] last socket error, [7] last failing call; X.room[14] TLS: 0 ok, 0x1000|BearSSL
 * error, or the socket error. */
#ifndef U3_PLAT_PC_INC
#define U3_PLAT_PC_INC
#define WSA __attribute__((force_align_arg_pointer))
/* v7.228 (#81): the PC glue up to plat_tls.inc lives in region A (region B, .text.ext, had no room left for the voice
 * chat); plat_tls.inc and what follows it stay in region B. */
#pragma clang section text=".text.platpc" rodata=".rodata.platpc"
/* The runtime's host function table: the layout of gow3_pc/src/uncharted_host.h version 1 (u3x_host_bind checks the
 * version and the size); System V functions, Winsock's conventions. */
typedef struct {
 uint32_t version,size;
 int (*startup)(void);
 uint64_t (*socket_)(int,int,int); int (*close_)(uint64_t); int (*bind_)(uint64_t,const void*,int); int (*connect_)(uint64_t,const void*,int);
 uint64_t (*accept_)(uint64_t,void*,int*); int (*listen_)(uint64_t,int);
 int (*send_)(uint64_t,const void*,int,int); int (*recv_)(uint64_t,void*,int,int);
 int (*sendto_)(uint64_t,const void*,int,int,const void*,int); int (*recvfrom_)(uint64_t,void*,int,int,void*,int*);
 int (*setopt_)(uint64_t,int,int,const void*,int); int (*ioctl_)(uint64_t,uint32_t*); int (*getname_)(uint64_t,void*,int*);
 int (*lasterr)(void); int (*resolve)(const char*,uint32_t*);
 void *(*alloc)(uint64_t); int (*thread)(plat_thread_fn,void*,uint64_t); void (*sleep_)(uint32_t); void (*utc)(uint16_t*);
 int (*random)(void*,uint32_t); uint32_t (*env_take)(const char*,char*,uint32_t); int (*diag)(const char*,const char*,uint32_t);
 int (*textbox)(const char*,const char*,const char*,int,char*,uint32_t,volatile uint8_t*,volatile int32_t*);
 const void *(*voice)(void);
} PlatHost;
_Static_assert(sizeof(PlatHost)==208,"uncharted_host.h version 1");
__attribute__((visibility("hidden"))) const void *u3x_host(void);   /* u3x_fixes.cpp: the table, or null (an older runtime) */
#define PH ((const PlatHost*)u3x_host())
typedef struct { uint16_t family,port; uint32_t addr; uint8_t zero[8]; } WinSa;
static uintptr_t ws_ctl_native EXT_BSS;
static struct { int32_t fd; int32_t nbio; } ws_nb[64] EXT_BSS;
static int32_t ws_err(uint32_t where){int e=PH->lasterr();X.room[6]=(uint32_t)e;X.room[7]=where;
 int b=e>=10000?e-10000:e;return (int32_t)(0x80410100u|(uint32_t)(b&0xff));}
static void ws_nbset(int32_t fd,int32_t v){int fr=-1;for(int i=0;i<64;i++){if(ws_nb[i].fd==fd){ws_nb[i].nbio=v;return;}if(!ws_nb[i].fd&&fr<0)fr=i;}if(fr>=0){ws_nb[fr].fd=fd;ws_nb[fr].nbio=v;}}
static int32_t ws_nbget(int32_t fd){for(int i=0;i<64;i++)if(ws_nb[i].fd==fd)return ws_nb[i].nbio;return 0;}
static void ws_nbdrop(int32_t fd){for(int i=0;i<64;i++)if(ws_nb[i].fd==fd){ws_nb[i].fd=0;ws_nb[i].nbio=0;}}
static void sa_in(WinSa *w,const void *a){const SceSockaddrIn *s=(const SceSockaddrIn*)a;clear(w,sizeof(*w));w->family=2;w->port=s->port;w->addr=s->addr;}
static void sa_out(void *a,uint32_t *al,const WinSa *w){if(!a)return;SceSockaddrIn *s=(SceSockaddrIn*)a;clear(s,sizeof(*s));s->len=16;s->family=2;s->port=w->port;s->addr=w->addr;if(al)*al=16;}
/* x003 test builds U3MP_LAN_LOOP=1 / 2 (eb[2] bits 30 / 31): two copies on one PC, on 127.0.0.2 and 127.0.0.3 (127.0.0.1 is treated as "self" by the LAN code). */
static uint32_t ws_loop_self(void){return X.eb[2]&0x40000000u?0x0200007fu:X.eb[2]&0x80000000u?0x0300007fu:0;}
static uint32_t ws_loop_peer(void){return X.eb[2]&0x40000000u?0x0300007fu:0x0200007fu;}
static void ws_loop_dest(WinSa *w){if(!ws_loop_self())return;if(w->addr==0xffffffffu)w->addr=ws_loop_peer();else if(w->addr==0x0100007fu)w->addr=ws_loop_self();}   /* localhost = this copy: its room server listens on its own address */
WSA static int32_t ws_socket(const char *n,int fam,int type,int proto){(void)n;uint64_t f=PH->socket_(fam,type,proto);if(f==~(uint64_t)0)return ws_err(1);return (int32_t)f;}
WSA static int32_t ws_close(int fd){ws_nbdrop(fd);return PH->close_((uint64_t)(int64_t)fd)<0?ws_err(2):0;}
WSA static int32_t ws_bind(int fd,const void *a,uint32_t l){(void)l;WinSa w;sa_in(&w,a);if(ws_loop_self())w.addr=ws_loop_self();return PH->bind_((uint64_t)(int64_t)fd,&w,16)<0?ws_err(3):0;}
WSA static int32_t ws_connect(int fd,const void *a,uint32_t l){(void)l;WinSa w;sa_in(&w,a);ws_loop_dest(&w);if(ws_loop_self()){WinSa me;clear(&me,sizeof(me));me.family=2;me.addr=ws_loop_self();PH->bind_((uint64_t)(int64_t)fd,&me,16);}   /* x012: an unbound loopback client appears as 127.0.0.1, so the room told the host the wrong guest address; a repeat bind just fails */
 if(PH->connect_((uint64_t)(int64_t)fd,&w,16)>=0)return 0;
 int32_t r=ws_err(4);return ((uint32_t)r&0xff)==35?(int32_t)0x80410124u:r;}                /* WSAEWOULDBLOCK on connect = EINPROGRESS */
WSA static int32_t ws_listen(int fd,int n){return PH->listen_((uint64_t)(int64_t)fd,n)<0?ws_err(5):0;}
WSA static int32_t ws_accept(int fd,void *a,uint32_t *al){WinSa w;int l=16;clear(&w,sizeof(w));uint64_t f=PH->accept_((uint64_t)(int64_t)fd,&w,&l);if(f==~(uint64_t)0)return ws_err(6);sa_out(a,al,&w);return (int32_t)f;}
WSA static int32_t ws_send(int fd,const void *b,uint32_t n,int fl){int dw=(fl&0x80)&&!ws_nbget(fd);uint32_t v=1;if(dw)PH->ioctl_((uint64_t)(int64_t)fd,&v);
 int r=PH->send_((uint64_t)(int64_t)fd,b,(int)n,fl&3);int32_t o=r<0?ws_err(7):r;if(dw){v=0;PH->ioctl_((uint64_t)(int64_t)fd,&v);}if(r>0)X.room[4]++;return o;}
WSA static int32_t ws_recv(int fd,void *b,uint32_t n,int fl){int dw=(fl&0x80)&&!ws_nbget(fd);uint32_t v=1;if(dw)PH->ioctl_((uint64_t)(int64_t)fd,&v);
 int r=PH->recv_((uint64_t)(int64_t)fd,b,(int)n,fl&3);int32_t o=r<0?ws_err(8):r;if(dw){v=0;PH->ioctl_((uint64_t)(int64_t)fd,&v);}if(r>0)X.room[5]++;return o;}
WSA static int32_t ws_sendto(int fd,const void *b,uint32_t n,int fl,const void *a,uint32_t l){(void)l;(void)fl;WinSa w;sa_in(&w,a);ws_loop_dest(&w);int r=PH->sendto_((uint64_t)(int64_t)fd,b,(int)n,0,&w,16);if(r<0)return ws_err(9);X.room[4]++;return r;}
WSA static int32_t ws_recvfrom(int fd,void *b,uint32_t n,int fl,void *a,uint32_t *al){(void)fl;WinSa w;int l=16;clear(&w,sizeof(w));int r=PH->recvfrom_((uint64_t)(int64_t)fd,b,(int)n,0,&w,&l);
 if(r<0){int32_t e=ws_err(10);return ((uint32_t)e&0xff)==54?(int32_t)0x80410123u:e;}       /* WSAECONNRESET after an ICMP unreachable: nothing to read */
 sa_out(a,al,&w);X.room[5]++;return r;}
WSA static int32_t ws_setopt(int fd,int lv,int opt,const void *v,uint32_t l){
 if(lv==0xffff&&opt==0x1200){uint32_t on=*(const int32_t*)v?1:0;ws_nbset(fd,(int32_t)on);return PH->ioctl_((uint64_t)(int64_t)fd,&on)<0?ws_err(11):0;}
 if(lv==0xffff&&(opt==0x1005||opt==0x1006)){int32_t ms=*(const int32_t*)v/1000;return PH->setopt_((uint64_t)(int64_t)fd,lv,opt,&ms,4)<0?ws_err(12):0;}   /* us -> ms */
 return PH->setopt_((uint64_t)(int64_t)fd,lv,opt,v,(int)l)<0?ws_err(12):0;}
static const char ws_dig[]="0123456789";
WSA static int32_t ws_ctl_info(int code,void *out){
 if(code!=14)return ws_ctl_native?((int32_t(*)(int,void*))ws_ctl_native)(code,out):-1;
 uint64_t f=ws_loop_self()?0:PH->socket_(2,2,0);if(f==~(uint64_t)0)return ws_err(13);
 WinSa w;clear(&w,sizeof(w));w.family=2;w.port=be16(9);w.addr=0x010a0a0au;                  /* 10.10.10.1: no packet is sent, this only picks the route */
 int l=16,ok=1;if(ws_loop_self())w.addr=ws_loop_self();else{ok=PH->connect_(f,&w,16)>=0&&PH->getname_(f,&w,&l)>=0;PH->close_(f);}if(!ok||!w.addr)return ws_err(13);
 X.room[3]=w.addr;char *o=(char*)out;for(int i=0;i<4;i++){uint32_t v=(w.addr>>(i*8))&0xff;if(v>=100)*o++=ws_dig[v/100];if(v>=10)*o++=ws_dig[v/10%10];*o++=ws_dig[v%10];if(i<3)*o++='.';}*o=0;return 0;}
static int plat_init(uintptr_t base){(void)base;return 0;}
/* Run before the first socket of each module; decides once. */
EXT static void plat_net_pick(void){
 if(X.room[0]||!X.net[0])return;
 if(X.eb[2]&0x20000000u){X.room[0]=1;return;}                                             /* U3MP_LAN_WINSOCK=0 */
 int32_t fd=((int32_t(*)(const char*,int,int,int))X.net[0])("u3mp-probe",2,2,0);X.room[1]=(uint32_t)fd;
 if(fd>=0){((int32_t(*)(int))X.net[1])(fd);X.room[0]=1;return;}
 if((uint32_t)fd!=0x80410133u||!PH){X.room[0]=3;return;}
 int s=PH->startup();X.room[2]=(uint32_t)s;if(s){X.room[0]=3;return;}
 ws_ctl_native=X.net[6];
 X.net[0]=(uintptr_t)&ws_socket;X.net[1]=(uintptr_t)&ws_close;X.net[2]=(uintptr_t)&ws_bind;X.net[3]=(uintptr_t)&ws_sendto;X.net[4]=(uintptr_t)&ws_recvfrom;
 X.net[5]=(uintptr_t)&ws_setopt;X.net[6]=(uintptr_t)&ws_ctl_info;X.net[7]=(uintptr_t)&ws_connect;X.net[8]=(uintptr_t)&ws_listen;X.net[9]=(uintptr_t)&ws_accept;
 X.net[10]=(uintptr_t)&ws_send;X.net[11]=(uintptr_t)&ws_recv;X.room[0]=2;}
static int plat_sock_ready(void){return X.room[0]==2;}   /* the thread code below runs on the raw host sockets */

/* ---- raw sockets: the table's functions, exactly as the thread code called the ws2_32 ones ---- */
static plat_sock plat_sock_open(int family,int type,int proto){return PH->socket_(family,type,proto);}
static int plat_sock_close(plat_sock f){return PH->close_(f);}
static int plat_sock_connect(plat_sock f,uint32_t addr,uint16_t port_be){WinSa a;clear(&a,sizeof(a));a.family=2;a.port=port_be;a.addr=addr;return PH->connect_(f,&a,16);}
static int plat_sock_send(plat_sock f,const void *b,int n,int flags){return PH->send_(f,b,n,flags);}
static int plat_sock_recv(plat_sock f,void *b,int n,int flags){return PH->recv_(f,b,n,flags);}
static int plat_sock_sendto(plat_sock f,const void *b,int n,uint32_t addr,uint16_t port_be){WinSa a;clear(&a,sizeof(a));a.family=2;a.port=port_be;a.addr=addr;return PH->sendto_(f,b,n,0,&a,16);}
static int plat_sock_recvfrom(plat_sock f,void *b,int n){return PH->recvfrom_(f,b,n,0,0,0);}
static int plat_sock_timeout(plat_sock f,int opt,int32_t ms){return PH->setopt_(f,0xffff,opt,&ms,4);}
static int plat_sock_nonblock(plat_sock f,uint32_t on){return PH->ioctl_(f,&on);}   /* FIONBIO */
static int plat_sock_err(void){return PH->lasterr();}
#define PLAT_EWOULDBLOCK 10035   /* WSAEWOULDBLOCK (the table reports Winsock numbers on Linux too) */
#define PLAT_ETIMEDOUT 10060     /* WSAETIMEDOUT */

/* ---- memory, threads, time, names: bound once (as v7.266 looked them up once) ---- */
static volatile uint8_t pc_bound EXT_BSS;
static int plat_sys_bind(void){if(pc_bound)return 1;if(!PH)return 0;pc_bound=1;return 1;}
static int plat_thread_ready(void){return pc_bound!=0;}
/* v7.228 #81: the voice chat table of the PC runtime (gow3_pc u3voice.h); null when the runtime has none. */
__attribute__((visibility("hidden"),noinline)) void *u3h_voice_api(void){return PH?(void*)PH->voice():0;}
static int plat_resolve(const char *host,uint32_t *addr){if(!plat_sys_bind())return -1;return PH->resolve(host,addr);}
static void *plat_alloc(uint64_t bytes){if(!plat_sys_bind())return 0;return PH->alloc(bytes);}   /* v7.102: bind on first use (LAN had none) */
static int plat_thread(plat_thread_fn fn,void *arg,uint64_t stack){if(!plat_sys_bind())return -1;return PH->thread(fn,arg,stack);}
static void plat_sleep_ms(uint32_t ms){if(pc_bound)PH->sleep_(ms);}
static int plat_utc(uint16_t st[8]){if(!pc_bound)return -1;PH->utc(st);return 0;}

static int plat_random(void *b,uint32_t n){if(!pc_bound)return -1;return PH->random(b,n);}
#pragma clang section text=".text.ext" rodata=".rodata.ext"   /* back to region B for plat_tls.inc (BearSSL) */
#include "plat_tls.inc"
#pragma clang section text=".text.ext" rodata=".rodata.ext"

/* ---- v7.59 lh01: plat_handoff (plat.h): the variable's value, removed from the process (the C runtime's copy and the
 * environment block on Windows), called once on the game thread (rp_tick). Nothing here logs the value. ---- */
static int plat_env_take(const char *var,char *out,uint32_t cap){
 if(!out||cap<2)return 0;if(!PH){clear(out,cap);return 0;}
 return (int)PH->env_take(var,out,cap);}
/* v7.90 rf24: append n bytes to path (relative to the working folder) from any thread, also while the game thread is
 * stuck. 0 when it could not be written. */
static int plat_diag_append(const char *path,const char *text,uint32_t n){return PH?PH->diag(path,text,n):0;}
static int plat_handoff(char *out,uint32_t cap){static const char var[]="U3MP_HANDOFF";return plat_env_take(var,out,cap);}
static int plat_relay(char *out,uint32_t cap){static const char var[]="U3MP_RELAY";return plat_env_take(var,out,cap);}   /* v7.89 rf23 */

/* ---- x035: one box with an edit field, so a value can be pasted (the game's text input has no clipboard) ---- */
static int plat_textbox_open(const char *title,const char *label,const char *initial,int password,char *out,uint32_t cap,volatile uint8_t *res,volatile int32_t *busy){
 if(!plat_sys_bind())return 0;
 return PH->textbox(title,label,initial,password,out,cap,res,busy);}
#endif
