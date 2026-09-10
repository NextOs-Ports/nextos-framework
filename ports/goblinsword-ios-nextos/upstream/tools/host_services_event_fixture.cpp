// Reuse only the fake Objective-C/Block boundary, with real system SDL virtual
// controllers. The older fixture remains independently runnable.
#define main original_services_fixture_main
#include "host_services_fixture.cpp"
#undef main

struct InputRecord { std::string name; float value; };
static std::vector<InputRecord> input_records;
static std::unordered_map<Obj,std::string> input_names;
static int profile_callbacks=0;
static void edge(void*,Obj object,float v,bool down){
    assert(down==(v>0.5f));input_records.push_back({input_names.at(object),v});
}
static void motion(void*,Obj object,float v){input_records.push_back({input_names.at(object),v});}
static void paused_edge(void*,Obj){input_records.push_back({"pause",1});}
static void profile_edge(void*,Obj,Obj){++profile_callbacks;}
static void ordered_post(Obj o,Sel s,Obj name,Obj object){
    post(o,s,name,object);
    if(!std::strcmp(rt_utf8(name),"GCControllerDidDisconnectNotification"))input_records.push_back({"disconnect",0});
}
static void expect(std::initializer_list<InputRecord> wanted){
    assert(input_records.size()==wanted.size());size_t index=0;
    for(const auto& expected:wanted){
        const auto& actual=input_records[index++];
        assert(actual.name==expected.name&&actual.value==expected.value);
    }
    input_records.clear();
}
static void update_button(SDL_Joystick* joy,int button,Uint8 down){
    assert(SDL_JoystickSetVirtualButton(joy,button,down)==0);SDL_GameControllerUpdate();
}
static void update_axis(SDL_Joystick* joy,int axis,Sint16 value){
    assert(SDL_JoystickSetVirtualAxis(joy,axis,value)==0);SDL_GameControllerUpdate();
}
static Obj find_fixture_controller(){
    Obj all=send<Obj>(rt_class("GCController"),"controllers");
    for(Obj candidate:hosts[all].array)
        if(!std::strcmp(rt_utf8(send<Obj>(candidate,"vendorName")),"Goblin Event Fixture"))return candidate;
    assert(!"virtual controller missing from GameController API");return nullptr;
}
int main(){
    // Linux input only; no real game data, display, audio or evdev injection.
    SDL_setenv("SDL_JOYSTICK_HIDAPI","0",1);
    rt_method("NSNotificationCenter","defaultCenter",reinterpret_cast<void*>(center),true);
    rt_method("NSNotificationCenter","postNotificationName:object:",reinterpret_cast<void*>(ordered_post));
    rt_install_services();rt_services_boot();
    assert(!(SDL_WasInit(0)&(SDL_INIT_VIDEO|SDL_INIT_AUDIO)));
    int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,1);assert(index>=0);
    SDL_Joystick* joy=SDL_JoystickOpen(index);assert(joy);
    char guid[64];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy),guid,sizeof(guid));
    std::string mapping=std::string(guid)+",Goblin Event Fixture,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Linux,";
    assert(SDL_GameControllerAddMapping(mapping.c_str())>=0);
    update_axis(joy,4,-32768);update_axis(joy,5,-32768);
    rt_services_tick();rt_services_tick();assert(notices>=1);
    Obj ctl=find_fixture_controller(),ext=send<Obj>(ctl,"extendedGamepad");
    Obj a=send<Obj>(ext,"buttonA"),b=send<Obj>(ext,"buttonB"),dpad=send<Obj>(ext,"dpad");
    Obj left=send<Obj>(ext,"leftThumbstick"),right=send<Obj>(ext,"rightThumbstick");
    Obj dx=send<Obj>(dpad,"xAxis"),dy=send<Obj>(dpad,"yAxis");
    Obj lx=send<Obj>(left,"xAxis"),ly=send<Obj>(left,"yAxis"),rx=send<Obj>(right,"xAxis"),ry=send<Obj>(right,"yAxis");
    Obj lt=send<Obj>(ext,"leftTrigger"),rt=send<Obj>(ext,"rightTrigger");
    Desc desc{0,sizeof(Block)};
    Block edgeblock{nullptr,0,0,reinterpret_cast<void*>(edge),&desc};
    Block motionblock{nullptr,0,0,reinterpret_cast<void*>(motion),&desc};
    Block pauseblock{nullptr,0,0,reinterpret_cast<void*>(paused_edge),&desc};
    Block profileblock{nullptr,0,0,reinterpret_cast<void*>(profile_edge),&desc};
    for(auto pair:std::initializer_list<std::pair<Obj,const char*>>{{a,"A"},{b,"B"},{lt,"LT"},{rt,"RT"}}){
        input_names[pair.first]=pair.second;send<void>(pair.first,"setValueChangedHandler:",&edgeblock);
    }
    for(auto pair:std::initializer_list<std::pair<Obj,const char*>>{{dx,"dpadX"},{dy,"dpadY"},{lx,"leftX"},{ly,"leftY"},{rx,"rightX"},{ry,"rightY"}}){
        input_names[pair.first]=pair.second;send<void>(pair.first,"setValueChangedHandler:",&motionblock);
    }
    send<void>(ctl,"setControllerPausedHandler:",&pauseblock);
    send<void>(ext,"setValueChangedHandler:",&profileblock);

    // The original bug: both hardware edges precede one runtime tick, and the
    // final native state is already neutral. Both original Blocks must run.
    update_button(joy,0,1);update_button(joy,0,0);
    assert(SDL_HasEvent(SDL_CONTROLLERBUTTONDOWN)&&SDL_HasEvent(SDL_CONTROLLERBUTTONUP));
    assert(!SDL_JoystickGetButton(joy,0));
    rt_services_tick();expect({{"A",1},{"A",0}});assert(profile_callbacks==2&&!send<bool>(a,"isPressed"));
    update_button(joy,0,1);update_button(joy,1,1);update_button(joy,0,0);update_button(joy,1,0);
    rt_services_tick();expect({{"A",1},{"B",1},{"A",0},{"B",0}});assert(profile_callbacks==6);
    rt_services_tick();expect({});assert(profile_callbacks==6);

    update_button(joy,11,1);update_button(joy,11,0);rt_services_tick();expect({{"dpadY",1},{"dpadY",0}});
    update_button(joy,11,1);update_button(joy,12,1);update_button(joy,11,0);update_button(joy,12,0);
    rt_services_tick();expect({{"dpadY",1},{"dpadY",0},{"dpadY",-1},{"dpadY",0}});
    update_button(joy,14,1);update_button(joy,14,0);rt_services_tick();expect({{"dpadX",1},{"dpadX",0}});
    update_axis(joy,0,32767);update_axis(joy,1,-32768);update_axis(joy,0,0);update_axis(joy,1,0);
    rt_services_tick();expect({{"leftX",1},{"leftY",1},{"leftX",0},{"leftY",0}});
    update_axis(joy,2,-32768);update_axis(joy,3,32767);update_axis(joy,2,0);update_axis(joy,3,0);
    rt_services_tick();expect({{"rightX",-1},{"rightY",-1},{"rightX",0},{"rightY",0}});
    update_axis(joy,4,32767);update_axis(joy,4,-32768);update_axis(joy,5,32767);update_axis(joy,5,-32768);
    rt_services_tick();expect({{"LT",1},{"LT",0},{"RT",1},{"RT",0}});
    update_button(joy,6,1);update_button(joy,6,0);update_button(joy,6,1);update_button(joy,6,0);
    rt_services_tick();expect({{"pause",1},{"pause",1}});

    update_button(joy,0,1);rt_services_tick();expect({{"A",1}});
    rt_services_tick();rt_services_tick();expect({});assert(send<bool>(a,"isPressed"));
    // A missing SDL edge is repaired by the final snapshot exactly once.
    update_button(joy,1,1);SDL_FlushEvents(SDL_CONTROLLERBUTTONDOWN,SDL_CONTROLLERBUTTONUP);
    rt_services_tick();expect({{"B",1}});rt_services_tick();expect({});
    update_button(joy,1,0);rt_services_tick();expect({{"B",0}});

    // Pending B edges precede disconnect; held A gets a release before its
    // disconnect notification. No early GetAttached scan may discard the tap.
    update_button(joy,1,1);update_button(joy,1,0);
    assert(SDL_JoystickDetachVirtual(index)==0);rt_services_tick();expect({{"B",1},{"B",0},{"A",0}});
    rt_services_tick();expect({{"disconnect",0}});assert(!send<bool>(a,"isPressed"));
    SDL_JoystickClose(joy);
    for(Obj object:{a,b,lt,rt,dx,dy,lx,ly,rx,ry,ext})send<void>(object,"setValueChangedHandler:",Obj(nullptr));
    send<void>(ctl,"setControllerPausedHandler:",Obj(nullptr));assert(refs.empty());

    // A controller connected while held exposes its real initial state and
    // does not manufacture repeat presses after handlers are registered.
    index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,16,1);assert(index>=0);
    joy=SDL_JoystickOpen(index);assert(joy);update_button(joy,0,1);
    rt_services_tick();rt_services_tick();ctl=find_fixture_controller();ext=send<Obj>(ctl,"extendedGamepad");a=send<Obj>(ext,"buttonA");
    input_names[a]="A";assert(send<bool>(a,"isPressed"));send<void>(a,"setValueChangedHandler:",&edgeblock);
    rt_services_tick();expect({});update_button(joy,0,0);rt_services_tick();expect({{"A",0}});
    send<void>(a,"setValueChangedHandler:",Obj(nullptr));assert(refs.empty());
    assert(SDL_JoystickDetachVirtual(index)==0);rt_services_tick();rt_services_tick();expect({{"disconnect",0}});SDL_JoystickClose(joy);
    assert(!(SDL_WasInit(0)&(SDL_INIT_VIDEO|SDL_INIT_AUDIO)));
    std::puts("PASS: system SDL virtual down/up in one tick; global edge order; no snapshot duplicates; dpad Apple Y; both sticks and triggers; START rising edges; held state; snapshot recovery; queued tap before detach; release before disconnect; held reconnect; no SDL video/audio");
}
