// Directed service fixture: actual system SDL virtual controllers, fake ObjC
// boundaries only. Each case execs a fresh subprocess; no guest code/device.
#define main previous_services_fixture_main
#include "host_services_fixture.cpp"
#undef main
#include "services.cpp"
#include <sys/wait.h>

static int exit_buttons = 0, exit_pauses = 0;
static void exit_button(void*, Obj, float value, bool down) {
    assert(value == (down ? 1.0f : 0.0f)); ++exit_buttons;
}
static void exit_pause(void*, Obj) { ++exit_pauses; }
static void exit_marker() {
    std::printf("ATEXIT_MARKER pid=%ld buttons=%d pauses=%d\n", long(getpid()), exit_buttons, exit_pauses);
}
struct Pad { int index; SDL_Joystick* joy; std::shared_ptr<Controller> c; };
static Desc exit_desc{0, sizeof(Block)};
static Block exit_button_block{nullptr, 0, 0, reinterpret_cast<void*>(exit_button), &exit_desc};
static Block exit_pause_block{nullptr, 0, 0, reinterpret_cast<void*>(exit_pause), &exit_desc};
static void physical(Pad& p, SDL_GameControllerButton button, bool down) {
    int raw = button == SDL_CONTROLLER_BUTTON_BACK ? 4 : 6;
    assert(SDL_JoystickSetVirtualButton(p.joy, raw, down ? 1 : 0) == 0);
    SDL_GameControllerUpdate();
}
static Pad attach(bool back = false, bool start = false) {
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 1); assert(index >= 0);
    SDL_Joystick* joy = SDL_JoystickOpen(index); assert(joy);
    char guid[64]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, sizeof(guid));
    std::string mapping = std::string(guid) + ",Goblin Exit Fixture,a:b0,b:b1,x:b2,y:b3,back:b4,start:b6,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:Linux,";
    assert(SDL_GameControllerAddMapping(mapping.c_str()) >= 0);
    Pad p{index, joy, {}}; physical(p, SDL_CONTROLLER_BUTTON_BACK, back); physical(p, SDL_CONTROLLER_BUTTON_START, start);
    open_controller(index); // Real service priming snapshot, dispatch=false.
    p.c = devices.at(SDL_JoystickInstanceID(joy));
    assert(p.c->exit_select_down == back && p.c->exit_start_down == start);
    set_handler(p.c->buttons[SDL_CONTROLLER_BUTTON_BACK], nullptr, &exit_button_block);
    set_handler(p.c->buttons[SDL_CONTROLLER_BUTTON_START], nullptr, &exit_button_block);
    gc_set_paused(p.c->object, nullptr, &exit_pause_block);
    return p;
}
static void edge(Pad& p, SDL_GameControllerButton button, bool down) { physical(p, button, down); rt_services_tick(); }
static void synthetic(Pad& p, SDL_GameControllerButton button, bool down) { button_event(*p.c, button, down, true, true); }
static void flush_button_edges() { SDL_FlushEvents(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLERBUTTONUP); }
static void duplicate(Pad& p, SDL_GameControllerButton button) {
    SDL_Event event{}; event.type = SDL_CONTROLLERBUTTONDOWN; event.cbutton.which = p.c->id;
    event.cbutton.button = Uint8(button); event.cbutton.state = SDL_PRESSED;
    assert(SDL_PushEvent(&event) == 1); rt_services_tick();
}
static int child_case(const std::string& name) {
    SDL_setenv("SDL_JOYSTICK_HIDAPI", "0", 1);
    setenv("GOBLIN_DIAGNOSTIC_INPUT", "1", 1);
    rt_method("NSNotificationCenter", "defaultCenter", reinterpret_cast<void*>(center), true);
    rt_method("NSNotificationCenter", "postNotificationName:object:", reinterpret_cast<void*>(post));
    rt_install_services(); rt_services_boot(); assert(std::atexit(exit_marker) == 0);
    std::printf("DIAGNOSTIC_DIRECTORY %s\n", diagnostic.directory);
    assert(!(SDL_WasInit(0) & (SDL_INIT_VIDEO | SDL_INIT_AUDIO)));
    constexpr auto back = SDL_CONTROLLER_BUTTON_BACK, start = SDL_CONTROLLER_BUTTON_START;
    if (name == "initial_chord") {
        auto p = attach(true, true); (void)p;
        assert(exit_buttons == 0 && exit_pauses == 0);
        std::puts("INITIAL_PRIMED_WITHOUT_CALLBACK"); rt_services_tick();
    } else {
        auto p = attach(); rt_services_tick(); rt_services_tick();
        assert(exit_buttons == 0 && exit_pauses == 0);
        if (name == "select_start") { edge(p, back, true); edge(p, start, true); }
        else if (name == "start_select") { edge(p, start, true); edge(p, back, true); }
        else if (name == "one_tick_select_start") { physical(p, back, true); physical(p, start, true); rt_services_tick(); }
        else if (name == "one_tick_start_select") { physical(p, start, true); physical(p, back, true); rt_services_tick(); }
        else if (name == "snapshot_chord") { physical(p, back, true); physical(p, start, true); flush_button_edges(); rt_services_tick(); }
        else if (name == "start_alone_duplicates") {
            edge(p, start, true); duplicate(p, start); rt_services_tick(); rt_services_tick();
            assert(exit_buttons == 1 && exit_pauses == 1);
            edge(p, start, false); edge(p, start, true); edge(p, start, false);
        } else if (name == "select_alone_duplicates") {
            edge(p, back, true); duplicate(p, back); rt_services_tick(); rt_services_tick();
            assert(exit_buttons == 1 && exit_pauses == 0); edge(p, back, false);
        } else if (name == "different_controllers") {
            auto q = attach(); rt_services_tick();
            edge(p, back, true); edge(q, start, true);
            assert(p.c->id != q.c->id && p.c->exit_select_down && !p.c->exit_start_down);
            assert(q.c->exit_start_down && !q.c->exit_select_down);
            edge(p, back, false); edge(q, start, false);
        } else if (name == "release_before_other") {
            edge(p, start, true); physical(p, start, false); physical(p, back, true); rt_services_tick(); edge(p, back, false);
        } else if (name == "snapshot_released_start") {
            edge(p, start, true); physical(p, start, false); physical(p, back, true); flush_button_edges();
            rt_services_tick(); rt_services_tick(); edge(p, back, false);
        } else if (name == "snapshot_released_select") {
            edge(p, back, true); physical(p, back, false); physical(p, start, true); flush_button_edges();
            rt_services_tick(); rt_services_tick(); edge(p, start, false);
        } else if (name == "disconnect_reconnect") {
            edge(p, back, true); assert(SDL_JoystickDetachVirtual(p.index) == 0); rt_services_tick(); rt_services_tick();
            assert(!p.c->connected && !p.c->native && !p.c->exit_select_down && !p.c->exit_start_down);
            auto q = attach(); rt_services_tick(); assert(q.c->id != p.c->id);
            edge(q, start, true); edge(q, start, false);
        } else if (name == "physical_select_synthetic_start") {
            edge(p, back, true); synthetic(p, start, true); assert(!p.c->exit_start_down);
            rt_services_tick(); edge(p, back, false);
        } else if (name == "synthetic_select_physical_start") {
            synthetic(p, back, true); assert(!p.c->exit_select_down); edge(p, start, true); edge(p, start, false);
        } else if (name == "synthetic_pair") {
            synthetic(p, back, true); synthetic(p, start, true);
            assert(!p.c->exit_select_down && !p.c->exit_start_down); rt_services_tick();
        } else if (name == "physical_duplicate_after_synthetic") {
            edge(p, back, true); synthetic(p, start, true); edge(p, start, true);
        } else if (name == "synthetic_release_keeps_physical") {
            edge(p, start, true); synthetic(p, start, false); assert(p.c->exit_start_down); edge(p, back, true);
        } else assert(!"unknown fixture case");
    }
    assert(!(SDL_WasInit(0) & (SDL_INIT_VIDEO | SDL_INIT_AUDIO)));
    std::puts("NORMAL_RETURN"); return 0;
}
struct Case { const char* name; bool exits; int buttons, pauses; };
int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--case")) return child_case(argv[2]);
    assert(argc == 1);
    const Case cases[] = {
        {"select_start", true, 1, 0}, {"start_select", true, 1, 1},
        {"one_tick_select_start", true, 1, 0}, {"one_tick_start_select", true, 1, 1},
        {"snapshot_chord", true, 0, 0}, {"initial_chord", true, 0, 0},
        {"start_alone_duplicates", false, 4, 2}, {"select_alone_duplicates", false, 2, 0},
        {"different_controllers", false, 4, 1}, {"release_before_other", false, 4, 1},
        {"snapshot_released_start", false, 4, 1}, {"snapshot_released_select", false, 4, 1},
        {"disconnect_reconnect", false, 4, 1}, {"physical_select_synthetic_start", false, 4, 1},
        {"synthetic_select_physical_start", false, 4, 1}, {"synthetic_pair", false, 4, 1},
        {"physical_duplicate_after_synthetic", true, 2, 1}, {"synthetic_release_keeps_physical", true, 2, 1},
    };
    for (const auto& test : cases) {
        int output[2]; assert(pipe(output) == 0); pid_t pid = fork(); assert(pid >= 0);
        if (!pid) {
            close(output[0]); assert(dup2(output[1], STDOUT_FILENO) >= 0); assert(dup2(output[1], STDERR_FILENO) >= 0); close(output[1]);
            execl(argv[0], argv[0], "--case", test.name, static_cast<char*>(nullptr)); _exit(127);
        }
        close(output[1]); std::string captured; char buffer[2048]; ssize_t n;
        while ((n = read(output[0], buffer, sizeof(buffer))) != 0) {
            if (n < 0 && errno == EINTR) continue;
            assert(n > 0); captured.append(buffer, size_t(n));
        }
        close(output[0]); int status; assert(waitpid(pid, &status, 0) == pid);
        auto require = [&](bool condition) {
            if (!condition) { std::fprintf(stderr, "FAILED case=%s status=%d\n%s", test.name, status, captured.c_str()); std::abort(); }
        };
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        std::string marker = "ATEXIT_MARKER pid=" + std::to_string(pid) + " buttons=" + std::to_string(test.buttons) + " pauses=" + std::to_string(test.pauses) + "\n";
        require(captured.find(marker) != std::string::npos);
        auto at = captured.find("USER_EXIT reason=select-start pid=");
        require((at != std::string::npos) == test.exits);
        require((captured.find("NORMAL_RETURN\n") == std::string::npos) == test.exits);
        if (test.exits) {
            require(captured.find("USER_EXIT reason=select-start pid=" + std::to_string(pid) + " instance=", at) == at);
            require(captured.find("USER_EXIT ", at + 1) == std::string::npos);
        }
        const std::string prefix = "DIAGNOSTIC_DIRECTORY "; at = captured.find(prefix); require(at != std::string::npos);
        auto end = captured.find('\n', at); std::string directory = captured.substr(at + prefix.size(), end - at - prefix.size());
        require(access(directory.c_str(), F_OK) == -1 && errno == ENOENT); // Real registered cleanup ran.
        std::printf("PASS case=%s exit0=true chord_exit=%s buttons=%d pauses=%d atexit=true diagnostic_cleanup=true\n",
                    test.name, test.exits ? "true" : "false", test.buttons, test.pauses);
    }
    std::puts("PASS: 18 subprocess cases; exact process exit=0; atexit marker and own FIFO cleanup; system SDL virtual input only; no guest/display/audio/device");
}
