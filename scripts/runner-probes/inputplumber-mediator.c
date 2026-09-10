/* Root-side pre-forward InputPlumber D-Bus reference monitor.
 *
 * This is intentionally a small sd-bus peer, not a post-hoc xdg-dbus-proxy
 * audit.  Every candidate method call is decoded and authorized before an
 * independently opened system-bus connection can see it.  The process is
 * single-client and request-scoped; the broker supplies a root-held topology
 * snapshot containing the one permitted composite path.
 */
#include <systemd/sd-bus.h>
#include <systemd/sd-id128.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define IP "org.shadowblip.InputPlumber"
#define ROOT "/org/shadowblip/InputPlumber"
#define MANAGER ROOT "/Manager"
#define IM "org.shadowblip.InputManager"
#define IC "org.shadowblip.Input.CompositeDevice"
#define IT "org.shadowblip.Input.Target"
#define PROP "org.freedesktop.DBus.Properties"
#define OM "org.freedesktop.DBus.ObjectManager"
#define INTRO "org.freedesktop.DBus.Introspectable"
#define DBUS "org.freedesktop.DBus"
#define MAX_TARGETS 4
#define MAX_PATH 512

struct state {
 sd_bus *peer, *system;
 FILE *audit;
 char source[MAX_PATH];
 char targets[MAX_TARGETS][MAX_PATH];
 char owner[64];
 size_t ntargets;
 bool mutations;
 char arguments[1024];
 uint64_t decision;
};
static volatile sig_atomic_t stop_requested;
static void on_signal(int sig) { (void)sig; stop_requested = 1; }
static bool same(const char *a,const char *b) { return a && b && strcmp(a,b)==0; }
static bool learned(struct state *s,const char *p) {
 for (size_t i=0;i<s->ntargets;i++) if (same(s->targets[i],p)) return true;
 return false;
}
static bool safe_path(struct state *s,const char *p) {
 return same(p,ROOT)||same(p,MANAGER)||same(p,s->source)||learned(s,p);
}
static void forget_target(struct state *s,const char *p) {
 for(size_t i=0;i<s->ntargets;i++)if(same(s->targets[i],p)){
  if(i+1<s->ntargets)memmove(s->targets[i],s->targets[i+1],(s->ntargets-i-1)*sizeof s->targets[0]);
  s->ntargets--; return;
 }
}
static int log_decision(struct state *s,sd_bus_message *m,bool allow,const char *why) {
 const char *p=sd_bus_message_get_path(m),*i=sd_bus_message_get_interface(m),*n=sd_bus_message_get_member(m),*sig=sd_bus_message_get_signature(m,true);
 int r=fprintf(s->audit,"{\"schema\":\"factory-inputplumber-preforward-decision/v1\",\"seq\":%llu,\"allow\":%s,\"path\":\"%s\",\"interface\":\"%s\",\"member\":\"%s\",\"signature\":\"%s\",\"arguments\":\"%s\",\"reason\":\"%s\"}\n",(unsigned long long)++s->decision,allow?"true":"false",p?p:"",i?i:"",n?n:"",sig?sig:"",s->arguments,why);
 if (r<0||fflush(s->audit)<0||fsync(fileno(s->audit))<0) return -EIO;
 return 0;
}
static int deny(struct state *s,sd_bus_message *m,const char *why) {
 int r=log_decision(s,m,false,why); if(r<0)return r;
 return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"mediator denied: %s",why);
}
static int read_one_string_array(sd_bus_message *m,const char *required,bool empty_ok) {
 int r=sd_bus_message_enter_container(m,'a',"s"); if(r<=0)return -EINVAL;
 const char *v=NULL; size_t count=0;
 while((r=sd_bus_message_read(m,"s",&v))>0) { count++; if(!required||!same(v,required))return -EPERM; }
 if(r<0||sd_bus_message_exit_container(m)<0)return -EINVAL;
 return (count==1||(empty_ok&&count==0))?0:-EPERM;
}
static int authorize_properties(sd_bus_message *m) {
 const char *iface=NULL,*property=NULL; int r;
 if((r=sd_bus_message_read(m,"ss",&iface,&property))<=0)return -EINVAL;
 if(!same(iface,IC)||!same(property,"InterceptMode"))return -EPERM;
 if((r=sd_bus_message_enter_container(m,'v',"u"))<=0)return -EINVAL;
 uint32_t value=0; if(sd_bus_message_read(m,"u",&value)<=0||value!=1)return -EPERM;
 if(sd_bus_message_exit_container(m)<0||!sd_bus_message_at_end(m,true))return -EINVAL;
 return 0;
}
static int authorize(struct state *s,sd_bus_message *m,bool *create) {
 const char *dest=sd_bus_message_get_destination(m),*path=sd_bus_message_get_path(m),*iface=sd_bus_message_get_interface(m),*member=sd_bus_message_get_member(m),*sig=sd_bus_message_get_signature(m,true);
 *create=false; s->arguments[0]=0;
 if(same(iface,IT)&&same(member,"InputEvent"))return -EPERM;
 if(same(dest,DBUS)&&same(path,"/org/freedesktop/DBus")&&same(iface,DBUS)) {
  const char *name=NULL;
  if(same(member,"GetNameOwner")&&same(sig,"s")&&sd_bus_message_read(m,"s",&name)>0&&same(name,IP)&&sd_bus_message_at_end(m,true)){strcpy(s->arguments,IP);return 0;}
  if(same(member,"GetConnectionUnixProcessID")&&same(sig,"s")&&sd_bus_message_read(m,"s",&name)>0&&s->owner[0]&&same(name,s->owner)&&sd_bus_message_at_end(m,true)){strcpy(s->arguments,s->owner);return 0;}
  return -EPERM;
 }
 if(!same(dest,IP)||!safe_path(s,path))return -EPERM;
 if(same(iface,OM)&&same(member,"GetManagedObjects")&&same(path,ROOT)&&same(sig,""))return 0;
 if(same(iface,INTRO)&&same(member,"Introspect")&&same(sig,""))return 0;
 if(same(iface,PROP)&&(same(member,"Get")||same(member,"GetAll"))) {
  const char *a=NULL,*b=NULL; int r;
  if(same(member,"Get")) { if(!same(sig,"ss")||(r=sd_bus_message_read(m,"ss",&a,&b))<=0||!b)return -EINVAL; }
  else if(!same(sig,"s")||(r=sd_bus_message_read(m,"s",&a))<=0)return -EINVAL;
  if(!(same(a,IM)||same(a,IC)||same(a,IT))||!sd_bus_message_at_end(m,true))return -EPERM;
  if(b && !(same(b,"InterceptMode")||same(b,"TargetDevices")||same(b,"SourceDevicePaths")||same(b,"DeviceType")||same(b,"Name")||same(b,"PersistentId")||same(b,"SupportedTargetDeviceIds")||same(b,"SupportedTargetDevices")))return -EPERM;
  snprintf(s->arguments,sizeof s->arguments,"%s%s%s",a,b?":":"",b?b:""); return 0;
 }
 if(!s->mutations)return -EPERM;
 if(same(iface,IM)&&same(member,"CreateTargetDevice")&&same(path,MANAGER)&&same(sig,"s")) {
  const char *kind=NULL; if(s->ntargets>=MAX_TARGETS||sd_bus_message_read(m,"s",&kind)<=0||!same(kind,"xb360")||!sd_bus_message_at_end(m,true))return -EPERM;
  strcpy(s->arguments,"xb360"); *create=true; return 0;
 }
 if(same(iface,IM)&&same(member,"StopTargetDevice")&&same(path,MANAGER)&&same(sig,"s")) {
  const char *target=NULL; if(sd_bus_message_read(m,"s",&target)<=0||!learned(s,target)||!sd_bus_message_at_end(m,true))return -EPERM; strcpy(s->arguments,target); return 0;
 }
 if(same(iface,IC)&&same(path,s->source)&&same(member,"SetTargetDevices")&&same(sig,"as")){int r=read_one_string_array(m,"xb360",true);if(r==0)strcpy(s->arguments,"[xb360-or-empty]");return r;}
 if(same(iface,IC)&&same(path,s->source)&&same(member,"SetInterceptActivation")&&same(sig,"ass")) {
  int r=read_one_string_array(m,"Guide",false); const char *target=NULL;
  if(r<0||sd_bus_message_read(m,"s",&target)<=0||!same(target,"Guide")||!sd_bus_message_at_end(m,true))return -EPERM;
  strcpy(s->arguments,"[Guide],Guide"); return 0;
 }
 if(same(iface,PROP)&&same(member,"Set")&&same(path,s->source)&&same(sig,"ssv")){int r=authorize_properties(m);if(r==0)strcpy(s->arguments,"CompositeDevice.InterceptMode=1");return r;}
 return -EPERM;
}
static int forward_call(struct state *s,sd_bus_message *incoming,bool create) {
 sd_bus_message *call=NULL,*reply=NULL,*out=NULL; sd_bus_error error=SD_BUS_ERROR_NULL;
 const char *dest=sd_bus_message_get_destination(incoming),*path=sd_bus_message_get_path(incoming),*iface=sd_bus_message_get_interface(incoming),*member=sd_bus_message_get_member(incoming);
 int r=sd_bus_message_new_method_call(s->system,&call,dest,path,iface,member); if(r<0)goto done;
 r=sd_bus_message_rewind(incoming,true); if(r<0)goto done;
 r=sd_bus_message_copy(call,incoming,true); if(r<0)goto done;
 r=sd_bus_call(s->system,call,30000000,&error,&reply);
 if(r<0) { r=sd_bus_reply_method_error(incoming,&error); goto done; }
 if(create) {
  const char *target=NULL; r=sd_bus_message_read(reply,"s",&target);
  if(r<=0||!target||target[0]!='/'||strlen(target)>=MAX_PATH||learned(s,target)) { r=sd_bus_reply_method_errorf(incoming,SD_BUS_ERROR_FAILED,"mediator rejected malformed/reused target reply"); goto done; }
  strcpy(s->targets[s->ntargets++],target); sd_bus_message_rewind(reply,true);
 } else if(same(iface,IM)&&same(member,"StopTargetDevice")) {
  const char *target=NULL; sd_bus_message_rewind(incoming,true);
  if(sd_bus_message_read(incoming,"s",&target)>0)forget_target(s,target);
 } else if(same(dest,DBUS)&&same(member,"GetNameOwner")) {
  const char *owner=NULL; r=sd_bus_message_read(reply,"s",&owner);
  if(r<=0||!owner||owner[0]!=':'||strlen(owner)>=sizeof s->owner) { r=sd_bus_reply_method_errorf(incoming,SD_BUS_ERROR_FAILED,"mediator rejected malformed owner reply"); goto done; }
  strcpy(s->owner,owner); sd_bus_message_rewind(reply,true);
 }
 r=sd_bus_message_new_method_return(incoming,&out); if(r<0)goto done;
 r=sd_bus_message_copy(out,reply,true); if(r<0)goto done;
 r=sd_bus_send(s->peer,out,NULL);
done:
 sd_bus_error_free(&error); sd_bus_message_unref(call); sd_bus_message_unref(reply); sd_bus_message_unref(out); return r;
}
static int filter(sd_bus_message *m,void *userdata,sd_bus_error *ret_error) {
 (void)ret_error; struct state *s=userdata;
 uint8_t type=0; if(sd_bus_message_get_type(m,&type)<0||type!=(uint8_t)SD_BUS_MESSAGE_METHOD_CALL)return 0;
 const char *iface=sd_bus_message_get_interface(m),*member=sd_bus_message_get_member(m); s->arguments[0]=0;
 if(same(iface,DBUS)&&same(member,"Hello")) { log_decision(s,m,true,"fixed safe bus hello"); return sd_bus_reply_method_return(m,"s",":1.1"); }
 if(same(iface,DBUS)&&same(member,"AddMatch")) { log_decision(s,m,false,"signals are not delegated by mediation profile"); return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"mediator denies signal subscriptions"); }
 bool create=false; int r=authorize(s,m,&create);
 if(r<0)return deny(s,m,"path/interface/member/signature/arguments outside request policy");
 r=log_decision(s,m,true,"authorized before system-bus forward"); if(r<0)return r;
 return forward_call(s,m,create);
}
static int load_source(const char *path,char out[MAX_PATH]) {
 FILE *f=fopen(path,"re"); if(!f)return -errno; char buf[2048]; size_t n=fread(buf,1,sizeof buf-1,f); fclose(f); if(n==0||n>=sizeof buf-1)return -EINVAL; buf[n]=0;
 char *p=strstr(buf,"\"composite\":\""); if(!p)return -EINVAL; p+=strlen("\"composite\":\""); char *e=strchr(p,'\"'); if(!e||(size_t)(e-p)>=MAX_PATH||*p!='/')return -EINVAL; memcpy(out,p,(size_t)(e-p));out[e-p]=0; return 0;
}
static int listen_socket(const char *path) {
 int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0); if(fd<0)return -errno;
 struct sockaddr_un sa={.sun_family=AF_UNIX}; if(strlen(path)>=sizeof sa.sun_path){close(fd);return -ENAMETOOLONG;} strcpy(sa.sun_path,path); unlink(path);
 if(bind(fd,(struct sockaddr*)&sa,sizeof sa)<0||chmod(path,0660)<0||listen(fd,1)<0){int e=-errno;close(fd);unlink(path);return e;} return fd;
}
int main(int argc,char **argv) {
 if(argc!=6||strcmp(argv[1],"--mutations")!=0) { fprintf(stderr,"usage: %s --mutations yes|no SOCKET SNAPSHOT AUDIT\n",argv[0]); return 64; }
 struct state s={.mutations=strcmp(argv[2],"yes")==0}; if(!s.mutations&&strcmp(argv[2],"no")!=0)return 64;
 if(load_source(argv[4],s.source)<0)return 65;
 s.audit=fopen(argv[5],"wxe"); if(!s.audit)return 66; if(chmod(argv[5],0400)<0)return 66;
 signal(SIGTERM,on_signal); signal(SIGINT,on_signal); signal(SIGHUP,on_signal);
 int listener=listen_socket(argv[3]); if(listener<0)return 67;
 int client=-1;
 while(!stop_requested) {
  client=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);
  if(client>=0)break;
  if(errno!=EAGAIN&&errno!=EINTR)return 68;
  struct pollfd p={.fd=listener,.events=POLLIN}; if(poll(&p,1,250)<0&&errno!=EINTR)return 68;
 }
 close(listener); if(client<0){unlink(argv[3]);return 0;}
 sd_id128_t id; int r=sd_id128_randomize(&id); if(r<0)goto done;
 r=sd_bus_new(&s.peer); if(r<0)goto done;
 r=sd_bus_set_fd(s.peer,client,client); if(r<0)goto done; client=-1;
 r=sd_bus_set_server(s.peer,1,id); if(r<0)goto done;
 r=sd_bus_set_anonymous(s.peer,1); if(r<0)goto done;
 r=sd_bus_add_filter(s.peer,NULL,filter,&s); if(r<0)goto done;
 r=sd_bus_start(s.peer); if(r<0)goto done;
 r=sd_bus_open_system(&s.system); if(r<0)goto done;
 while(!stop_requested) {
  do r=sd_bus_process(s.peer,NULL); while(r>0);
  if(r<0||sd_bus_is_open(s.peer)<=0)break;
  r=sd_bus_wait(s.peer,500000); if(r<0&&r!=-EINTR)break;
 }
 r=(r<0)?r:0;
done:
 if(client>=0)close(client);
 sd_bus_flush_close_unref(s.peer); sd_bus_flush_close_unref(s.system);
 if(s.audit){fflush(s.audit);fsync(fileno(s.audit));fclose(s.audit);}
 unlink(argv[3]);
 if(r<0){fprintf(stderr,"inputplumber-mediator: %s\n",strerror(-r));return 69;}
 return 0;
}
