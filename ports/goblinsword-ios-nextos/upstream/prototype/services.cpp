#include "runtime.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Private Goblin Sword prototype. GameKit remains explicitly offline. Controls
// expose the system SDL2 mapping as GameController elements; the guest's original
// Blocks decide gameplay actions. No private SDL, keyboard bridge, evdev mapping,
// guest-code patch, audio-success stub or SDL video initialization is used here.
namespace {
struct BlockHeader { void* isa; uint32_t flags, reserved; void* invoke; void* descriptor; };
template<class... A> void invoke(void* handler, A... args) {
    if (!handler) return;
    void* held = rt_block_copy(handler);
    auto* b = static_cast<BlockHeader*>(held);
    if (!b->invoke) rt_fail("services: Block has no invoke function");
    reinterpret_cast<void(*)(void*, A...)>(b->invoke)(held, args...);
    rt_block_release(held);
}
void replace_block(void*& destination, void* source) {
    void* next = rt_block_copy(source);
    rt_block_release(destination);
    destination = next;
}
enum class PendingKind { Authentication, ErrorOnly, ObjectsAndError, Discovery, Notification };
struct Pending {
    PendingKind kind;
    void* block = nullptr;
    Obj object = nullptr;
    Obj name = nullptr;
};
std::vector<Pending> pending;
Obj local_player = nullptr, unavailable_error = nullptr;
void* authentication_handler = nullptr;
Obj connected_name = nullptr, disconnected_name = nullptr;
bool booted = false, input_available = false, scanning = false, draining_input = false;

Obj offline_error() {
    if (!unavailable_error) {
        Obj info = rt_new("NSDictionary");
        rt_host(info).dictionary["NSLocalizedDescription"] =
            rt_string("Game Center is unavailable on this host; the player is not authenticated.");
        unavailable_error = send<Obj>(rt_class("NSError"), "errorWithDomain:code:userInfo:",
            rt_string("NextOSGameKitShimErrorDomain"), int64_t(1), info);
        send<Obj>(unavailable_error, "retain");
    }
    return unavailable_error;
}
Obj gk_local_player(Obj, Sel) {
    if (!local_player) local_player = rt_new("GKLocalPlayer");
    return local_player;
}
bool gk_authenticated(Obj, Sel) { return false; }
Obj gk_player_identity(Obj, Sel) { return nullptr; }
Obj gk_get_authentication_handler(Obj, Sel) { return authentication_handler; }
void gk_set_authentication_handler(Obj, Sel, void* block) {
    replace_block(authentication_handler, block);
    if (block) pending.push_back({PendingKind::Authentication, rt_block_copy(authentication_handler)});
    std::fprintf(stderr, "GAMEKIT_OFFLINE authenticateHandler=%s authenticated=false\n", block ? "queued error" : "cleared");
}
void gk_authenticate_legacy(Obj, Sel, void* block) {
    if (block) pending.push_back({PendingKind::ErrorOnly, rt_block_copy(block)});
    std::fprintf(stderr, "GAMEKIT_OFFLINE legacy authentication queued error\n");
}
void gk_load_objects(Obj, Sel, void* block) {
    if (block) pending.push_back({PendingKind::ObjectsAndError, rt_block_copy(block)});
}
void gk_report_objects(Obj, Sel, Obj, void* block) {
    if (block) pending.push_back({PendingKind::ErrorOnly, rt_block_copy(block)});
}
void gk_report_instance(Obj, Sel, void* block) {
    if (block) pending.push_back({PendingKind::ErrorOnly, rt_block_copy(block)});
}

enum class ElementKind { Button, Axis, DirectionPad, Profile };
struct Element {
    ElementKind kind = ElementKind::Button;
    Obj object = nullptr, controller = nullptr, parent = nullptr;
    float value = 0, x = 0, y = 0;
    bool analog = false;
    void* changed = nullptr;
    void* pressed_changed = nullptr;
    std::unordered_map<std::string, Obj> children;
};
struct Controller {
    SDL_GameController* native = nullptr;
    SDL_JoystickID id = -1;
    Obj object = nullptr, standard = nullptr, extended = nullptr, name = nullptr;
    Obj dpad = nullptr, left_stick = nullptr, right_stick = nullptr;
    Obj left_trigger = nullptr, right_trigger = nullptr;
    std::array<Obj, SDL_CONTROLLER_BUTTON_MAX> buttons{};
    std::array<Uint8, SDL_CONTROLLER_BUTTON_MAX> button_state{};
    std::array<Sint16, SDL_CONTROLLER_AXIS_MAX> axis_state{};
    void* paused = nullptr;
    int64_t player_index = -1;
    bool start_pressed = false, connected = false;
    // Physical state is separate from the guest-visible diagnostic pulses.
    bool exit_select_down = false, exit_start_down = false;
};
std::unordered_map<Obj, std::unique_ptr<Element>> elements;
std::unordered_map<Obj, std::shared_ptr<Controller>> controller_objects;
std::unordered_map<SDL_JoystickID, std::shared_ptr<Controller>> devices;

Element& element(Obj object) {
    auto i = elements.find(object);
    if (i == elements.end()) rt_fail("services: unknown GameController element");
    return *i->second;
}
Controller& controller(Obj object) {
    auto i = controller_objects.find(object);
    if (i == controller_objects.end()) rt_fail("services: unknown GCController object");
    return *i->second;
}
Obj make_element(const char* class_name, ElementKind kind, Obj owner, Obj parent = nullptr, bool analog = false) {
    Obj object = rt_new(class_name);
    auto data = std::make_unique<Element>();
    data->kind = kind; data->object = object; data->controller = owner; data->parent = parent; data->analog = analog;
    elements.emplace(object, std::move(data));
    return object;
}
Obj make_dpad(Obj owner, Obj parent, bool analog) {
    Obj object = make_element("GCControllerDirectionPad", ElementKind::DirectionPad, owner, parent, analog);
    for (const char* key : {"up", "down", "left", "right"})
        element(object).children[key] = make_element("GCControllerButtonInput", ElementKind::Button, owner, object, analog);
    for (const char* key : {"xAxis", "yAxis"})
        element(object).children[key] = make_element("GCControllerAxisInput", ElementKind::Axis, owner, object, analog);
    return object;
}
Obj child(Obj object, Sel selector) {
    auto& children = element(object).children;
    auto it = children.find(selector);
    return it == children.end() ? nullptr : it->second;
}
float value(Obj object, Sel) { return element(object).value; }
bool pressed(Obj object, Sel) { return element(object).value > 0.5f; }
bool analog(Obj object, Sel) { return element(object).analog; }
Obj parent(Obj object, Sel) { return element(object).parent; }
Obj element_controller(Obj object, Sel) { return element(object).controller; }
Obj get_handler(Obj object, Sel) { return element(object).changed; }
void set_handler(Obj object, Sel, void* block) { replace_block(element(object).changed, block); }
Obj get_pressed_handler(Obj object, Sel) { return element(object).pressed_changed; }
void set_pressed_handler(Obj object, Sel, void* block) { replace_block(element(object).pressed_changed, block); }
Obj gc_gamepad(Obj object, Sel) { return controller(object).standard; }
Obj gc_extended(Obj object, Sel) { return controller(object).extended; }
Obj gc_vendor(Obj object, Sel) { return controller(object).name; }
bool gc_attached(Obj object, Sel) {
    // SDL does not expose Apple's "physically attached" (built into the device)
    // property. USB/Bluetooth gamepads are external, independently of connection.
    (void)controller(object); return false;
}
int64_t gc_player_index(Obj object, Sel) { return controller(object).player_index; }
void gc_set_player_index(Obj object, Sel, int64_t index) {
    if (index < -1 || index > 3) rt_fail("services: invalid GCController player index");
    controller(object).player_index = index;
}
Obj gc_get_paused(Obj object, Sel) { return controller(object).paused; }
void gc_set_paused(Obj object, Sel, void* block) { replace_block(controller(object).paused, block); }

void notify(Obj name, Obj object) {
    Obj center = send<Obj>(rt_class("NSNotificationCenter"), "defaultCenter");
    send<void>(center, "postNotificationName:object:", name, object);
}
void profile_changed(Obj object) {
    Element& e = element(object);
    Controller& c = controller(e.controller);
    invoke(element(c.standard).changed, c.standard, object);
    invoke(element(c.extended).changed, c.extended, object);
}
void scalar_changed(Obj object, float next, bool dispatch) {
    auto& e = element(object);
    if (e.value == next) return;
    bool old_pressed = e.value > 0.5f;
    e.value = next;
    if (!dispatch) return;
    if (e.kind == ElementKind::Button) {
        bool is_pressed = next > 0.5f;
        invoke(e.changed, object, next, is_pressed);
        if (old_pressed != is_pressed) invoke(e.pressed_changed, object, next, is_pressed);
    } else invoke(e.changed, object, next);
    profile_changed(object);
}
void dpad_changed(Obj object, float x, float y, bool dispatch) {
    auto& e = element(object);
    bool changed = e.x != x || e.y != y;
    e.x = x; e.y = y;
    scalar_changed(e.children.at("xAxis"), x, dispatch);
    scalar_changed(e.children.at("yAxis"), y, dispatch);
    scalar_changed(e.children.at("left"), std::max(0.0f, -x), dispatch);
    scalar_changed(e.children.at("right"), std::max(0.0f, x), dispatch);
    scalar_changed(e.children.at("up"), std::max(0.0f, y), dispatch);
    scalar_changed(e.children.at("down"), std::max(0.0f, -y), dispatch);
    if (changed && dispatch) { invoke(e.changed, object, x, y); profile_changed(object); }
}
float axis_value(Sint16 value) { return value < 0 ? float(value) / 32768.0f : float(value) / 32767.0f; }
void exit_if_physical_chord(const Controller& c) {
    if (!c.connected || !c.native || !c.exit_select_down || !c.exit_start_down) return;
    std::fprintf(stderr, "USER_EXIT reason=select-start pid=%ld instance=%d\n", long(getpid()), int(c.id));
    // Exit this process only. Registered idle/diagnostic atexit cleanup runs;
    // neither a guest callback nor another process/service is involved.
    std::exit(0);
}
void button_event(Controller& c, SDL_GameControllerButton button, bool down, bool dispatch, bool synthetic = false) {
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) return;
    if (!synthetic) {
        if (button == SDL_CONTROLLER_BUTTON_BACK) c.exit_select_down = down;
        if (button == SDL_CONTROLLER_BUTTON_START) c.exit_start_down = down;
        // Check before duplicate suppression: a synthetic pulse may have
        // already changed button_state without changing the physical state.
        if (dispatch && (button == SDL_CONTROLLER_BUTTON_BACK || button == SDL_CONTROLLER_BUTTON_START))
            exit_if_physical_chord(c);
    }
    if (c.button_state[button] == Uint8(down)) return;
    c.button_state[button] = Uint8(down);
    bool dpad = button >= SDL_CONTROLLER_BUTTON_DPAD_UP && button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    if (dispatch && (c.buttons[button] || dpad)) {
        const char* name = SDL_GameControllerGetStringForButton(button);
        std::fprintf(stderr, "%s instance=%d logical=%s state=%s\n",
                     synthetic ? "DIAGNOSTIC_SYNTHETIC_INPUT" : "GAMECONTROLLER_BUTTON",
                     int(c.id), name ? name : "unknown", down ? "down" : "up");
    }
    if (c.buttons[button]) scalar_changed(c.buttons[button], down ? 1.0f : 0.0f, dispatch);
    if (dpad) {
        dpad_changed(c.dpad,
            float(c.button_state[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]) - c.button_state[SDL_CONTROLLER_BUTTON_DPAD_LEFT],
            float(c.button_state[SDL_CONTROLLER_BUTTON_DPAD_UP]) - c.button_state[SDL_CONTROLLER_BUTTON_DPAD_DOWN], dispatch);
    }
    if (button == SDL_CONTROLLER_BUTTON_START) {
        bool previous = c.start_pressed;
        c.start_pressed = down;
        if (down && !previous && dispatch) invoke(c.paused, c.object);
    }
}
void axis_event(Controller& c, SDL_GameControllerAxis axis, Sint16 raw, bool dispatch) {
    if (axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) return;
    if (c.axis_state[axis] == raw) return;
    c.axis_state[axis] = raw;
    auto current = [&](SDL_GameControllerAxis a) { return axis_value(c.axis_state[a]); };
    // Apple +Y points up; SDL +Y points down. Preserve the full SDL magnitude.
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX: case SDL_CONTROLLER_AXIS_LEFTY:
        dpad_changed(c.left_stick, current(SDL_CONTROLLER_AXIS_LEFTX), -current(SDL_CONTROLLER_AXIS_LEFTY), dispatch); break;
    case SDL_CONTROLLER_AXIS_RIGHTX: case SDL_CONTROLLER_AXIS_RIGHTY:
        dpad_changed(c.right_stick, current(SDL_CONTROLLER_AXIS_RIGHTX), -current(SDL_CONTROLLER_AXIS_RIGHTY), dispatch); break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        scalar_changed(c.left_trigger, std::max(0.0f, current(axis)), dispatch); break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        scalar_changed(c.right_trigger, std::max(0.0f, current(axis)), dispatch); break;
    default: break;
    }
}
void snapshot(Controller& c, bool dispatch, bool neutral = false) {
    // Reconcile only after queued edges have been delivered. These same state
    // setters suppress duplicates without losing down/up pairs between frames.
    std::array<Uint8, SDL_CONTROLLER_BUTTON_MAX> physical{};
    if (!neutral && c.native)
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
            physical[i] = SDL_GameControllerGetButton(c.native, SDL_GameControllerButton(i));
    // Reconcile the pair together, before any callback. Otherwise an old START
    // plus a new BACK could falsely overlap when START's release edge was lost.
    c.exit_select_down = physical[SDL_CONTROLLER_BUTTON_BACK] != 0;
    c.exit_start_down = physical[SDL_CONTROLLER_BUTTON_START] != 0;
    if (dispatch) exit_if_physical_chord(c);
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
        auto b = SDL_GameControllerButton(i);
        button_event(c, b, physical[i] != 0, dispatch);
    }
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i) {
        auto a = SDL_GameControllerAxis(i);
        axis_event(c, a, !neutral && c.native ? SDL_GameControllerGetAxis(c.native, a) : 0, dispatch);
    }
}

// Private, opt-in experiment only. A FIFO command produces GameController edges;
// it cannot invoke a selector, change a scene/save or modify guest code. No pulse
// is generated without a complete explicit command. Never label this physical
// input evidence. No caller-supplied path is accepted.
struct DiagnosticInput {
    bool enabled=false,released_this_tick=false;
    int directory_fd=-1,fifo_fd=-1;
    dev_t directory_device=0,fifo_device=0;
    ino_t directory_inode=0,fifo_inode=0;
    char directory[sizeof("/tmp/goblin-ios-input.XXXXXX")]{};
    char line[6]{};
    size_t line_size=0;
    std::deque<SDL_GameControllerButton> queue;
    std::shared_ptr<Controller> active;
    SDL_GameControllerButton active_button=SDL_CONTROLLER_BUTTON_INVALID;
} diagnostic;
static constexpr const char* diagnostic_fifo="buttons";
[[noreturn]] void diagnostic_fail(const char* reason){
    std::fprintf(stderr,"DIAGNOSTIC_SYNTHETIC_INPUT rejected=%s\n",reason);
    rt_fail(reason);
}
bool diagnostic_same(const struct stat& st,dev_t device,ino_t inode){
    return st.st_dev==device&&st.st_ino==inode&&st.st_uid==geteuid();
}
void diagnostic_cleanup(){
    if(diagnostic.fifo_fd>=0){close(diagnostic.fifo_fd);diagnostic.fifo_fd=-1;}
    if(diagnostic.directory_fd>=0){
        struct stat st{};
        if(diagnostic.fifo_inode&&fstatat(diagnostic.directory_fd,diagnostic_fifo,&st,AT_SYMLINK_NOFOLLOW)==0&&
           S_ISFIFO(st.st_mode)&&diagnostic_same(st,diagnostic.fifo_device,diagnostic.fifo_inode))
            unlinkat(diagnostic.directory_fd,diagnostic_fifo,0);
        close(diagnostic.directory_fd);diagnostic.directory_fd=-1;
    }
    struct stat st{};
    if(diagnostic.directory_inode&&lstat(diagnostic.directory,&st)==0&&S_ISDIR(st.st_mode)&&
       diagnostic_same(st,diagnostic.directory_device,diagnostic.directory_inode))rmdir(diagnostic.directory);
    diagnostic.enabled=false;
}
void diagnostic_validate(){
    struct stat dir{},opened{},named{};
    if(fstat(diagnostic.directory_fd,&dir)||!S_ISDIR(dir.st_mode)||
       (dir.st_mode&07777)!=0700||!diagnostic_same(dir,diagnostic.directory_device,diagnostic.directory_inode))
        diagnostic_fail("diagnostic input directory identity/mode changed");
    if(fstat(diagnostic.fifo_fd,&opened)||!S_ISFIFO(opened.st_mode)||opened.st_nlink!=1||
       (opened.st_mode&07777)!=0600||!diagnostic_same(opened,diagnostic.fifo_device,diagnostic.fifo_inode)||
       fstatat(diagnostic.directory_fd,diagnostic_fifo,&named,AT_SYMLINK_NOFOLLOW)||!S_ISFIFO(named.st_mode)||
       (named.st_mode&07777)!=0600||!diagnostic_same(named,diagnostic.fifo_device,diagnostic.fifo_inode))
        diagnostic_fail("diagnostic input FIFO identity/mode changed");
}
void diagnostic_boot(){
    const char* setting=std::getenv("GOBLIN_DIAGNOSTIC_INPUT");
    if(!setting||std::strcmp(setting,"1"))return;
    std::memcpy(diagnostic.directory,"/tmp/goblin-ios-input.XXXXXX",sizeof(diagnostic.directory));
    if(!mkdtemp(diagnostic.directory))diagnostic_fail("cannot create private diagnostic input directory");
    diagnostic.directory_fd=open(diagnostic.directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat dir{};
    if(diagnostic.directory_fd<0||fstat(diagnostic.directory_fd,&dir)||!S_ISDIR(dir.st_mode)||
       dir.st_uid!=geteuid()||(dir.st_mode&07777)!=0700)
        diagnostic_fail("invalid private diagnostic input directory");
    diagnostic.directory_device=dir.st_dev;diagnostic.directory_inode=dir.st_ino;
    if(std::atexit(diagnostic_cleanup))diagnostic_fail("cannot register diagnostic input cleanup");
    if(mkfifoat(diagnostic.directory_fd,diagnostic_fifo,0600))diagnostic_fail("cannot create diagnostic input FIFO");
    struct stat fifo{};
    if(fstatat(diagnostic.directory_fd,diagnostic_fifo,&fifo,AT_SYMLINK_NOFOLLOW)||!S_ISFIFO(fifo.st_mode)||
       fifo.st_uid!=geteuid()||(fifo.st_mode&07777)!=0600||fifo.st_nlink!=1)
        diagnostic_fail("invalid newly created diagnostic input FIFO");
    diagnostic.fifo_device=fifo.st_dev;diagnostic.fifo_inode=fifo.st_ino;
    diagnostic.fifo_fd=openat(diagnostic.directory_fd,diagnostic_fifo,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(diagnostic.fifo_fd<0)diagnostic_fail("cannot open diagnostic input FIFO");
    diagnostic_validate();diagnostic.enabled=true;
    std::fprintf(stderr,"DIAGNOSTIC_SYNTHETIC_INPUT enabled=private-experiment fifo=%s/%s commands=a,b,up,down,left,right,start physical_input_proof=false\n",
                 diagnostic.directory,diagnostic_fifo);
}
std::shared_ptr<Controller> diagnostic_controller(SDL_GameControllerButton button){
    if(!input_available||devices.size()!=1)diagnostic_fail("diagnostic pulse requires exactly one connected controller");
    auto c=devices.begin()->second;
    if(!c->connected||!c->native||!SDL_GameControllerGetAttached(c->native))
        diagnostic_fail("diagnostic pulse controller is not connected");
    if(SDL_GameControllerGetButton(c->native,button))diagnostic_fail("diagnostic pulse button is physically held");
    return c;
}
void diagnostic_parse(char ch){
    if(ch=='\n'){
        diagnostic.line[diagnostic.line_size]=0;
        SDL_GameControllerButton button=SDL_CONTROLLER_BUTTON_INVALID;
        const std::pair<const char*,SDL_GameControllerButton> commands[]={
            {"a",SDL_CONTROLLER_BUTTON_A},{"b",SDL_CONTROLLER_BUTTON_B},
            {"up",SDL_CONTROLLER_BUTTON_DPAD_UP},{"down",SDL_CONTROLLER_BUTTON_DPAD_DOWN},
            {"left",SDL_CONTROLLER_BUTTON_DPAD_LEFT},{"right",SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
            {"start",SDL_CONTROLLER_BUTTON_START}};
        for(const auto& command:commands)if(!std::strcmp(diagnostic.line,command.first))button=command.second;
        if(button==SDL_CONTROLLER_BUTTON_INVALID)diagnostic_fail("unknown diagnostic button command");
        if(diagnostic.queue.size()>=16)diagnostic_fail("diagnostic input queue exceeds 16 pulses");
        (void)diagnostic_controller(button);
        diagnostic.queue.push_back(button);
        std::fprintf(stderr,"DIAGNOSTIC_SYNTHETIC_INPUT accepted=%s queued=%zu\n",diagnostic.line,diagnostic.queue.size());
        diagnostic.line_size=0;
    }else{
        if(ch<'a'||ch>'z'||diagnostic.line_size>=5)diagnostic_fail("invalid or oversized diagnostic input line");
        diagnostic.line[diagnostic.line_size++]=ch;
    }
}
void diagnostic_begin_tick(){
    if(!diagnostic.enabled)return;
    diagnostic.released_this_tick=false;
    if(diagnostic.active){
        auto c=std::move(diagnostic.active);
        button_event(*c,diagnostic.active_button,false,true,true);
        diagnostic.active_button=SDL_CONTROLLER_BUTTON_INVALID;
        diagnostic.released_this_tick=true;
    }
}
void diagnostic_after_snapshot(){
    if(!diagnostic.enabled)return;
    diagnostic_validate();
    // Finite work even if a writer keeps its descriptor open and floods data.
    char buffer[64];size_t budget=256;
    while(budget){
        ssize_t n=read(diagnostic.fifo_fd,buffer,std::min(budget,sizeof(buffer)));
        if(n==0)break; // No current writer / EOF is benign, including partial lines.
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)break;diagnostic_fail("reading diagnostic input FIFO failed");}
        budget-=size_t(n);for(ssize_t i=0;i<n;++i)diagnostic_parse(buffer[i]);
    }
    if(diagnostic.released_this_tick||diagnostic.active||diagnostic.queue.empty())return;
    auto button=diagnostic.queue.front();auto c=diagnostic_controller(button);
    diagnostic.queue.pop_front();diagnostic.active=c;diagnostic.active_button=button;
    button_event(*c,button,true,true,true);
}
void open_controller(int index) {
    if (!SDL_IsGameController(index)) return;
    SDL_GameController* native = SDL_GameControllerOpen(index);
    if (!native) { std::fprintf(stderr, "GAMECONTROLLER_OPEN_FAILED %s\n", SDL_GetError()); return; }
    SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(native));
    if (devices.count(id)) { SDL_GameControllerClose(native); return; }
    auto c = std::make_shared<Controller>();
    c->native = native; c->id = id; c->object = rt_new("GCController"); c->connected = true;
    const char* name = SDL_GameControllerName(native);
    c->name = rt_string(name ? name : "SDL game controller");
    c->standard = make_element("GCGamepad", ElementKind::Profile, c->object);
    c->extended = make_element("GCExtendedGamepad", ElementKind::Profile, c->object);
    const std::pair<const char*, SDL_GameControllerButton> binding[] = {
        {"buttonA", SDL_CONTROLLER_BUTTON_A}, {"buttonB", SDL_CONTROLLER_BUTTON_B},
        {"buttonX", SDL_CONTROLLER_BUTTON_X}, {"buttonY", SDL_CONTROLLER_BUTTON_Y},
        {"leftShoulder", SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, {"rightShoulder", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {"leftThumbstickButton", SDL_CONTROLLER_BUTTON_LEFTSTICK}, {"rightThumbstickButton", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
        {"buttonMenu", SDL_CONTROLLER_BUTTON_START}, {"buttonOptions", SDL_CONTROLLER_BUTTON_BACK},
    };
    for (const auto& b : binding) {
        Obj input = make_element("GCControllerButtonInput", ElementKind::Button, c->object, c->extended);
        c->buttons[b.second] = input;
        element(c->extended).children[b.first] = input;
        if (b.second <= SDL_CONTROLLER_BUTTON_Y || b.second == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || b.second == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            element(c->standard).children[b.first] = input;
    }
    c->dpad = make_dpad(c->object, c->extended, false);
    c->left_stick = make_dpad(c->object, c->extended, true);
    c->right_stick = make_dpad(c->object, c->extended, true);
    c->left_trigger = make_element("GCControllerButtonInput", ElementKind::Button, c->object, c->extended, true);
    c->right_trigger = make_element("GCControllerButtonInput", ElementKind::Button, c->object, c->extended, true);
    element(c->standard).children["dpad"] = c->dpad;
    auto& ext = element(c->extended).children;
    ext["dpad"] = c->dpad; ext["leftThumbstick"] = c->left_stick; ext["rightThumbstick"] = c->right_stick;
    ext["leftTrigger"] = c->left_trigger; ext["rightTrigger"] = c->right_trigger;
    controller_objects[c->object] = c; devices[id] = c;
    snapshot(*c, false);
    pending.push_back({PendingKind::Notification, nullptr, c->object, connected_name});
    std::fprintf(stderr, "GAMECONTROLLER_CONNECTED instance=%d name=%s profile=extended mapped_by=system_SDL\n", int(id), rt_utf8(c->name));
}
void remove_controller(SDL_JoystickID id) {
    auto entry = devices.find(id);
    if (entry == devices.end()) return;
    auto c = entry->second;
    c->connected = false;
    snapshot(*c, true, true); // Deliver held releases before disconnect notification.
    SDL_GameControllerClose(c->native); c->native = nullptr;
    devices.erase(id);
    pending.push_back({PendingKind::Notification, nullptr, c->object, disconnected_name});
    std::fprintf(stderr, "GAMECONTROLLER_DISCONNECTED instance=%d\n", int(id));
}
void scan() {
    // A guest callback may enumerate controllers while we deliver an edge. Do
    // not let that enumeration remove a device ahead of its queued SDL events.
    if (!input_available || scanning || draining_input) return;
    scanning = true;
    std::vector<SDL_JoystickID> removed;
    for (const auto& p : devices) if (!SDL_GameControllerGetAttached(p.second->native)) removed.push_back(p.first);
    for (auto id : removed) remove_controller(id);
    int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        if (!devices.count(id)) open_controller(i);
    }
    scanning = false;
}
void drain_controller_events() {
    draining_input = true;
    SDL_Event event{};
    int result;
    // Pump exactly once in the caller, then consume its queue without PollEvent
    // repumping while guest callbacks run. This is the sole SDL event consumer.
    while ((result = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) > 0) {
        switch (event.type) {
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            auto entry = devices.find(event.cbutton.which);
            if (entry != devices.end()) {
                auto c = entry->second;
                button_event(*c, SDL_GameControllerButton(event.cbutton.button), event.type == SDL_CONTROLLERBUTTONDOWN, true);
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            auto entry = devices.find(event.caxis.which);
            if (entry != devices.end()) {
                auto c = entry->second;
                axis_event(*c, SDL_GameControllerAxis(event.caxis.axis), event.caxis.value, true);
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED: open_controller(event.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: remove_controller(event.cdevice.which); break;
        default: break; // Joystick duplicates are intentionally not another input path.
        }
    }
    draining_input = false;
    if (result < 0) rt_fail("services: cannot read the SDL controller event queue");
}
Obj gc_controllers(Obj, Sel) {
    scan();
    Obj array = rt_new("NSArray");
    std::vector<SDL_JoystickID> ordered;
    for (const auto& p : devices) ordered.push_back(p.first);
    std::sort(ordered.begin(), ordered.end());
    for (auto id : ordered) rt_host(array).array.push_back(send<Obj>(devices.at(id)->object,"retain"));
    return send<Obj>(array, "autorelease");
}
void gc_discover(Obj, Sel, void* block) {
    scan();
    if (block) pending.push_back({PendingKind::Discovery, rt_block_copy(block)});
    std::fprintf(stderr, "GAMECONTROLLER_DISCOVERY system enumeration complete; Bluetooth pairing is managed by host OS\n");
}
void gc_stop_discovery(Obj, Sel) {
    // The one-shot host enumeration has no Bluetooth pairing task to stop.
}
} // namespace

void rt_services_boot() {
    if (booted) return;
    booted = true;
    diagnostic_boot();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "GAMECONTROLLER_UNAVAILABLE SDL input initialization failed: %s\n", SDL_GetError());
        return;
    }
    input_available = true;
    SDL_PumpEvents(); SDL_GameControllerUpdate(); scan();
    std::fprintf(stderr, "SERVICES_BOOT GameKit=offline controllers=%zu SDL_video_initialized_by_services=false\n", devices.size());
}
void rt_services_tick() {
    diagnostic_begin_tick(); // Release last tick's synthetic edge before physical edges.
    // Main-thread queue: callbacks never run during registration or before UIKit
    // has completed UIApplicationMain's launch delegate call.
    auto events = std::move(pending); pending.clear();
    for (const auto& p : events) {
        switch (p.kind) {
        case PendingKind::Authentication:
            if (p.block == authentication_handler) invoke(p.block, Obj(nullptr), offline_error());
            break;
        case PendingKind::ErrorOnly: invoke(p.block, offline_error()); break;
        case PendingKind::ObjectsAndError: invoke(p.block, Obj(nullptr), offline_error()); break;
        case PendingKind::Discovery: invoke(p.block); break;
        case PendingKind::Notification: notify(p.name, p.object); break;
        }
        rt_block_release(p.block);
    }
    if (input_available) {
        SDL_PumpEvents(); SDL_GameControllerUpdate(); drain_controller_events(); scan();
        std::vector<std::shared_ptr<Controller>> active;
        for (const auto& p : devices) active.push_back(p.second);
        for (const auto& c : active) if (c->connected) snapshot(*c, true);
    }
    diagnostic_after_snapshot(); // New DOWN is visible to this frame's native callbacks.
}
void rt_install_services() {
#define M(c,s,f) rt_method(c,s,reinterpret_cast<void*>(f))
#define C(c,s,f) rt_method(c,s,reinterpret_cast<void*>(f),true)
    C("GKLocalPlayer", "localPlayer", gk_local_player);
    M("GKLocalPlayer", "isAuthenticated", gk_authenticated);
    M("GKLocalPlayer", "authenticated", gk_authenticated);
    M("GKLocalPlayer", "playerID", gk_player_identity);
    M("GKLocalPlayer", "alias", gk_player_identity);
    M("GKLocalPlayer", "displayName", gk_player_identity);
    M("GKLocalPlayer", "authenticateHandler", gk_get_authentication_handler);
    M("GKLocalPlayer", "setAuthenticateHandler:", gk_set_authentication_handler);
    M("GKLocalPlayer", "authenticateWithCompletionHandler:", gk_authenticate_legacy);
    C("GKAchievement", "loadAchievementsWithCompletionHandler:", gk_load_objects);
    C("GKAchievement", "reportAchievements:withCompletionHandler:", gk_report_objects);
    C("GKScore", "reportScores:withCompletionHandler:", gk_report_objects);
    M("GKAchievement", "reportAchievementWithCompletionHandler:", gk_report_instance);
    M("GKScore", "reportScoreWithCompletionHandler:", gk_report_instance);
    C("GCController", "controllers", gc_controllers);
    C("GCController", "startWirelessControllerDiscoveryWithCompletionHandler:", gc_discover);
    C("GCController", "stopWirelessControllerDiscovery", gc_stop_discovery);
    M("GCController", "gamepad", gc_gamepad); M("GCController", "extendedGamepad", gc_extended);
    M("GCController", "vendorName", gc_vendor); M("GCController", "isAttachedToDevice", gc_attached);
    M("GCController", "playerIndex", gc_player_index); M("GCController", "setPlayerIndex:", gc_set_player_index);
    M("GCController", "controllerPausedHandler", gc_get_paused); M("GCController", "setControllerPausedHandler:", gc_set_paused);
    for (const char* c : {"GCGamepad", "GCExtendedGamepad", "GCControllerDirectionPad", "GCControllerAxisInput", "GCControllerButtonInput"}) {
        M(c, "valueChangedHandler", get_handler); M(c, "setValueChangedHandler:", set_handler);
        M(c, "controller", element_controller); M(c, "collection", parent); M(c, "isAnalog", analog);
    }
    for (const char* c : {"GCGamepad", "GCExtendedGamepad"})
        for (const char* s : {"buttonA", "buttonB", "buttonX", "buttonY", "leftShoulder", "rightShoulder", "dpad"}) M(c,s,child);
    for (const char* s : {"leftThumbstick", "rightThumbstick", "leftTrigger", "rightTrigger", "leftThumbstickButton", "rightThumbstickButton", "buttonMenu", "buttonOptions"}) M("GCExtendedGamepad",s,child);
    for (const char* s : {"up", "down", "left", "right", "xAxis", "yAxis"}) M("GCControllerDirectionPad",s,child);
    M("GCControllerAxisInput", "value", value); M("GCControllerButtonInput", "value", value);
    M("GCControllerButtonInput", "isPressed", pressed);
    M("GCControllerButtonInput", "pressedChangedHandler", get_pressed_handler);
    M("GCControllerButtonInput", "setPressedChangedHandler:", set_pressed_handler);
    connected_name = rt_string("GCControllerDidConnectNotification");
    disconnected_name = rt_string("GCControllerDidDisconnectNotification");
#undef M
#undef C
}
void* rt_services_symbol(const char* symbol) {
    if (!std::strcmp(symbol, "_GCControllerDidConnectNotification")) return &connected_name;
    if (!std::strcmp(symbol, "_GCControllerDidDisconnectNotification")) return &disconnected_name;
    return nullptr;
}
