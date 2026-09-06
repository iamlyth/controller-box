/* Independent read-only physical-to-four-target evdev observer. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TARGETS 4
#define NODES (TARGETS + 1)
#define QUIET_TAIL_MS 1000
static const char *physical_path, *physical_name, *fixture_path;
static const char *target_path[TARGETS], *target_name[TARGETS];
static int target_count, selected = -1, clear_mode, window_ms = 60000;
static int etype=EV_KEY, ecode=BTN_SOUTH, evalue=1;
struct reader { unsigned char pending[sizeof(struct input_event)]; size_t used; };
static void fail(const char *s){fprintf(stderr,"routing-observer: FAIL: %s\n",s);exit(1);}
static long long event_us(const struct timeval *v){return (long long)v->tv_sec*1000000LL+v->tv_usec;}
static long long realtime_us(void){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return (long long)t.tv_sec*1000000LL+t.tv_nsec/1000;}
static long long monotonic_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000LL+t.tv_nsec/1000000;}
static int match(const struct input_event *e){return e->type==etype&&e->code==ecode&&e->value==evalue;}
static void raw_hex(const struct input_event *e){const unsigned char *p=(const unsigned char*)e;for(size_t i=0;i<sizeof(*e);i++)printf("%02x",p[i]);}
static int identity(int fd,const char *want,const char *label){char n[256]={0};if(!want||ioctl(fd,EVIOCGNAME(sizeof(n)),n)<0||strcmp(n,want)){fprintf(stderr,"routing-observer: identity mismatch %s\n",label);return 0;}printf("routing-observer: identity node=%s name=%s\n",label,n);return 1;}
static int open_node(const char *p,const char *name,const char *label){int fd=open(p,O_RDONLY|O_NONBLOCK|O_CLOEXEC);unsigned char b[1];if(fd<0||ioctl(fd,EVIOCGBIT(0,sizeof(b)),b)<0||!identity(fd,name,label))fail("cannot open/identify authoritative evdev node");return fd;}
static int name_only(const char *p){int fd=open(p,O_RDONLY|O_CLOEXEC);char n[256]={0};if(fd<0||ioctl(fd,EVIOCGNAME(sizeof(n)),n)<0)return 1;puts(n);close(fd);return 0;}

static int fixture(void){
 FILE *f=fopen(fixture_path,"r");if(!f)fail("fixture unavailable");char line[512],dev[64];long long baseline=1000,pts[TARGETS]={-1,-1,-1,-1},last=-1,ts;int hit[TARGETS]={0},ty,co,va,slot;
 while(fgets(line,sizeof(line),f)){
  if(sscanf(line,"baseline %lld",&baseline)==1)continue;
  if(sscanf(line,"%63s event %d %d %d ts %lld",dev,&ty,&co,&va,&ts)!=5)continue;
  if(ts<baseline)continue;
  if(!strcmp(dev,"clear-target0")||!strcmp(dev,"clear-target1")||!strcmp(dev,"clear-target2")||!strcmp(dev,"clear-target3"))fail("post-clear target activity");
  if(sscanf(dev,"physical%d",&slot)==1){if(slot<0||slot>=TARGETS||ty!=etype||co!=ecode||va!=evalue||ts<=last)fail("physical event does not match expected type/code/value or was reused");pts[slot]=ts;last=ts;}
  else if(sscanf(dev,"target%d",&slot)==1){if(slot<0||slot>=TARGETS||pts[slot]<baseline||ty!=etype||co!=ecode||va!=evalue||ts<pts[slot])fail("target event without preceding physical event (direct injection/synthetic routing rejected)");hit[slot]=1;}
 }
 fclose(f);for(int i=0;i<TARGETS;i++)if(!hit[i])fail("all selected targets were not independently observed");puts("routing-observer: PASS (four concurrent-target windows, bounded quiet tails, no leakage)");return 0;
}

static void process_event(const struct input_event *e,int node,long long baseline,long long *matched,int *fresh){
 if(e->type==EV_SYN)return;
 long long t=event_us(&e->time);
 printf("routing-observer: RAW node=%d ts=%lld type=%u code=%u value=%d bytes=",node,t,e->type,e->code,e->value);raw_hex(e);putchar('\n');
 if(t<baseline)return;
 *fresh=1;if(match(e))*matched=t;
}

/* evdev normally returns whole records, but stream interception and unusual
 * kernels may split one. Preserve the tail and reconstruct before parsing. */
static void drain(int fd,int node,long long baseline,long long *matched,int *fresh,struct reader *state){
 unsigned char buf[4096];
 for(;;){
  ssize_t n=read(fd,buf,sizeof(buf));
  if(n==0){if(state->used)fail("evdev ended with truncated input_event");return;}
  if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)return;if(errno==EINTR)continue;fail("evdev read failed");}
  size_t off=0;
  if(state->used){
   size_t need=sizeof(struct input_event)-state->used;
   size_t take=(size_t)n<need?(size_t)n:need;
   memcpy(state->pending+state->used,buf,take);state->used+=take;off+=take;
   if(state->used==sizeof(struct input_event)){struct input_event e;memcpy(&e,state->pending,sizeof(e));process_event(&e,node,baseline,matched,fresh);state->used=0;}
  }
  while((size_t)n-off>=sizeof(struct input_event)){struct input_event e;memcpy(&e,buf+off,sizeof(e));process_event(&e,node,baseline,matched,fresh);off+=sizeof(e);}
  if(off<(size_t)n){state->used=(size_t)n-off;memcpy(state->pending,buf+off,state->used);}
 }
}

static int live(void){
 if(!physical_path||!physical_name||target_count!=TARGETS)fail("one physical and exactly four target nodes are required");
 int fd[NODES];fd[0]=open_node(physical_path,physical_name,"physical");for(int i=0;i<TARGETS;i++){char l[32];snprintf(l,sizeof(l),"target-%d",i);fd[i+1]=open_node(target_path[i],target_name[i],l);}
 struct pollfd p[NODES];struct reader readers[NODES];memset(readers,0,sizeof(readers));for(int i=0;i<NODES;i++){p[i].fd=fd[i];p[i].events=POLLIN;}
 long long baseline=realtime_us(),deadline=monotonic_ms()+window_ms,quiet_deadline=-1,pm=-1,tm[TARGETS]={-1,-1,-1,-1},result_pm=-1,result_tm=-1;int pf=0,tf[TARGETS]={0};
 printf("routing-observer: WINDOW baseline=%lld selected=%d clear=%s duration_ms=%d quiet_tail_ms=%d\n",baseline,selected,clear_mode?"true":"false",window_ms,QUIET_TAIL_MS);
 while(monotonic_ms()<deadline){
  long long now=monotonic_ms(),end=(quiet_deadline>=0&&quiet_deadline<deadline)?quiet_deadline:deadline;int timeout=(int)(end-now);if(timeout>100)timeout=100;if(timeout<0)timeout=0;
  int r=poll(p,NODES,timeout);if(r<0){if(errno==EINTR)continue;fail("poll failed");}
  if(r>0)for(int i=0;i<NODES;i++){
   if(p[i].revents&(POLLERR|POLLHUP|POLLNVAL))fail("evdev node became unavailable during observation");
   if(p[i].revents&POLLIN){int fresh=0;long long m=-1;drain(fd[i],i-1,baseline,&m,&fresh,&readers[i]);if(i==0){pf|=fresh;if(m>=0)pm=m;}else{tf[i-1]|=fresh;if(m>=0)tm[i-1]=m;}}
  }
  if(clear_mode){for(int i=0;i<TARGETS;i++)if(tf[i])fail("post-clear activity on a target node");continue;}
  for(int i=0;i<TARGETS;i++)if(i!=selected&&tf[i])fail("cross-target leakage in selected window, including quiet tail");
  if(quiet_deadline<0&&pm>=0&&tm[selected]>=pm){result_pm=pm;result_tm=tm[selected];quiet_deadline=monotonic_ms()+QUIET_TAIL_MS;printf("routing-observer: QUIET-TAIL selected=%d start_ms=%lld duration_ms=%d\n",selected,monotonic_ms(),QUIET_TAIL_MS);}
  if(quiet_deadline>=0&&monotonic_ms()>=quiet_deadline)break;
 }
 for(int i=0;i<NODES;i++){if(readers[i].used)fail("observation ended with partial input_event");close(fd[i]);}
 if(pm<0)fail("no fresh matching physical event");
 if(clear_mode){printf("routing-observer: RESULT clear source_event_us=%lld all_targets_silent=true full_window=true read_only=true\n",pm);return 0;}
 if(result_pm<0||result_tm<result_pm||result_tm-result_pm>2000000)fail("selected target did not receive correlated event");for(int i=0;i<TARGETS;i++)if(i!=selected&&tf[i])fail("non-selected target was not silent");
 printf("routing-observer: RESULT selected=%d source_event_us=%lld target_event_us=%lld nonselected_events=0 read_only=true quiet_tail_ms=%d\n",selected,result_pm,result_tm,QUIET_TAIL_MS);return 0;
}
int main(int argc,char **argv){for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--name-only")&&++i<argc)return name_only(argv[i]);else if(!strcmp(argv[i],"--fixture")&&++i<argc)fixture_path=argv[i];else if(!strcmp(argv[i],"--physical-device")&&++i<argc)physical_path=argv[i];else if(!strcmp(argv[i],"--physical-name")&&++i<argc)physical_name=argv[i];else if(!strcmp(argv[i],"--target-device")&&++i<argc){if(target_count>=TARGETS)return 2;target_path[target_count++]=argv[i];}else if(!strcmp(argv[i],"--target-name")&&++i<argc){int n=0;while(n<TARGETS&&target_name[n])n++;if(n>=TARGETS)return 2;target_name[n]=argv[i];}else if(!strcmp(argv[i],"--selected")&&++i<argc)selected=atoi(argv[i]);else if(!strcmp(argv[i],"--clear"))clear_mode=1;else if(!strcmp(argv[i],"--type")&&++i<argc)etype=strtol(argv[i],0,0);else if(!strcmp(argv[i],"--code")&&++i<argc)ecode=strtol(argv[i],0,0);else if(!strcmp(argv[i],"--value")&&++i<argc)evalue=strtol(argv[i],0,0);else if(!strcmp(argv[i],"--window")&&++i<argc)window_ms=atoi(argv[i])*1000;else return 2;}if(fixture_path)return fixture();if(!clear_mode&&(selected<0||selected>=TARGETS))return 2;return live();}
