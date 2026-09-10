#include "runtime.h"
void rt_services_boot();
#include <SDL2/SDL.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>
struct Fake{std::string name;bool cls=false;};
static std::unordered_map<std::string,Obj> classes;
static std::unordered_map<Obj,HostData> hosts;
static std::unordered_map<std::string,void*> methods;
static std::unordered_map<void*,int> refs;
static int notices=0,disconnects=0,authcalls=0,buttoncalls=0,axiscalls=0,pausecalls=0;
static bool lastpressed=false;
static float lastaxis=0;
struct Desc{uintptr_t reserved,size;};
struct Block{void* isa;uint32_t flags,reserved;void* invoke;Desc* desc;};
void*rt_class(const char*n){auto&i=classes[n];if(!i)i=new Fake{n,true};return i;}
Obj rt_new(const char*n){Obj o=new Fake{n,false};hosts[o];return o;}
Obj rt_string(const char*s){Obj o=rt_new("NSString");hosts[o].text=s?s:"";return o;}
const char*rt_utf8(Obj o){return o?hosts[o].text.c_str():"";}
HostData&rt_host(Obj o){return hosts[o];}
const char*rt_class_name(Obj o){return static_cast<Fake*>(o)->name.c_str();}
void rt_method(const char*c,const char*s,void*p,bool cls){methods[std::string(cls?"+":"-")+c+" "+s]=p;}
static Obj same(Obj o,Sel){return o;}
extern "C" void*rt_lookup(Obj o,Sel s){auto*f=static_cast<Fake*>(o);if(!strcmp(s,"retain")||!strcmp(s,"release")||!strcmp(s,"autorelease"))return reinterpret_cast<void*>(same);auto i=methods.find(std::string(f->cls?"+":"-")+f->name+" "+s);if(i==methods.end()){fprintf(stderr,"missing %s %s\n",f->name.c_str(),s);abort();}return i->second;}
bool rt_responds(Obj o,Sel s){auto*f=static_cast<Fake*>(o);return methods.count(std::string(f->cls?"+":"-")+f->name+" "+s);}
[[noreturn]] void rt_fail(const char*s){fprintf(stderr,"FAIL %s\n",s);abort();}
void*rt_block_copy(void*p){if(!p)return p;auto*b=static_cast<Block*>(p);if(b->flags&(1u<<28))return p;auto i=refs.find(p);if(i!=refs.end()){++i->second;return p;}auto*q=malloc(b->desc->size);memcpy(q,p,b->desc->size);refs[q]=1;return q;}
void rt_block_release(void*p){auto i=refs.find(p);if(i!=refs.end()&&!--i->second){refs.erase(i);free(p);}}
static Obj error(Obj,Sel,Obj domain,int64_t code,Obj info){Obj o=rt_new("NSError");hosts[o].dictionary["domain"]=domain;hosts[o].dictionary["userInfo"]=info;hosts[o].number=code;return o;}
static Obj center(Obj,Sel){static Obj o=rt_new("NSNotificationCenter");return o;}
static void post(Obj,Sel,Obj name,Obj){if(!strcmp(rt_utf8(name),"GCControllerDidConnectNotification"))++notices;else if(!strcmp(rt_utf8(name),"GCControllerDidDisconnectNotification"))++disconnects;else abort();}
static void auth(void*,Obj vc,Obj err){assert(!vc&&err);assert(hosts[err].number==1);assert(!strcmp(rt_utf8(hosts[err].dictionary.at("domain")),"NextOSGameKitShimErrorDomain"));++authcalls;}
static void button(void*,Obj,float v,bool pressed){assert(v==0||v==1);assert(pressed==(v>0.5));++buttoncalls;lastpressed=pressed;}
static void axis(void*,Obj,float v){++axiscalls;lastaxis=v;}
static void pause(void*,Obj){++pausecalls;}
int main(){
 rt_method("NSError","errorWithDomain:code:userInfo:",reinterpret_cast<void*>(error),true);
 rt_method("NSNotificationCenter","defaultCenter",reinterpret_cast<void*>(center),true);
 rt_method("NSNotificationCenter","postNotificationName:object:",reinterpret_cast<void*>(post));
 rt_install_services();rt_services_boot();assert(!(SDL_WasInit(0)&SDL_INIT_VIDEO));
 Obj local=send<Obj>(rt_class("GKLocalPlayer"),"localPlayer");assert(!send<bool>(local,"isAuthenticated"));
 Desc desc{0,sizeof(Block)};Block authblock{nullptr,0,0,reinterpret_cast<void*>(auth),&desc};
 send<void>(local,"setAuthenticateHandler:",&authblock);assert(!authcalls);rt_services_tick();assert(authcalls==1&&!send<bool>(local,"isAuthenticated"));
 send<void>(local,"setAuthenticateHandler:",Obj(nullptr));assert(refs.empty());
 int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,1);assert(index>=0);
 SDL_Joystick*joy=SDL_JoystickOpen(index);assert(joy);
 char guid[64];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy),guid,sizeof(guid));
 std::string map=std::string(guid)+",Goblin Services Fixture,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Linux,";
 assert(SDL_GameControllerAddMapping(map.c_str())>=0);rt_services_tick();rt_services_tick();assert(notices>=1);
 Obj all=send<Obj>(rt_class("GCController"),"controllers"),ctl=nullptr;
 for(auto o:hosts[all].array)if(!strcmp(rt_utf8(send<Obj>(o,"vendorName")),"Goblin Services Fixture"))ctl=o;
 assert(ctl);Obj ext=send<Obj>(ctl,"extendedGamepad");assert(ext);
 Obj a=send<Obj>(ext,"buttonA"),left=send<Obj>(ext,"leftThumbstick"),xaxis=send<Obj>(left,"xAxis"),yaxis=send<Obj>(left,"yAxis");
 Block buttonblock{nullptr,0,0,reinterpret_cast<void*>(button),&desc},axisblock{nullptr,0,0,reinterpret_cast<void*>(axis),&desc},pauseblock{nullptr,0,0,reinterpret_cast<void*>(pause),&desc};
 send<void>(a,"setValueChangedHandler:",&buttonblock);send<void>(xaxis,"setValueChangedHandler:",&axisblock);send<void>(ctl,"setControllerPausedHandler:",&pauseblock);
 assert(SDL_JoystickSetVirtualButton(joy,0,1)==0);assert(SDL_JoystickSetVirtualAxis(joy,0,32767)==0);assert(SDL_JoystickSetVirtualAxis(joy,1,-32768)==0);assert(SDL_JoystickSetVirtualButton(joy,6,1)==0);rt_services_tick();
 assert(buttoncalls==1&&lastpressed&&send<bool>(a,"isPressed"));assert(axiscalls==1&&lastaxis==1.0f&&send<float>(yaxis,"value")==1.0f);assert(pausecalls==1);
 rt_services_tick();assert(buttoncalls==1&&axiscalls==1&&pausecalls==1);
 assert(SDL_JoystickSetVirtualButton(joy,0,0)==0);rt_services_tick();assert(buttoncalls==2&&!lastpressed);
 assert(SDL_JoystickSetVirtualButton(joy,0,1)==0);rt_services_tick();assert(buttoncalls==3&&lastpressed);
 assert(SDL_JoystickDetachVirtual(index)==0);rt_services_tick();rt_services_tick();assert(buttoncalls==4&&!lastpressed&&disconnects>=1);
 send<void>(a,"setValueChangedHandler:",Obj(nullptr));send<void>(xaxis,"setValueChangedHandler:",Obj(nullptr));send<void>(ctl,"setControllerPausedHandler:",Obj(nullptr));assert(refs.empty());SDL_JoystickClose(joy);
 assert(!(SDL_WasInit(0)&SDL_INIT_VIDEO));
 printf("PASS GameKit async NSError offline; copied-stack-Block lifetime; real system SDL virtual gamepad enumeration/notifications; button press/release; float axis ABI and Y inversion; paused rising edge; no repeat for held input; release before disconnect; no SDL video initialization\n");return 0;
}
