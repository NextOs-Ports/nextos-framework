/* SDL controller state -> Dead Trigger's own PlayerControlsGamepad semantics. */
#define _GNU_SOURCE

#include "dt.h"
#include "gamepad_dt.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum dt_game_input {
    DT_INPUT_FIRE = 0,
    DT_INPUT_RELOAD = 1,
    DT_INPUT_PAUSE = 2,
    DT_INPUT_PREV_WEAPON = 3,
    DT_INPUT_NEXT_WEAPON = 4,
    DT_INPUT_AIM = 5,
    DT_INPUT_ITEM1 = 6,
    DT_INPUT_ITEM2 = 7,
    DT_INPUT_ITEM3 = 8,
    DT_INPUT_ITEM4 = 9,
    DT_INPUT_ACTION = 10,
    DT_INPUT_MOVE_RIGHT = 11,
    DT_INPUT_MOVE_UP = 12,
    DT_INPUT_VIEW_RIGHT = 13,
    DT_INPUT_VIEW_UP = 14,
    DT_INPUT_COUNT = 15
};

typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef const void *(*il2cpp_class_get_method_from_name_fn)(
    void *, const char *, int);
typedef void *(*il2cpp_class_get_field_from_name_fn)(void *, const char *);
typedef void (*il2cpp_field_static_get_value_fn)(void *, void *);

static _Atomic uint32_t g_left_x_bits;
static _Atomic uint32_t g_left_y_bits;
static _Atomic uint32_t g_right_x_bits;
static _Atomic uint32_t g_right_y_bits;
static _Atomic uint32_t g_level;
static _Atomic uint32_t g_pending_down;
static _Atomic uint32_t g_pending_up;

/*
 * Edge delivery is latched: PlayerControlsGamepad.Update is skipped by the
 * game while the HUD is hidden, a fade runs or InUseMode is active, so an
 * edge exposed for exactly one frame can be consumed by nobody. A latched
 * bit only clears on the frame after the game actually read it.
 */
static uint32_t g_frame_level;
static uint32_t g_down_latch;
static uint32_t g_up_latch;
static uint32_t g_down_read;
static uint32_t g_up_read;
static uint32_t g_previous_level;
static int g_fire_down_delivered;
static float g_frame_left_x;
static float g_frame_left_y;
static float g_frame_right_x;
static float g_frame_right_y;

/*
 * The game computes view delta as GetGpadAxis * ViewSensitivity in DEGREES
 * PER FRAME, clamped to 360°*dt. ViewSensitivity = 2x the touch-sensitivity
 * option, which is large enough that any axis scaling still hit the 360°/s
 * clamp — that is why scaling the axis alone changed nothing on the TV. The
 * sensitivity getters are hooked instead, so the camera speed is ours:
 * max speed = view_sens degrees per frame (~25 fps on this device).
 */
static float g_look_scale = 1.0f;
static float g_look_expo = 0.55f;
static float g_view_sens_x = 6.0f;
static float g_view_sens_y = 4.0f;

/*
 * The in-game sensitivity slider writes GuiOptions.sensitivity (static
 * float, persisted as "OptionsSensitivityDT2"); the original getters
 * returned 2.0x / 1.5x of it. Hooking them with constants detached the
 * slider, so the slider value is read back here and applied as a ratio
 * against GuiOptions.DefaultSensitivity: at the default position the
 * camera keeps the exact tuned speed above.
 */
static il2cpp_field_static_get_value_fn g_field_static_get_value;
static void *g_field_sensitivity;
static void *g_field_default_sensitivity;
static int g_fire_sticky_up = 1;
static int g_fire_level_down = 1;
static int g_padlog;

static int g_install_state;
static unsigned g_install_attempts;
static atomic_ullong g_gameplay_axis_millis;

/*
 * DT_FIRELOG=1: diagnostic sampling of every gate that can silence the
 * trigger (post-auto-reload "mute gun" report). Reads the game's own
 * state via il2cpp metadata on the frames the game polls ButtonDown(FIRE)
 * and prints transitions only. Never enabled by the release launcher.
 */
typedef size_t (*il2cpp_field_get_offset_fn)(void *);
typedef uint8_t (*m_bool_fn)(void *, void *);
typedef void *(*m_obj_fn)(void *, void *);
typedef int32_t (*m_int_fn)(void *, void *);

static void *find_class(dt_module *il2cpp, const char *class_name);
static void *find_method(dt_module *il2cpp, const char *class_name,
                         const char *method_name, int parameter_count);
static void *find_method_ns(dt_module *il2cpp, const char *name_space,
                            const char *class_name, const char *method_name,
                            int parameter_count);
static uint64_t monotonic_milliseconds(void);
static void gamepad_env_init(void);
static int replace_method_body(dt_module *il2cpp, const void *method,
                               void *replacement, const char *name);

static int g_firelog;
static il2cpp_field_static_get_value_fn g_fl_static_get;
static il2cpp_field_get_offset_fn g_fl_field_offset;
static void *g_fl_player_instance;   /* FieldInfo Player.Instance (static) */
static m_bool_fn g_fl_canfire;
static m_bool_fn g_fl_isbusy;
static m_obj_fn g_fl_getcurweapon;
static m_int_fn g_fl_clipammo;
static size_t g_fl_off_owner;        /* ComponentPlayer.<Owner>k__BackingField */
static size_t g_fl_off_bb;           /* AgentHuman.BlackBoard */
static size_t g_fl_off_wcomp;        /* AgentHuman.<WeaponComponent>k__BackingField */
static size_t g_fl_off_busyaction;   /* BlackBoard.BusyAction */
static size_t g_fl_off_actionpoint;  /* BlackBoard.ActionPointOn */
static size_t g_fl_off_stop;         /* BlackBoard.<Stop>k__BackingField */
static int g_fl_ready;

static void *fl_read_ptr(void *obj, size_t offset) {
    return obj && offset ? *(void **)((char *)obj + offset) : NULL;
}

static uint8_t fl_read_bool(void *obj, size_t offset) {
    return obj && offset ? *(uint8_t *)((char *)obj + offset) : 0;
}

static void firelog_sample(int held) {
    if (!g_fl_ready)
        return;
    void *player = NULL;
    g_fl_static_get(g_fl_player_instance, &player);
    if (!player)
        return;
    void *owner = fl_read_ptr(player, g_fl_off_owner);
    if (!owner)
        return;
    void *bb = fl_read_ptr(owner, g_fl_off_bb);
    void *wcomp = fl_read_ptr(owner, g_fl_off_wcomp);
    void *weapon = wcomp ? g_fl_getcurweapon(wcomp, NULL) : NULL;
    unsigned state =
        (held ? 1u : 0u) |
        ((unsigned)(g_fl_canfire(owner, NULL) & 1) << 1) |
        ((weapon && g_fl_isbusy(weapon, NULL)) ? 4u : 0u) |
        ((unsigned)(fl_read_bool(bb, g_fl_off_busyaction) & 1) << 3) |
        ((unsigned)(fl_read_bool(bb, g_fl_off_actionpoint) & 1) << 4) |
        ((unsigned)(fl_read_bool(bb, g_fl_off_stop) & 1) << 5);
    int clip = weapon ? g_fl_clipammo(weapon, NULL) : -1;
    static unsigned last_state = ~0u;
    static int last_clip = -2;
    if (state == last_state && clip == last_clip)
        return;
    last_state = state;
    last_clip = clip;
    fprintf(stderr,
            "[firelog] %llu held=%u canfire=%u clip=%d wbusy=%u "
            "bbBusyAction=%u bbActionPoint=%u bbStop=%u\n",
            (unsigned long long)monotonic_milliseconds(),
            state & 1, (state >> 1) & 1, clip, (state >> 2) & 1,
            (state >> 3) & 1, (state >> 4) & 1, (state >> 5) & 1);
}

/*
 * Post-auto-reload mute fix. The game arms the weapon's Busy deadline twice
 * per reload: WeaponBase.Reload() with the WeaponSettings TimeReload and the
 * AnimState handler with the actual animation length; the later call wins.
 * Measured live on device: every normal reload ends busy after ~1.65 s, but
 * the first auto-reload keeps a ~3.24 s deadline because the two calls land
 * in the opposite order, leaving the long value in place — the "gun mute for
 * 2-5 s after the reload animation" report. SetBusy is reimplemented
 * faithfully (Busy = TimeManager.timeSinceLevelLoad + t) with one guard: a
 * call that would EXTEND a still-active long deadline set moments before on
 * the same weapon is ignored, so whichever order the two reload writes
 * arrive in, the shorter one rules. Fire-rate SetBusy calls always start
 * from an expired deadline (CanFire gate) and are never refused.
 */
static m_obj_fn g_sb_get_instance;      /* TimeManager.get_Instance */
typedef float (*m_float_fn)(void *, void *);
static m_float_fn g_sb_get_time;        /* get_timeSinceLevelLoad */
static size_t g_sb_off_busy;            /* WeaponBase.Busy */
static int g_busy_guard = 1;

static void hooked_set_busy(void *weapon, float busy_time) {
    static void *previous_weapon;
    static uint64_t previous_millis;
    if (!weapon)
        return;
    float now = 0.0f;
    void *manager = g_sb_get_instance ? g_sb_get_instance(NULL, NULL) : NULL;
    if (manager && g_sb_get_time)
        now = g_sb_get_time(manager, NULL);
    float *busy = (float *)((char *)weapon + g_sb_off_busy);
    float end = now + busy_time;
    uint64_t millis = monotonic_milliseconds();
    int refused = g_busy_guard && weapon == previous_weapon &&
                  millis - previous_millis < 250u &&
                  *busy - now > 0.5f && end > *busy;
    if (!refused)
        *busy = end;
    if (g_firelog)
        fprintf(stderr,
                "[firelog] SetBusy t=%.3f now=%.3f cur_end=%.3f%s\n",
                (double)busy_time, (double)now, (double)*busy,
                refused ? " (raise recusado)" : "");
    previous_weapon = weapon;
    previous_millis = millis;
}

static void busy_guard_install(dt_module *il2cpp) {
    il2cpp_class_get_field_from_name_fn class_get_field =
        (il2cpp_class_get_field_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_field_from_name");
    il2cpp_field_get_offset_fn field_offset = (il2cpp_field_get_offset_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_get_offset");
    const void *set_busy = find_method(il2cpp, "WeaponBase", "SetBusy", 1);
    const void *get_instance =
        find_method(il2cpp, "TimeManager", "get_Instance", 0);
    const void *get_time =
        find_method(il2cpp, "TimeManager", "get_timeSinceLevelLoad", 0);
    void *weapon_class = find_class(il2cpp, "WeaponBase");
    void *busy_field = weapon_class && class_get_field
        ? class_get_field(weapon_class, "Busy") : NULL;
    if (!set_busy || !get_instance || !get_time || !busy_field ||
        !field_offset) {
        fprintf(stderr,
                "[gamepad] aviso: SetBusy/TimeManager ausentes — janela de "
                "reload segue o jogo\n");
        return;
    }
    g_sb_get_instance = (m_obj_fn)*(const uintptr_t *)get_instance;
    g_sb_get_time = (m_float_fn)*(const uintptr_t *)get_time;
    g_sb_off_busy = field_offset(busy_field);
    if (replace_method_body(il2cpp, set_busy, (void *)&hooked_set_busy,
                            "SetBusy"))
        fprintf(stderr,
                "[gamepad] SetBusy com guarda anti-extensao instalado "
                "(fix reload 1a recarga)\n");
    else
        g_sb_get_instance = NULL;
}

/*
 * Speed probe + pace knob for the "zombies 2x" field reports (dArkOSRE,
 * Mali-G31). Gameplay is deltaTime-driven, so the fps cap cannot change the
 * wall-clock pace; the only mechanisms that can are the engine clock or
 * Time.timeScale. DT_SPEEDLOG=1 samples, once per second on the Unity
 * thread, game-time vs CLOCK_MONOTONIC (ratio), Time.timeScale, deltaTime,
 * unscaledDeltaTime and the rendered fps, which separates the two.
 * DT_TIME_SCALE=<x> (0.05..4.0) tames the normal-play timeScale to <x>,
 * leaving the game's own slow-motion states (<1) untouched. Neither is
 * enabled by the release launcher.
 *
 * Pace governor (DT_PACE=0 disables): measured live on the R36S/ArkOS, the
 * engine advances SCALED game time by the sum of the LAST TWO frame
 * intervals per frame (dt = udt(n) + udt(n-1)) while unscaledDeltaTime and
 * timeScale stay correct — game time runs exactly 2x wall clock at ANY fps,
 * which is the "zombies too fast" field report and why the fps cap never
 * changed the pace. The governor never trusts the theory alone: it engages
 * only after measuring a sustained game-time/wall-time ratio of ~2 with the
 * game in the normal timeScale=1 state, then applies a proportional 0.5
 * compensation to EVERY timeScale the game sets (slow-motion keeps its
 * designed feel: 0.3 -> engine 0.15 -> effective 0.3 real).
 */
static m_float_fn g_sp_get_time;      /* UnityEngine.Time.get_time */
static m_float_fn g_sp_get_delta;     /* get_deltaTime */
static m_float_fn g_sp_get_unscaled;  /* get_unscaledDeltaTime */
static m_float_fn g_sp_get_scale;     /* get_timeScale */
static m_int_fn g_sp_get_framecount;  /* get_frameCount */
typedef void (*m_set_float_fn)(float, void *);
static m_set_float_fn g_sp_set_scale; /* set_timeScale */
static int g_speedlog;
static float g_time_scale_override;   /* 0 = knob off */
static int g_sp_ready;
static int g_pace_enabled = 1;        /* DT_PACE=0 turns the governor off */
static int g_pace_engaged;            /* 0 = still measuring */
static int g_pace_hits;               /* consecutive ~2x samples */
static float g_pace_factor = 0.5f;    /* compensation applied when engaged */
static float g_pace_applied = -1.0f;  /* last timeScale written by us */

/*
 * Intended-view fix for the governor's side effects on audio ("zombie moans
 * in slow motion", "music only partially right"). The game derives audio
 * pitch from the timeScale it reads back: MusicManager.Update and
 * AgentHuman.LateUpdate both do pitch = timeScale >= 1 ? 1 :
 * Mathf.Max(timeScale, 0.5) — with the compensated 0.5 permanently visible,
 * every AgentHuman voice and the music track play at half pitch.
 * TimeManager.GetRealDeltaTime (deltaTime/timeScale) is skewed the same way.
 *
 * The C# wrapper of Time.get_timeScale is therefore replaced so the GAME
 * always observes the pace it asked for (a healthy engine's view): the raw
 * engine value divided by the compensation while it matches our own write,
 * untouched otherwise (fresh game writes ARE the intent; 0 means paused).
 * The governor itself reads the raw value through the native icall, which
 * survives the wrapper patch. DT_PITCH_FIX=0 restores the old behavior.
 */
typedef float (*icall_get_float_fn)(void);
typedef void *(*il2cpp_resolve_icall_fn)(const char *);
static icall_get_float_fn g_raw_get_scale;
static int g_pitch_fix = 1;

static float read_raw_time_scale(void) {
    return g_raw_get_scale ? g_raw_get_scale()
                           : g_sp_get_scale(NULL, NULL);
}

static float hooked_get_time_scale(void) {
    float raw = g_raw_get_scale();
    if (!g_pace_engaged || raw <= 0.0005f)
        return raw;
    if (fabsf(raw - g_pace_applied) <= 0.0005f)
        return raw / g_pace_factor;
    return raw;
}

static void speed_probe_install(dt_module *il2cpp) {
    const char *log_setting = getenv("DT_SPEEDLOG");
    const char *scale_setting = getenv("DT_TIME_SCALE");
    const char *pace_setting = getenv("DT_PACE");
    g_speedlog = log_setting && atoi(log_setting) != 0;
    g_time_scale_override = scale_setting ? (float)atof(scale_setting) : 0.0f;
    if (g_time_scale_override < 0.05f || g_time_scale_override > 4.0f)
        g_time_scale_override = 0.0f;
    if (pace_setting && atoi(pace_setting) == 0)
        g_pace_enabled = 0;
    if (g_time_scale_override > 0.0f)
        g_pace_enabled = 0;   /* manual knob wins over the governor */
    if (!g_speedlog && g_time_scale_override == 0.0f && !g_pace_enabled)
        return;
    const void *m_time =
        find_method_ns(il2cpp, "UnityEngine", "Time", "get_time", 0);
    const void *m_delta =
        find_method_ns(il2cpp, "UnityEngine", "Time", "get_deltaTime", 0);
    const void *m_unscaled =
        find_method_ns(il2cpp, "UnityEngine", "Time",
                       "get_unscaledDeltaTime", 0);
    const void *m_scale =
        find_method_ns(il2cpp, "UnityEngine", "Time", "get_timeScale", 0);
    const void *m_frames =
        find_method_ns(il2cpp, "UnityEngine", "Time", "get_frameCount", 0);
    const void *m_set =
        find_method_ns(il2cpp, "UnityEngine", "Time", "set_timeScale", 1);
    if (!m_time || !m_delta || !m_unscaled || !m_scale || !m_frames ||
        !m_set) {
        fprintf(stderr,
                "[speed] aviso: UnityEngine.Time incompleto — sonda "
                "desligada\n");
        return;
    }
    g_sp_get_time = (m_float_fn)*(const uintptr_t *)m_time;
    g_sp_get_delta = (m_float_fn)*(const uintptr_t *)m_delta;
    g_sp_get_unscaled = (m_float_fn)*(const uintptr_t *)m_unscaled;
    g_sp_get_scale = (m_float_fn)*(const uintptr_t *)m_scale;
    g_sp_get_framecount = (m_int_fn)*(const uintptr_t *)m_frames;
    g_sp_set_scale = (m_set_float_fn)*(const uintptr_t *)m_set;
    g_sp_ready = 1;
    fprintf(stderr, "[speed] sonda instalada (log=%d timeScale=%.2f)\n",
            g_speedlog, (double)g_time_scale_override);

    const char *pitch_setting = getenv("DT_PITCH_FIX");
    if (pitch_setting && atoi(pitch_setting) == 0)
        g_pitch_fix = 0;
    if (!g_pitch_fix || !g_pace_enabled)
        return;
    il2cpp_resolve_icall_fn resolve_icall = (il2cpp_resolve_icall_fn)
        dt_module_symbol(il2cpp, "il2cpp_resolve_icall");
    if (resolve_icall) {
        g_raw_get_scale = (icall_get_float_fn)
            resolve_icall("UnityEngine.Time::get_timeScale()");
        if (!g_raw_get_scale)
            g_raw_get_scale = (icall_get_float_fn)
                resolve_icall("UnityEngine.Time::get_timeScale");
    }
    if (!g_raw_get_scale) {
        fprintf(stderr,
                "[speed] aviso: icall get_timeScale nao resolvido — pitch "
                "de audio segue o timeScale compensado\n");
        return;
    }
    if (replace_method_body(il2cpp, m_scale,
                            (void *)&hooked_get_time_scale,
                            "get_timeScale"))
        fprintf(stderr,
                "[speed] get_timeScale devolve o ritmo pretendido ao jogo "
                "(fix pitch musica/zumbis)\n");
    else
        g_raw_get_scale = NULL;
}

static void speed_probe_tick(void) {
    if (!g_sp_ready)
        return;
    /* Raw ENGINE value on purpose: after the intended-view hook installs,
     * the C# getter reports the game's intent, not what we wrote. */
    float scale = read_raw_time_scale();
    if (g_time_scale_override > 0.0f && scale > 0.999f &&
        fabsf(scale - g_time_scale_override) > 0.001f) {
        /* Only the normal-play state is tamed; slow-mo (<1) stays owned by
         * the game's TimeManager. */
        g_sp_set_scale(g_time_scale_override, NULL);
        scale = g_time_scale_override;
    }
    if (g_pace_engaged && scale <= 0.0005f) {
        /* Frozen: TimeManager.PauseTime() saves the LIVE Time.timeScale into
         * m_StoredScale and writes 0; UnpauseTime() writes m_StoredScale
         * back. Compensating the 0 would clear our memory of the running
         * value, so the restored (already compensated) scale would look like
         * a fresh intent and get halved again — every pause/resume halved the
         * pace, and with it the 360*deltaTime view clamp, which is the
         * "aim gets slower on every pause" report. Leave the freeze alone and
         * keep g_pace_applied, so the restore matches and is absorbed. */
    } else if (g_pace_engaged && fabsf(scale - g_pace_applied) > 0.0005f) {
        /* The game wrote a new timeScale (slow-mo in/out). Apply the
         * proportional compensation to its intent, never on top of our own
         * previous write. */
        float target = scale * g_pace_factor;
        g_sp_set_scale(target, NULL);
        g_pace_applied = target;
        scale = target;
    }
    static uint64_t wall_prev;
    static float game_prev;
    static int32_t frame_prev;
    static int primed;
    uint64_t wall = monotonic_milliseconds();
    if (!primed) {
        primed = 1;
        wall_prev = wall;
        game_prev = g_sp_get_time(NULL, NULL);
        frame_prev = g_sp_get_framecount(NULL, NULL);
        return;
    }
    if (wall - wall_prev < 1000u)
        return;
    float game = g_sp_get_time(NULL, NULL);
    int32_t frames = g_sp_get_framecount(NULL, NULL);
    float dwall = (float)(wall - wall_prev) / 1000.0f;
    float dgame = game - game_prev;
    float ratio = dwall > 0.0f ? dgame / dwall : 0.0f;
    if (g_pace_enabled && !g_pace_engaged) {
        /* Engage only from the pristine state: timeScale untouched at 1.0
         * and a sustained ~2x game-time/wall-time ratio. A healthy engine
         * (ratio ~1) never trips this. */
        if (scale > 0.95f && scale < 1.05f &&
            ratio > 1.6f && ratio < 2.6f)
            ++g_pace_hits;
        else
            g_pace_hits = 0;
        if (g_pace_hits >= 3) {
            g_pace_engaged = 1;
            float target = scale * g_pace_factor;
            g_sp_set_scale(target, NULL);
            g_pace_applied = target;
            fprintf(stderr,
                    "[speed] inflacao de deltaTime medida (ratio=%.3f) — "
                    "compensacao timeScale x%.2f engatada\n",
                    (double)ratio, (double)g_pace_factor);
            scale = target;
        }
    }
    if (g_speedlog)
        fprintf(stderr,
                "[speed] ratio=%.3f dgame=%.2f dwall=%.2f fps=%.1f "
                "timeScale=%.3f dt=%.4f udt=%.4f%s\n",
                (double)ratio, (double)dgame, (double)dwall,
                dwall > 0.0f
                    ? (double)((float)(frames - frame_prev) / dwall) : 0.0,
                (double)scale, (double)g_sp_get_delta(NULL, NULL),
                (double)g_sp_get_unscaled(NULL, NULL),
                g_pace_engaged ? " [governado]" : "");
    wall_prev = wall;
    game_prev = game;
    frame_prev = frames;
}

static void firelog_install(dt_module *il2cpp) {
    if (!g_firelog)
        return;
    il2cpp_class_get_field_from_name_fn class_get_field =
        (il2cpp_class_get_field_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_field_from_name");
    g_fl_static_get = (il2cpp_field_static_get_value_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_static_get_value");
    g_fl_field_offset = (il2cpp_field_get_offset_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_get_offset");
    void *c_player = find_class(il2cpp, "Player");
    void *c_component = find_class(il2cpp, "ComponentPlayer");
    void *c_agent = find_class(il2cpp, "AgentHuman");
    void *c_bb = find_class(il2cpp, "BlackBoard");
    const void *m_canfire = find_method(il2cpp, "AgentHuman", "CanFire", 0);
    const void *m_curweap =
        find_method(il2cpp, "ComponentWeapons", "GetCurrentWeapon", 0);
    const void *m_clip = find_method(il2cpp, "WeaponBase", "get_ClipAmmo", 0);
    const void *m_busy = find_method(il2cpp, "WeaponBase", "IsBusy", 0);
    if (!class_get_field || !g_fl_static_get || !g_fl_field_offset ||
        !c_player || !c_component || !c_agent || !c_bb ||
        !m_canfire || !m_curweap || !m_clip || !m_busy) {
        fprintf(stderr, "[firelog] metadata incompleta; log desativado\n");
        return;
    }
    g_fl_player_instance = class_get_field(c_player, "Instance");
    void *f_owner = class_get_field(c_component, "<Owner>k__BackingField");
    void *f_bb = class_get_field(c_agent, "BlackBoard");
    void *f_wcomp =
        class_get_field(c_agent, "<WeaponComponent>k__BackingField");
    void *f_busyaction = class_get_field(c_bb, "BusyAction");
    void *f_actionpoint = class_get_field(c_bb, "ActionPointOn");
    void *f_stop = class_get_field(c_bb, "<Stop>k__BackingField");
    if (!g_fl_player_instance || !f_owner || !f_bb || !f_wcomp ||
        !f_busyaction || !f_actionpoint || !f_stop) {
        fprintf(stderr, "[firelog] campos ausentes; log desativado\n");
        return;
    }
    g_fl_off_owner = g_fl_field_offset(f_owner);
    g_fl_off_bb = g_fl_field_offset(f_bb);
    g_fl_off_wcomp = g_fl_field_offset(f_wcomp);
    g_fl_off_busyaction = g_fl_field_offset(f_busyaction);
    g_fl_off_actionpoint = g_fl_field_offset(f_actionpoint);
    g_fl_off_stop = g_fl_field_offset(f_stop);
    g_fl_canfire = (m_bool_fn)*(const uintptr_t *)m_canfire;
    g_fl_getcurweapon = (m_obj_fn)*(const uintptr_t *)m_curweap;
    g_fl_clipammo = (m_int_fn)*(const uintptr_t *)m_clip;
    g_fl_isbusy = (m_bool_fn)*(const uintptr_t *)m_busy;
    g_fl_ready = 1;
    fprintf(stderr, "[firelog] gates instalados (CanFire/BusyAction/"
            "ActionPoint/Stop/IsBusy/ClipAmmo)\n");
}

static uint64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000u +
           (uint64_t)value.tv_nsec / 1000000u;
}

/*
 * DT_ATTACKLOG=1: diagnostic probe for the field reports "zombie swing too
 * fast", "zombies silent when attacking" and "storage melts in defense
 * missions". Three faithful reimplementations with logging on top:
 * ComponentEnemy.PlayAttackSound (does the attack roar even get requested?),
 * AgentHuman.SoundUniquePlay(clip) (all agent voice traffic) and
 * DestructibleObject.TakeDamage (damage per swing + cadence + HP drain).
 * Never enabled by the release launcher; nothing here runs without the env.
 */
typedef void (*il2cpp_field_static_set_value_fn)(void *, void *);
typedef void *(*il2cpp_string_new_fn)(const char *);
typedef float (*m_sup3_fn)(void *, void *, float, void *, void *);
typedef uint8_t (*m_bool_obj_fn)(void *, void *, void *);
typedef void (*m_ondamage_fn)(void *, void *, float, uint8_t, void *);

static int g_attacklog;
static il2cpp_string_new_fn g_al_string_new;
static il2cpp_field_static_set_value_fn g_al_static_set;
static m_obj_fn g_al_get_attack;      /* ComponentEnemy.get_AttackSound */
static m_obj_fn g_al_get_spit;        /* ComponentEnemy.get_SpitSound */
static m_obj_fn g_al_obj_name;        /* UnityEngine.Object.get_name */
static m_float_fn g_al_clip_length;   /* AudioClip.get_length */
static m_float_fn g_al_time_sll;      /* Time.get_timeSinceLevelLoad */
static m_sup3_fn g_al_sup3;           /* SoundUniquePlay(clip, lock, name) */
static m_bool_obj_fn g_al_is_playing; /* AgentHuman.IsSoundPlaying(clip) */
static m_ondamage_fn g_al_ondamage;   /* DestructibleObject.OnDamage */
static size_t g_al_off_ce_owner;      /* ComponentEnemy.<Owner>k__BackingField */
static size_t g_al_off_asp;           /* ComponentEnemy.AttackSoundPlaying */
static void *g_al_f_dontspeak;        /* static ComponentEnemy.DontSpeakTimer */
static size_t g_al_off_versions;      /* DestructibleObject.m_Versions */
static size_t g_al_off_currversion;   /* DestructibleObject.m_CurrVersion */
static size_t g_al_off_remaining;     /* DestructibleObject.m_RemainingHP */
static size_t g_al_off_total;         /* DestructibleObject.m_TotalHP */

static void il2string_ascii(void *str, char *out, size_t cap) {
    out[0] = 0;
    if (!str || cap < 2)
        return;
    int32_t length = *(int32_t *)((char *)str + 0x10);
    const uint16_t *chars = (const uint16_t *)((char *)str + 0x14);
    if (length < 0 || length > 255)
        return;
    size_t written = 0;
    for (int32_t index = 0; index < length && written + 1 < cap; ++index)
        out[written++] = chars[index] >= 32 && chars[index] < 127
            ? (char)chars[index] : '?';
    out[written] = 0;
}

/* AgentHuman.SoundUniquePlay(clip) == SoundUniquePlay(clip, clip.length,
 * clip.name); reimplemented verbatim with a log line per voice play. */
static float hooked_sound_unique_play1(void *agent, void *clip) {
    if (!clip || !g_al_sup3)
        return 0.0f;
    float length = g_al_clip_length ? g_al_clip_length(clip, NULL) : 0.0f;
    void *name_obj = g_al_obj_name ? g_al_obj_name(clip, NULL) : NULL;
    uint8_t busy = g_al_is_playing ? g_al_is_playing(agent, clip, NULL) : 0;
    float result = g_al_sup3(agent, clip, length, name_obj, NULL);
    char name[48];
    il2string_ascii(name_obj, name, sizeof name);
    fprintf(stderr,
            "[atk] %llu voice agent=%p clip=%s len=%.2f ja_tocando=%u\n",
            (unsigned long long)monotonic_milliseconds(), agent, name,
            (double)length, busy);
    return result;
}

/* ComponentEnemy.PlayAttackSound(type): clip = type == 6 (E_MELEE_SPIT)
 * ? SpitSound : AttackSound; AttackSoundPlaying = true;
 * Owner.SoundUniquePlay(clip, clip.length, "Attack");
 * DontSpeakTimer = Time.timeSinceLevelLoad + clip.length; returns length. */
static float hooked_play_attack_sound(void *self, int32_t type) {
    uint64_t now = monotonic_milliseconds();
    void *clip = type == 6
        ? (g_al_get_spit ? g_al_get_spit(self, NULL) : NULL)
        : (g_al_get_attack ? g_al_get_attack(self, NULL) : NULL);
    if (!clip) {
        fprintf(stderr, "[atk] %llu roar type=%d clip=NULL (sem som!)\n",
                (unsigned long long)now, type);
        return 0.0f;
    }
    if (g_al_off_asp)
        *(uint8_t *)((char *)self + g_al_off_asp) = 1;
    float length = g_al_clip_length ? g_al_clip_length(clip, NULL) : 0.0f;
    void *owner = fl_read_ptr(self, g_al_off_ce_owner);
    uint8_t busy = owner && g_al_is_playing
        ? g_al_is_playing(owner, clip, NULL) : 0;
    if (owner && g_al_sup3) {
        void *lock_name =
            g_al_string_new ? g_al_string_new("Attack") : NULL;
        g_al_sup3(owner, clip, length, lock_name, NULL);
    }
    float level_time = g_al_time_sll ? g_al_time_sll(NULL, NULL) : 0.0f;
    if (g_al_f_dontspeak && g_al_static_set) {
        float until = level_time + length;
        g_al_static_set(g_al_f_dontspeak, &until);
    }
    char name[48];
    void *name_obj = g_al_obj_name ? g_al_obj_name(clip, NULL) : NULL;
    il2string_ascii(name_obj, name, sizeof name);
    fprintf(stderr,
            "[atk] %llu roar type=%d clip=%s len=%.2f owner=%p "
            "ja_tocando=%u t=%.2f\n",
            (unsigned long long)now, type, name, (double)length, owner,
            busy, (double)level_time);
    return length;
}

/* DestructibleObject.TakeDamage(attacker, damage): forwards to
 * OnDamage(attacker, damage, true) while versions remain; logs the swing. */
static void hooked_take_damage(void *self, void *attacker, float damage) {
    void *versions = fl_read_ptr(self, g_al_off_versions);
    /* List<T> on arm64: _items @0x10, _size @0x18. */
    int32_t count = versions ? *(int32_t *)((char *)versions + 0x18) : 0;
    int32_t current = g_al_off_currversion
        ? *(int32_t *)((char *)self + g_al_off_currversion) : 0;
    float before = g_al_off_remaining
        ? *(float *)((char *)self + g_al_off_remaining) : 0.0f;
    float total = g_al_off_total
        ? *(float *)((char *)self + g_al_off_total) : 0.0f;
    if (versions && current < count - 1 && g_al_ondamage)
        g_al_ondamage(self, attacker, damage, 1, NULL);
    float after = g_al_off_remaining
        ? *(float *)((char *)self + g_al_off_remaining) : 0.0f;
    fprintf(stderr,
            "[atk] %llu dano obj=%p agressor=%p dmg=%.2f hp=%.1f->%.1f/"
            "%.1f ver=%d/%d\n",
            (unsigned long long)monotonic_milliseconds(), self, attacker,
            (double)damage, (double)before, (double)after, (double)total,
            current, count);
}

/*
 * Attack-roar fix ("zombies stay completely silent when attacking", Discord
 * field report). UpdateSpeech only requests the melee roar when
 * Owner.IsVisible && !BlackBoard.Stop && Owner.IsAlive &&
 * !AttackSoundPlaying && MotionType == Attack(3), and AgentHuman.IsVisible
 * is just Renderer.isVisible of the REFERENCE SkinnedMeshRenderer. Measured
 * live on the R36S: zombies drawn through LOD1/LOD2 keep the reference
 * renderer culled (vis=0 while lods=v0,v1,v0), so an on-screen zombie counts
 * as invisible and never roars — only frames where LOD0 takes over let the
 * roar through ("I hear it sometimes"). The getter is reimplemented to treat
 * the agent as visible when ANY of its renderers (reference or LOD) is
 * visible, which is the getter's evident intent. DT_VIS_FIX=0 restores the
 * original reference-only behavior. Validated live on the R36S (every
 * attacking zombie roars again). The gate log below only runs under
 * DT_ATTACKLOG.
 */
static m_bool_fn g_al_renderer_visible;  /* Renderer.get_isVisible */
static m_bool_fn g_al_renderer_enabled;  /* Renderer.get_enabled */
static size_t g_al_off_agent_renderer;   /* AgentHuman.<Renderer>k__BackingField */
static size_t g_al_off_agent_lods;       /* AgentHuman.<LodRenderers>k__BackingField */
static size_t g_al_off_agent_bb;         /* AgentHuman.BlackBoard */
static size_t g_al_off_agent_enemy;      /* AgentHuman.<EnemyComponent>k__BackingField */
static size_t g_al_off_bb_motion;        /* BlackBoard.MotionType */
static size_t g_al_off_bb_stop;          /* BlackBoard.<Stop>k__BackingField */

static int g_vis_fix = 1;                /* DT_VIS_FIX=0 desliga o OR de LODs */

static uint8_t hooked_get_is_visible(void *agent) {
    void *renderer = fl_read_ptr(agent, g_al_off_agent_renderer);
    uint8_t visible = renderer && g_al_renderer_visible
        ? (g_al_renderer_visible(renderer, NULL) & 1) : 0;
    if (!visible && g_vis_fix && g_al_off_agent_lods &&
        g_al_renderer_visible) {
        /* Candidate fix: the reference SkinnedMeshRenderer goes dark when
         * the LOD system draws a sibling; the agent is still on screen, so
         * treat any visible LOD renderer as visible. */
        void *lod_array = fl_read_ptr(agent, g_al_off_agent_lods);
        int32_t count = lod_array
            ? *(int32_t *)((char *)lod_array + 0x18) : 0;
        for (int32_t index = 0; index < count && index < 8 && !visible;
             ++index) {
            void *lod = ((void **)((char *)lod_array + 0x20))[index];
            if (lod)
                visible = g_al_renderer_visible(lod, NULL) & 1;
        }
    }
    if (!g_attacklog)
        return visible;
    void *board = fl_read_ptr(agent, g_al_off_agent_bb);
    int32_t motion = board && g_al_off_bb_motion
        ? *(int32_t *)((char *)board + g_al_off_bb_motion) : -1;
    if (motion == 3) {
        static uint64_t last_millis;
        uint64_t now = monotonic_milliseconds();
        if (now - last_millis >= 1000u) {
            last_millis = now;
            uint8_t stop = board && g_al_off_bb_stop
                ? fl_read_bool(board, g_al_off_bb_stop) : 0;
            void *enemy = fl_read_ptr(agent, g_al_off_agent_enemy);
            uint8_t roaring = enemy && g_al_off_asp
                ? fl_read_bool(enemy, g_al_off_asp) : 0;
            uint8_t ref_enabled = renderer && g_al_renderer_enabled
                ? (g_al_renderer_enabled(renderer, NULL) & 1) : 0;
            /* LodRenderers: il2cpp array on arm64 keeps the length at
             * 0x18 and pointer elements from 0x20. */
            char lods[64] = "-";
            void *lod_array = fl_read_ptr(agent, g_al_off_agent_lods);
            if (lod_array) {
                int32_t count =
                    *(int32_t *)((char *)lod_array + 0x18);
                size_t used = 0;
                lods[0] = 0;
                for (int32_t index = 0;
                     index < count && index < 4 &&
                     used + 8 < sizeof lods; ++index) {
                    void *lod = ((void **)((char *)lod_array + 0x20))
                        [index];
                    uint8_t lv = lod && g_al_renderer_visible
                        ? (g_al_renderer_visible(lod, NULL) & 1) : 0;
                    uint8_t le = lod && g_al_renderer_enabled
                        ? (g_al_renderer_enabled(lod, NULL) & 1) : 0;
                    used += (size_t)snprintf(lods + used,
                                             sizeof lods - used,
                                             "%sv%ue%u",
                                             index ? "," : "", lv, le);
                }
            }
            fprintf(stderr,
                    "[atk] %llu gate agent=%p vis=%u en=%u stop=%u "
                    "roar_flag=%u lods=%s\n",
                    (unsigned long long)now, agent, visible, ref_enabled,
                    stop, roaring, lods);
        }
    }
    return visible;
}

static void attacklog_install(dt_module *il2cpp) {
    if (!g_attacklog)
        return;
    il2cpp_class_get_field_from_name_fn class_get_field =
        (il2cpp_class_get_field_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_field_from_name");
    il2cpp_field_get_offset_fn field_offset = (il2cpp_field_get_offset_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_get_offset");
    g_al_string_new = (il2cpp_string_new_fn)
        dt_module_symbol(il2cpp, "il2cpp_string_new");
    g_al_static_set = (il2cpp_field_static_set_value_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_static_set_value");
    const void *m_play =
        find_method(il2cpp, "ComponentEnemy", "PlayAttackSound", 1);
    const void *m_attack =
        find_method(il2cpp, "ComponentEnemy", "get_AttackSound", 0);
    const void *m_spit =
        find_method(il2cpp, "ComponentEnemy", "get_SpitSound", 0);
    const void *m_sup1 =
        find_method(il2cpp, "AgentHuman", "SoundUniquePlay", 1);
    const void *m_sup3 =
        find_method(il2cpp, "AgentHuman", "SoundUniquePlay", 3);
    const void *m_isplay =
        find_method(il2cpp, "AgentHuman", "IsSoundPlaying", 1);
    const void *m_length =
        find_method_ns(il2cpp, "UnityEngine", "AudioClip", "get_length", 0);
    const void *m_name =
        find_method_ns(il2cpp, "UnityEngine", "Object", "get_name", 0);
    const void *m_sll = find_method_ns(il2cpp, "UnityEngine", "Time",
                                       "get_timeSinceLevelLoad", 0);
    const void *m_take =
        find_method(il2cpp, "DestructibleObject", "TakeDamage", 2);
    const void *m_ondmg =
        find_method(il2cpp, "DestructibleObject", "OnDamage", 3);
    void *c_enemy = find_class(il2cpp, "ComponentEnemy");
    void *c_destr = find_class(il2cpp, "DestructibleObject");
    if (!class_get_field || !field_offset || !m_play || !m_attack ||
        !m_spit || !m_sup1 || !m_sup3 || !m_isplay || !m_length ||
        !m_name || !m_sll || !m_take || !m_ondmg || !c_enemy || !c_destr) {
        fprintf(stderr, "[atk] metadata incompleta; attacklog desativado\n");
        return;
    }
    void *f_owner = class_get_field(c_enemy, "<Owner>k__BackingField");
    void *f_asp = class_get_field(c_enemy, "AttackSoundPlaying");
    g_al_f_dontspeak = class_get_field(c_enemy, "DontSpeakTimer");
    void *f_versions = class_get_field(c_destr, "m_Versions");
    void *f_curr = class_get_field(c_destr, "m_CurrVersion");
    void *f_remaining = class_get_field(c_destr, "m_RemainingHP");
    void *f_total = class_get_field(c_destr, "m_TotalHP");
    if (!f_owner || !f_asp || !g_al_f_dontspeak || !f_versions || !f_curr ||
        !f_remaining || !f_total) {
        fprintf(stderr, "[atk] campos ausentes; attacklog desativado\n");
        return;
    }
    g_al_off_ce_owner = field_offset(f_owner);
    g_al_off_asp = field_offset(f_asp);
    g_al_off_versions = field_offset(f_versions);
    g_al_off_currversion = field_offset(f_curr);
    g_al_off_remaining = field_offset(f_remaining);
    g_al_off_total = field_offset(f_total);
    g_al_get_attack = (m_obj_fn)*(const uintptr_t *)m_attack;
    g_al_get_spit = (m_obj_fn)*(const uintptr_t *)m_spit;
    g_al_sup3 = (m_sup3_fn)*(const uintptr_t *)m_sup3;
    g_al_is_playing = (m_bool_obj_fn)*(const uintptr_t *)m_isplay;
    g_al_clip_length = (m_float_fn)*(const uintptr_t *)m_length;
    g_al_obj_name = (m_obj_fn)*(const uintptr_t *)m_name;
    g_al_time_sll = (m_float_fn)*(const uintptr_t *)m_sll;
    g_al_ondamage = (m_ondamage_fn)*(const uintptr_t *)m_ondmg;
    int installed = 0;
    installed += replace_method_body(il2cpp, m_play,
                                     (void *)&hooked_play_attack_sound,
                                     "PlayAttackSound");
    installed += replace_method_body(il2cpp, m_sup1,
                                     (void *)&hooked_sound_unique_play1,
                                     "SoundUniquePlay(clip)");
    installed += replace_method_body(il2cpp, m_take,
                                     (void *)&hooked_take_damage,
                                     "TakeDamage");
    fprintf(stderr, "[atk] attacklog instalado (%d/3 hooks)\n", installed);
}

static void visfix_install(dt_module *il2cpp) {
    gamepad_env_init();
    if (!g_vis_fix && !g_attacklog)
        return;
    il2cpp_class_get_field_from_name_fn class_get_field =
        (il2cpp_class_get_field_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_field_from_name");
    il2cpp_field_get_offset_fn field_offset = (il2cpp_field_get_offset_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_get_offset");
    const void *m_isvis =
        find_method(il2cpp, "AgentHuman", "get_IsVisible", 0);
    const void *m_rvis = find_method_ns(il2cpp, "UnityEngine", "Renderer",
                                        "get_isVisible", 0);
    const void *m_ren = find_method_ns(il2cpp, "UnityEngine", "Renderer",
                                       "get_enabled", 0);
    void *c_agent = find_class(il2cpp, "AgentHuman");
    void *c_board = find_class(il2cpp, "BlackBoard");
    void *f_renderer = c_agent && class_get_field
        ? class_get_field(c_agent, "<Renderer>k__BackingField") : NULL;
    void *f_lods = c_agent && class_get_field
        ? class_get_field(c_agent, "<LodRenderers>k__BackingField") : NULL;
    if (!class_get_field || !field_offset || !m_isvis || !m_rvis ||
        !f_renderer || !f_lods) {
        fprintf(stderr,
                "[gamepad] aviso: metadata de IsVisible ausente — roar de "
                "ataque segue o culling do LOD0\n");
        return;
    }
    g_al_renderer_visible = (m_bool_fn)*(const uintptr_t *)m_rvis;
    if (m_ren)
        g_al_renderer_enabled = (m_bool_fn)*(const uintptr_t *)m_ren;
    g_al_off_agent_renderer = field_offset(f_renderer);
    g_al_off_agent_lods = field_offset(f_lods);
    /* Gate-sampler extras (log only, best effort). */
    void *f_agent_bb = class_get_field(c_agent, "BlackBoard");
    void *f_agent_enemy =
        class_get_field(c_agent, "<EnemyComponent>k__BackingField");
    void *f_motion = c_board
        ? class_get_field(c_board, "MotionType") : NULL;
    void *f_stop = c_board
        ? class_get_field(c_board, "<Stop>k__BackingField") : NULL;
    if (f_agent_bb)
        g_al_off_agent_bb = field_offset(f_agent_bb);
    if (f_agent_enemy)
        g_al_off_agent_enemy = field_offset(f_agent_enemy);
    if (f_motion)
        g_al_off_bb_motion = field_offset(f_motion);
    if (f_stop)
        g_al_off_bb_stop = field_offset(f_stop);
    if (replace_method_body(il2cpp, m_isvis,
                            (void *)&hooked_get_is_visible,
                            "get_IsVisible"))
        fprintf(stderr,
                "[gamepad] IsVisible considera todos os LODs (fix roar de "
                "ataque)%s\n", g_vis_fix ? "" : " [SOMENTE LOG]");
}

static void gamepad_env_init(void) {
    static int initialized;
    if (initialized)
        return;
    initialized = 1;
    const char *setting;
    if ((setting = getenv("DT_LOOK_SCALE")))
        g_look_scale = strtof(setting, NULL);
    if ((setting = getenv("DT_LOOK_EXPO")))
        g_look_expo = strtof(setting, NULL);
    if ((setting = getenv("DT_VIEW_SENS_X")))
        g_view_sens_x = strtof(setting, NULL);
    if ((setting = getenv("DT_VIEW_SENS_Y")))
        g_view_sens_y = strtof(setting, NULL);
    if ((setting = getenv("DT_FIRE_STICKY")))
        g_fire_sticky_up = atoi(setting) != 0;
    if ((setting = getenv("DT_FIRE_LEVEL")))
        g_fire_level_down = atoi(setting) != 0;
    g_padlog = getenv("DT_PADLOG") != NULL;
    g_firelog = getenv("DT_FIRELOG") != NULL;
    g_attacklog = getenv("DT_ATTACKLOG") != NULL;
    if ((setting = getenv("DT_VIS_FIX")))
        g_vis_fix = atoi(setting) != 0;
    if ((setting = getenv("DT_BUSY_GUARD")))
        g_busy_guard = atoi(setting) != 0;
    fprintf(stderr,
            "[gamepad] look_scale=%.2f look_expo=%.2f "
            "view_sens=%.1f/%.1f fire_sticky=%d fire_level=%d\n",
            g_look_scale, g_look_expo,
            g_view_sens_x, g_view_sens_y, g_fire_sticky_up,
            g_fire_level_down);
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float bits_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float clamp_axis(float value) {
    const float dead_zone = 0.16f;
    float magnitude = fabsf(value);
    if (magnitude < dead_zone)
        return 0.0f;
    /* Rescale so motion starts from zero at the dead-zone edge instead of
     * jumping straight to 0.16 — the raw cut made fine aim impossible. */
    magnitude = (magnitude - dead_zone) / (1.0f - dead_zone);
    if (magnitude > 1.0f)
        magnitude = 1.0f;
    return value < 0.0f ? -magnitude : magnitude;
}

void dt_gamepad_publish(float left_x, float left_y,
                        float right_x, float right_y,
                        uint32_t semantic_level, int enabled) {
    if (!enabled) {
        left_x = left_y = right_x = right_y = 0.0f;
        semantic_level = 0;
    }
    semantic_level &= (1u << (DT_INPUT_ACTION + 1)) - 1u;

    atomic_store_explicit(&g_left_x_bits, float_bits(clamp_axis(left_x)),
                          memory_order_relaxed);
    atomic_store_explicit(&g_left_y_bits, float_bits(clamp_axis(left_y)),
                          memory_order_relaxed);
    atomic_store_explicit(&g_right_x_bits, float_bits(clamp_axis(right_x)),
                          memory_order_relaxed);
    atomic_store_explicit(&g_right_y_bits, float_bits(clamp_axis(right_y)),
                          memory_order_relaxed);

    uint32_t old = atomic_exchange_explicit(
        &g_level, semantic_level, memory_order_acq_rel);
    uint32_t pressed = semantic_level & ~old;
    uint32_t released = old & ~semantic_level;
    if (pressed)
        atomic_fetch_or_explicit(&g_pending_down, pressed,
                                 memory_order_release);
    if (released)
        atomic_fetch_or_explicit(&g_pending_up, released,
                                 memory_order_release);
}

void dt_gamepad_frame_begin(void) {
    gamepad_env_init();
    uint32_t level =
        atomic_load_explicit(&g_level, memory_order_acquire);
    g_frame_level = level;
    uint32_t new_down =
        atomic_exchange_explicit(&g_pending_down, 0, memory_order_acq_rel) |
        (level & ~g_previous_level);
    uint32_t new_up =
        atomic_exchange_explicit(&g_pending_up, 0, memory_order_acq_rel) |
        (g_previous_level & ~level);
    g_down_latch = (g_down_latch & ~g_down_read) | new_down;
    g_up_latch = (g_up_latch & ~g_up_read) | new_up;
    g_down_read = 0;
    g_up_read = 0;
    if (g_padlog && (new_down || new_up))
        fprintf(stderr, "[pad] down=%03x up=%03x level=%03x\n",
                new_down, new_up, level);
    g_previous_level = level;

    g_frame_left_x = bits_float(atomic_load_explicit(
        &g_left_x_bits, memory_order_relaxed));
    g_frame_left_y = bits_float(atomic_load_explicit(
        &g_left_y_bits, memory_order_relaxed));
    g_frame_right_x = bits_float(atomic_load_explicit(
        &g_right_x_bits, memory_order_relaxed));
    g_frame_right_y = bits_float(atomic_load_explicit(
        &g_right_y_bits, memory_order_relaxed));
    speed_probe_tick();
}

/*
 * IL2CPP static methods normally receive E_Input in x0. Keeping the x1
 * fallback also covers older generated wrappers that retained a null
 * synthetic "this" argument; no fixed ABI guess is needed at runtime.
 */
static int semantic_argument(uintptr_t arg0, uintptr_t arg1) {
    if (arg1 < DT_INPUT_COUNT && (arg0 == 0 || arg0 >= DT_INPUT_COUNT))
        return (int)arg1;
    return (int)arg0;
}

static uint8_t hooked_button_down(uintptr_t arg0, uintptr_t arg1,
                                  uintptr_t arg2) {
    (void)arg2;
    int input = semantic_argument(arg0, arg1);
    if (input < 0 || input > DT_INPUT_ACTION)
        return 0;
    uint32_t bit = 1u << input;
    if (input == DT_INPUT_FIRE && g_firelog)
        firelog_sample((g_frame_level & bit) != 0);
    if (!(g_down_latch & bit)) {
        /*
         * Fire must be level-driven, not edge-driven: ActionBeginFire sets
         * Desires.WeaponTriggerOn only when CanFire() is true at that exact
         * press, and CanFire() is false during the whole reload. A single
         * consumed edge therefore dies silently when the pull lands during
         * a reload (or the pull predates it), leaving the weapon mute until
         * a lucky re-press — the "gun frozen after reload" bug. Re-asserting
         * the press every frame the trigger is physically held is safe:
         * ActionBeginFire only performs idempotent state writes behind the
         * CanFire() gate, so firing resumes on the first Ready frame.
         */
        if (input == DT_INPUT_FIRE && g_fire_level_down &&
            (g_frame_level & bit)) {
            g_fire_down_delivered = 1;
            return 1;
        }
        return 0;
    }
    g_down_read |= bit;
    if (input == DT_INPUT_FIRE)
        g_fire_down_delivered = 1;
    return 1;
}

static uint8_t hooked_button_up(uintptr_t arg0, uintptr_t arg1,
                                uintptr_t arg2) {
    (void)arg2;
    int input = semantic_argument(arg0, arg1);
    if (input < 0 || input > DT_INPUT_ACTION)
        return 0;
    uint32_t bit = 1u << input;
    if (g_up_latch & bit) {
        g_up_read |= bit;
        return 1;
    }
    /*
     * Fire is the only input whose ButtonUp stops something. The stop path
     * is skipped by the game while InUseMode is active, losing the release
     * for good and leaving the weapon firing forever. Re-asserting the
     * release whenever the trigger is idle is safe: the FireUpDelegate stop
     * is an idempotent state write.
     */
    return input == DT_INPUT_FIRE && g_fire_sticky_up &&
           g_fire_down_delivered && !(g_frame_level & bit);
}

/*
 * The game's ViewSensitivity was tuned for touch swipes and is far too fast
 * for a stick held at full deflection. Scale it down and blend in a
 * quadratic response so small deflections aim finely.
 */
static float look_axis(float value) {
    float magnitude = fabsf(value);
    float shaped =
        value * ((1.0f - g_look_expo) + g_look_expo * magnitude);
    return shaped * g_look_scale;
}

static float hooked_get_axis(uintptr_t arg0, uintptr_t arg1,
                             uintptr_t arg2) {
    (void)arg2;
    int input = semantic_argument(arg0, arg1);
    if (input >= DT_INPUT_MOVE_RIGHT && input <= DT_INPUT_VIEW_UP)
        atomic_store_explicit(&g_gameplay_axis_millis,
                              monotonic_milliseconds(),
                              memory_order_release);
    switch (input) {
        case DT_INPUT_MOVE_RIGHT:
            return g_frame_left_x;
        case DT_INPUT_MOVE_UP:
            /* SDL has +Y down; Unity's vertical game axis has +Y up. */
            return -g_frame_left_y;
        case DT_INPUT_VIEW_RIGHT:
            return look_axis(g_frame_right_x);
        case DT_INPUT_VIEW_UP:
            /* The game treats positive ViewUp as look-down; SDL's raw +Y
             * down already matches it (confirmed on the TV — negating this
             * axis inverted the camera). */
            return look_axis(g_frame_right_y);
        default:
            return 0.0f;
    }
}

/*
 * Ratio of the slider value over its default, so the tuned speeds hold at
 * the default position. The ceiling keeps X below the game's 360°/s view
 * clamp (6°/frame * 2.4 = 14.4°/frame = 360°/s at 25 fps), which is what
 * made the slider appear dead in the original touch-tuned scale.
 */
static float slider_scale(void) {
    if (!g_field_static_get_value || !g_field_sensitivity ||
        !g_field_default_sensitivity)
        return 1.0f;
    float current = 0.0f;
    float fallback = 0.0f;
    g_field_static_get_value(g_field_sensitivity, &current);
    g_field_static_get_value(g_field_default_sensitivity, &fallback);
    if (!(current > 0.0f) || !(fallback > 0.0f))
        return 1.0f;
    float scale = current / fallback;
    if (scale < 0.2f)
        scale = 0.2f;
    if (scale > 2.4f)
        scale = 2.4f;
    return scale;
}

static float hooked_view_sens_x(void) {
    return g_view_sens_x * slider_scale();
}

static float hooked_view_sens_y(void) {
    return g_view_sens_y * slider_scale();
}

/*
 * GuiMogaPopup is the obsolete "MOGA controller connected" accessory popup.
 * In this asset set, Connection_Layout does not contain the widgets expected
 * by Init(), so its exception aborts MFGuiManager.LateUpdate every frame after
 * a mission. NextOS has no MOGA service and uses the semantic SDL bridge
 * below, therefore only the popup's two large entry points are made inert.
 */
static void hooked_noop(void) {
}

/*
 * IL2CPP folds identical method bodies across the whole binary, so tiny
 * getters/updaters share code with unrelated classes — patching one
 * corrupts all of them (SIGILL seen live when 10 GuiMogaPopup methods were
 * patched). Every body address is recorded and duplicates are refused;
 * only large, uniquely-bodied methods get replaced.
 */
static uintptr_t g_patched_bodies[16];
static int g_patched_body_count;

static int body_already_patched(uintptr_t target) {
    for (int index = 0; index < g_patched_body_count; ++index)
        if (g_patched_bodies[index] == target)
            return 1;
    if (g_patched_body_count <
        (int)(sizeof g_patched_bodies / sizeof g_patched_bodies[0]))
        g_patched_bodies[g_patched_body_count++] = target;
    return 0;
}

static void *find_class(dt_module *il2cpp, const char *class_name) {
#define API(type, name)                                                        \
    type name = (type)dt_module_symbol(il2cpp, #name)
    API(il2cpp_domain_get_fn, il2cpp_domain_get);
    API(il2cpp_domain_get_assemblies_fn, il2cpp_domain_get_assemblies);
    API(il2cpp_assembly_get_image_fn, il2cpp_assembly_get_image);
    API(il2cpp_class_from_name_fn, il2cpp_class_from_name);
#undef API

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name)
        return NULL;

    void *domain = il2cpp_domain_get();
    if (!domain)
        return NULL;
    size_t assembly_count = 0;
    const void **assemblies =
        il2cpp_domain_get_assemblies(domain, &assembly_count);
    if (!assemblies)
        return NULL;

    for (size_t index = 0; index < assembly_count; ++index) {
        void *image = il2cpp_assembly_get_image(assemblies[index]);
        void *klass = image
            ? il2cpp_class_from_name(image, "", class_name) : NULL;
        if (klass)
            return klass;
    }
    return NULL;
}

static void *find_method(dt_module *il2cpp, const char *class_name,
                         const char *method_name, int parameter_count) {
    il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name =
        (il2cpp_class_get_method_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_method_from_name");
    void *klass = find_class(il2cpp, class_name);
    if (!il2cpp_class_get_method_from_name || !klass)
        return NULL;
    return (void *)il2cpp_class_get_method_from_name(
        klass, method_name, parameter_count);
}

static void *find_class_ns(dt_module *il2cpp, const char *name_space,
                           const char *class_name) {
#define API(type, name)                                                        \
    type name = (type)dt_module_symbol(il2cpp, #name)
    API(il2cpp_domain_get_fn, il2cpp_domain_get);
    API(il2cpp_domain_get_assemblies_fn, il2cpp_domain_get_assemblies);
    API(il2cpp_assembly_get_image_fn, il2cpp_assembly_get_image);
    API(il2cpp_class_from_name_fn, il2cpp_class_from_name);
#undef API

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name)
        return NULL;

    void *domain = il2cpp_domain_get();
    if (!domain)
        return NULL;
    size_t assembly_count = 0;
    const void **assemblies =
        il2cpp_domain_get_assemblies(domain, &assembly_count);
    if (!assemblies)
        return NULL;

    for (size_t index = 0; index < assembly_count; ++index) {
        void *image = il2cpp_assembly_get_image(assemblies[index]);
        void *klass = image
            ? il2cpp_class_from_name(image, name_space, class_name) : NULL;
        if (klass)
            return klass;
    }
    return NULL;
}

static void *find_method_ns(dt_module *il2cpp, const char *name_space,
                            const char *class_name, const char *method_name,
                            int parameter_count) {
    il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name =
        (il2cpp_class_get_method_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_method_from_name");
    void *klass = find_class_ns(il2cpp, name_space, class_name);
    if (!il2cpp_class_get_method_from_name || !klass)
        return NULL;
    return (void *)il2cpp_class_get_method_from_name(
        klass, method_name, parameter_count);
}

static int replace_method_body(dt_module *il2cpp, const void *method,
                               void *replacement, const char *name) {
#if defined(__aarch64__)
    if (!method || !replacement)
        return 0;
    uintptr_t target = *(const uintptr_t *)method;
    uintptr_t base = (uintptr_t)dt_module_base(il2cpp);
    size_t size = dt_module_size(il2cpp);
    if (target < base || target + 16 > base + size) {
        fprintf(stderr,
                "[gamepad] methodPointer invalido para %s: %p\n",
                name, (void *)target);
        return 0;
    }
    if (body_already_patched(target)) {
        fprintf(stderr,
                "[gamepad] corpo de %s compartilhado (folding) — "
                "patch recusado\n", name);
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 0;
    uintptr_t page =
        target & ~((uintptr_t)page_size - 1u);
    uintptr_t end =
        (target + 16u + (uintptr_t)page_size - 1u) &
        ~((uintptr_t)page_size - 1u);
    size_t span = (size_t)(end - page);
    if (mprotect((void *)page, span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[gamepad] mprotect %s: %s\n",
                name, strerror(errno));
        return 0;
    }

    uint32_t *code = (uint32_t *)target;
    uint32_t old0 = code[0], old1 = code[1];
    code[0] = 0x58000050u; /* ldr x16, #8 */
    code[1] = 0xd61f0200u; /* br x16 */
    memcpy(code + 2, &replacement, sizeof replacement);
    __builtin___clear_cache((char *)target, (char *)target + 16);
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0)
        fprintf(stderr, "[gamepad] aviso: restaurar RX em %s: %s\n",
                name, strerror(errno));
    fprintf(stderr,
            "[gamepad] %s @ il2cpp+0x%lx (%08x %08x) -> %p\n",
            name, (unsigned long)(target - base), old0, old1, replacement);
    return 1;
#else
    (void)il2cpp;
    (void)method;
    (void)replacement;
    (void)name;
    return 0;
#endif
}

void dt_gamepad_try_install(void) {
    if (g_install_state)
        return;
    dt_module *il2cpp = dt_module_find("libil2cpp.so");
    if (!il2cpp)
        return;

    const void *button_down =
        find_method(il2cpp, "PlayerControlsGamepad", "ButtonDown", 1);
    const void *button_up =
        find_method(il2cpp, "PlayerControlsGamepad", "ButtonUp", 1);
    const void *get_axis =
        find_method(il2cpp, "PlayerControlsGamepad", "GetGpadAxis", 1);
    if (!button_down || !button_up || !get_axis) {
        if (++g_install_attempts == 1 || g_install_attempts % 600 == 0)
            fprintf(stderr,
                    "[gamepad] aguardando metadata PlayerControlsGamepad "
                    "(tentativa %u)\n", g_install_attempts);
        return;
    }

    uintptr_t down_body = *(const uintptr_t *)button_down;
    uintptr_t up_body = *(const uintptr_t *)button_up;
    uintptr_t axis_body = *(const uintptr_t *)get_axis;
    if (down_body == up_body || down_body == axis_body ||
        up_body == axis_body) {
        fprintf(stderr,
                "[gamepad] corpos IL2CPP compartilhados inesperadamente; "
                "instalacao recusada\n");
        g_install_state = -1;
        return;
    }

    if (!replace_method_body(il2cpp, button_down,
                             (void *)&hooked_button_down, "ButtonDown") ||
        !replace_method_body(il2cpp, button_up,
                             (void *)&hooked_button_up, "ButtonUp") ||
        !replace_method_body(il2cpp, get_axis,
                             (void *)&hooked_get_axis, "GetGpadAxis")) {
        fprintf(stderr, "[gamepad] falha ao instalar ponte semantica\n");
        g_install_state = -1;
        return;
    }
    /*
     * Camera speed: overriding the sensitivity getters detaches the pad
     * camera from the game's touch-sensitivity option (see comment at
     * g_view_sens_x). Non-fatal if a build lacks them.
     */
    const void *sens_x =
        find_method(il2cpp, "PlayerControlsGamepad",
                    "get_ViewSensitivityX", 0);
    const void *sens_y =
        find_method(il2cpp, "PlayerControlsGamepad",
                    "get_ViewSensitivityY", 0);
    if (sens_x && sens_y) {
        if (!replace_method_body(il2cpp, sens_x,
                                 (void *)&hooked_view_sens_x,
                                 "get_ViewSensitivityX") ||
            !replace_method_body(il2cpp, sens_y,
                                 (void *)&hooked_view_sens_y,
                                 "get_ViewSensitivityY"))
            fprintf(stderr,
                    "[gamepad] aviso: sensibilidade da camera segue a "
                    "opcao de toque do jogo\n");
    } else {
        fprintf(stderr,
                "[gamepad] aviso: getters de sensibilidade ausentes na "
                "metadata\n");
    }

    /*
     * Reconnect the options slider to the hooked getters (see
     * slider_scale). Non-fatal: without the fields the camera simply
     * keeps the fixed tuned speed.
     */
    il2cpp_class_get_field_from_name_fn class_get_field =
        (il2cpp_class_get_field_from_name_fn)
        dt_module_symbol(il2cpp, "il2cpp_class_get_field_from_name");
    g_field_static_get_value = (il2cpp_field_static_get_value_fn)
        dt_module_symbol(il2cpp, "il2cpp_field_static_get_value");
    void *gui_options = find_class(il2cpp, "GuiOptions");
    if (class_get_field && g_field_static_get_value && gui_options) {
        g_field_sensitivity =
            class_get_field(gui_options, "sensitivity");
        g_field_default_sensitivity =
            class_get_field(gui_options, "DefaultSensitivity");
    }
    if (g_field_sensitivity && g_field_default_sensitivity)
        fprintf(stderr,
                "[gamepad] slider de sensibilidade conectado "
                "(GuiOptions.sensitivity)\n");
    else
        fprintf(stderr,
                "[gamepad] aviso: GuiOptions.sensitivity ausente — "
                "sensibilidade fixa\n");

    static const struct {
        const char *method;
        int parameters;
    } moga_hooks[] = {
        { "Init", 0 },
        { "Show", 2 },
    };
    int moga_neutered = 0;
    for (size_t entry = 0;
         entry < sizeof moga_hooks / sizeof moga_hooks[0]; ++entry) {
        const void *method =
            find_method(il2cpp, "GuiMogaPopup", moga_hooks[entry].method,
                        moga_hooks[entry].parameters);
        if (method &&
            replace_method_body(il2cpp, method, (void *)&hooked_noop,
                                moga_hooks[entry].method))
            ++moga_neutered;
    }
    fprintf(stderr,
            "[gamepad] GuiMogaPopup neutralizado (%d/2 metodos)\n",
            moga_neutered);

    busy_guard_install(il2cpp);
    speed_probe_install(il2cpp);
    firelog_install(il2cpp);
    visfix_install(il2cpp);
    attacklog_install(il2cpp);

    g_install_state = 1;
    fprintf(stderr,
            "[gamepad] ponte SDL -> PlayerControlsGamepad instalada por "
            "metadata; fluxo Update original preservado\n");
}

int dt_gamepad_is_installed(void) {
    return g_install_state == 1;
}

int dt_gamepad_gameplay_active(void) {
    uint64_t last = atomic_load_explicit(&g_gameplay_axis_millis,
                                         memory_order_acquire);
    uint64_t now = monotonic_milliseconds();
    return last && now >= last && now - last <= 300u;
}
