/*
 * Port-owned sink adapter for nxinput 0.5.1's GPTK parser/dispatcher.
 *
 * Physical identity remains SDL/PortMaster's job. This layer reads the owner
 * mapping without following symlinks, validates every semantic action against
 * Sally Face's real Android input sinks, then makes the dispatcher the sole
 * authority for mapped buttons. Sticks mapped to sf.move take one explicit
 * semantic path into the game's single MotionEvent; they never also enter as
 * a raw stick sample.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gptk_adapter.h"

typedef struct {
    const char *action;
    int keycode;
} sf_gptk_action_route;

static const sf_gptk_action_route action_routes[] = {
    { "sf.interact", 96 },
    { "sf.back", 97 },
    { "sf.gasmask", 99 },
    { "sf.inventory", 100 },
    { "sf.pause", 105 },
};

static const char *const allowed_actions[] = {
    "sf.interact", "sf.back", "sf.gasmask", "sf.inventory", "sf.pause",
    "sf.move",
};

static const char builtin_default[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "port = sallyface\n"
    "\n"
    "[menu]\n"
    "A = sf.interact\n"
    "B = sf.back\n"
    "START = sf.pause\n"
    "LEFT_STICK = sf.move\n"
    "\n"
    "[gameplay]\n"
    "A = sf.interact\n"
    "B = sf.back\n"
    "X = sf.gasmask\n"
    "Y = sf.inventory\n"
    "START = sf.pause\n"
    "LEFT_STICK = sf.move\n";

static nxinput_gptk mapping;
static nxinput_gptk_dispatcher dispatcher;
static sf_gptk_button_delivery_fn deliver_button;
static void *delivery_user;
static int initialized;
static int active_control = -1;
static int proof_enabled;
static unsigned proof_lines;
static unsigned long owned_edges;
static unsigned long semantic_edges;
static unsigned long semantic_stick_samples;
static unsigned long native_button_samples;
static unsigned long native_stick_samples;
static unsigned long raw_duplicate_edges;
static unsigned long receipt_sequence;
static char loaded_source[16] = "none";

static const char *control_name(int control)
{
    static const char *const names[NXINPUT_GPTK_CONTROL_COUNT] = {
        "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
        "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT", "LEFT_STICK",
        "RIGHT_STICK",
    };
    return control >= 0 && control < (int)NXINPUT_GPTK_CONTROL_COUNT
               ? names[control] : "UNKNOWN";
}

static int action_keycode(const char *action)
{
    size_t i;
    for (i = 0; i < sizeof action_routes / sizeof action_routes[0]; i++)
        if (strcmp(action_routes[i].action, action) == 0)
            return action_routes[i].keycode;
    return 0;
}

static void action_sink(void *user, const char *action, int pressed,
                        float value)
{
    (void)user;
    (void)value;
    int keycode = action_keycode(action);
    if (!keycode || !deliver_button)
        return;
    semantic_edges++;
    if (proof_enabled && proof_lines < 32) {
        proof_lines++;
        fprintf(stderr,
                "GPTK-ACTION-RECEIPT {\"schema\":\"sf-gptk-action-v1\","
                "\"physical\":\"%s\",\"action\":\"%s\","
                "\"pressed\":%s,\"sink\":\"android-keycode-%d\","
                "\"delivery\":%lu}\n",
                control_name(active_control), action,
                pressed ? "true" : "false", keycode, semantic_edges);
    }
    deliver_button(delivery_user, keycode, pressed, active_control);
}

static int validate_semantic_shape(const nxinput_gptk *map, char *error,
                                   size_t error_size)
{
    int context;
    int control;
    if (map->port[0] && strcmp(map->port, "sallyface") != 0) {
        snprintf(error, error_size, "NXI1001: mapping belongs to another port");
        return NXINPUT_GPTK_ERR_UNKNOWN_NAME;
    }
    for (context = 0; context < (int)NXINPUT_GPTK_CONTEXT_COUNT; context++) {
        for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT;
             control++) {
            const char *action = nxinput_gptk_action(
                map, (nxinput_gptk_context)context, control);
            if (!action)
                continue;
            if (control == (int)NXINPUT_GPTK_LEFT_STICK ||
                control == (int)NXINPUT_GPTK_RIGHT_STICK) {
                if (strcmp(action, "sf.move") != 0) {
                    snprintf(error, error_size,
                             "NXI1002: stick requires sf.move vector sink");
                    return NXINPUT_GPTK_ERR_MALFORMED;
                }
            } else if (control == (int)NXINPUT_GPTK_L2 ||
                       control == (int)NXINPUT_GPTK_R2) {
                snprintf(error, error_size,
                         "NXI1002: Sally Face has no GPTK trigger sink");
                return NXINPUT_GPTK_ERR_MALFORMED;
            } else if (strcmp(action, "sf.move") == 0) {
                snprintf(error, error_size,
                         "NXI1002: sf.move requires a physical stick");
                return NXINPUT_GPTK_ERR_MALFORMED;
            }
        }
    }
    return 0;
}

static int install_mapping(const char *text, size_t length,
                           sf_gptk_button_delivery_fn delivery, void *user,
                           const char *source_label, char *error,
                           size_t error_size)
{
    nxinput_gptk parsed;
    int result = nxinput_gptk_parse(text, length, &parsed, error, error_size);
    if (result != 0)
        return result;
    result = nxinput_gptk_validate_actions(
        &parsed, allowed_actions,
        sizeof allowed_actions / sizeof allowed_actions[0], error, error_size);
    if (result != 0)
        return result;
    result = validate_semantic_shape(&parsed, error, error_size);
    if (result != 0)
        return result;

    mapping = parsed;
    deliver_button = delivery;
    delivery_user = user;
    nxinput_gptk_dispatcher_init(&dispatcher, &mapping);
    nxinput_gptk_dispatcher_set_context(
        &dispatcher, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
    size_t i;
    for (i = 0; i < sizeof action_routes / sizeof action_routes[0]; i++) {
        if (nxinput_gptk_dispatcher_register(
                &dispatcher, action_routes[i].action, action_sink, NULL) != 0) {
            snprintf(error, error_size,
                     "NXI1002: adapter sink table is incomplete");
            memset(&mapping, 0, sizeof mapping);
            memset(&dispatcher, 0, sizeof dispatcher);
            return NXINPUT_GPTK_ERR_MALFORMED;
        }
    }
    initialized = 1;
    snprintf(loaded_source, sizeof loaded_source, "%s",
             source_label && *source_label ? source_label : "memory");
    return 0;
}

int sf_gptk_init_text(const char *text, size_t length,
                      sf_gptk_button_delivery_fn delivery, void *user,
                      const char *source_label)
{
    char error[192] = { 0 };
    proof_lines = 0;
    owned_edges = semantic_edges = semantic_stick_samples = 0;
    native_button_samples = native_stick_samples = raw_duplicate_edges = 0;
    receipt_sequence = 0;
    initialized = 0;
    memset(&mapping, 0, sizeof mapping);
    memset(&dispatcher, 0, sizeof dispatcher);
    deliver_button = NULL;
    delivery_user = NULL;
    int result = install_mapping(text, length, delivery, user, source_label,
                                 error, sizeof error);
    if (result != 0)
        fprintf(stderr, "[sf/gptk] %s\n", error[0] ? error : "invalid map");
    return result == 0 ? 0 : -1;
}

static int read_owner_file(const char *game_dir, char **text, size_t *length)
{
    int directory = -1;
    int file = -1;
    char *buffer = NULL;
    struct stat info;
    int result = -1;

    if (!game_dir || !*game_dir || !text || !length)
        return -1;
    directory = open(game_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0)
        goto out;
    file = openat(directory, "NEXTOSCONTROLLERS.gptk",
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0 || fstat(file, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 ||
        (unsigned long long)info.st_size > NXINPUT_GPTK_MAX_BYTES)
        goto out;
    buffer = malloc((size_t)info.st_size + 1u);
    if (!buffer)
        goto out;
    size_t used = 0;
    while (used < (size_t)info.st_size) {
        ssize_t got = read(file, buffer + used, (size_t)info.st_size - used);
        if (got > 0) {
            used += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        goto out;
    }
    buffer[used] = '\0';
    *text = buffer;
    *length = used;
    buffer = NULL;
    result = 0;
out:
    free(buffer);
    if (file >= 0)
        close(file);
    if (directory >= 0)
        close(directory);
    return result;
}

int sf_gptk_init(const char *game_dir, sf_gptk_button_delivery_fn delivery,
                 void *user)
{
    char *owner = NULL;
    size_t owner_length = 0;
    char error[192] = { 0 };
    int owner_result = NXINPUT_GPTK_ERR_MALFORMED;
    const char *fallback = "unreadable";

    proof_enabled = getenv("SF_GPTK_PROOF") &&
                    strcmp(getenv("SF_GPTK_PROOF"), "0") != 0;
    proof_lines = 0;
    owned_edges = semantic_edges = semantic_stick_samples = 0;
    native_button_samples = native_stick_samples = raw_duplicate_edges = 0;
    receipt_sequence = 0;

    if (read_owner_file(game_dir, &owner, &owner_length) == 0) {
        owner_result = install_mapping(owner, owner_length, delivery, user,
                                       "owner", error, sizeof error);
        fallback = error[0] ? error : "invalid";
    }
    free(owner);

    if (owner_result != 0) {
        char default_error[192] = { 0 };
        int default_result = install_mapping(
            builtin_default, sizeof builtin_default - 1u, delivery, user,
            "default", default_error, sizeof default_error);
        if (default_result != 0) {
            fprintf(stderr, "[sf/gptk] built-in default rejected: %s\n",
                    default_error);
            initialized = 0;
            return -1;
        }
        fprintf(stderr,
                "GPTK-RECEIPT {\"schema\":\"sf-gptk-load-v1\","
                "\"source\":\"default\",\"context\":\"gameplay\","
                "\"fallback\":\"%.120s\",\"single_authority\":true}\n",
                fallback);
    } else {
        fprintf(stderr,
                "GPTK-RECEIPT {\"schema\":\"sf-gptk-load-v1\","
                "\"source\":\"owner\",\"context\":\"gameplay\","
                "\"single_authority\":true}\n");
    }
    return 0;
}

int sf_gptk_control_owned(int control)
{
    return initialized &&
           nxinput_gptk_action(&mapping, dispatcher.context, control) != NULL;
}

const char *sf_gptk_control_action(int control)
{
    return initialized
               ? nxinput_gptk_action(&mapping, dispatcher.context, control)
               : NULL;
}

void sf_gptk_feed_button(int control, int pressed, float value)
{
    if (!sf_gptk_control_owned(control))
        return;
    owned_edges++;
    active_control = control;
    nxinput_gptk_dispatcher_feed(&dispatcher, control, pressed, value);
    active_control = -1;
}

int sf_gptk_route_stick(int control, float x, float y,
                        float *move_x, float *move_y)
{
    const char *action = sf_gptk_control_action(control);
    if (!action)
        return 0;
    if (strcmp(action, "sf.move") == 0) {
        if (move_x)
            *move_x = x;
        if (move_y)
            *move_y = y;
        semantic_stick_samples++;
        if (proof_enabled && proof_lines < 32 &&
            (x > 0.2f || x < -0.2f || y > 0.2f || y < -0.2f)) {
            proof_lines++;
            fprintf(stderr,
                    "GPTK-ACTION-RECEIPT {\"schema\":\"sf-gptk-action-v1\","
                    "\"physical\":\"%s\",\"action\":\"sf.move\","
                    "\"sink\":\"android-motion-left-stick\","
                    "\"x\":%.3f,\"y\":%.3f,\"delivery\":%lu}\n",
                    control_name(control), x, y, semantic_stick_samples);
        }
    }
    return 1;
}

void sf_gptk_note_native_button_delivery(int control)
{
    native_button_samples++;
    if (sf_gptk_control_owned(control))
        raw_duplicate_edges++;
}

void sf_gptk_note_native_stick_delivery(int control)
{
    native_stick_samples++;
    if (sf_gptk_control_owned(control))
        raw_duplicate_edges++;
}

void sf_gptk_periodic_receipt(unsigned long frame)
{
    if (!initialized || (frame != 600 && frame != 1200 && frame != 2400))
        return;
    receipt_sequence++;
    fprintf(stderr,
            "GPTK-INPUT-RECEIPT {\"schema\":\"sf-gptk-input-v1\","
            "\"sequence\":%lu,\"source\":\"%s\","
            "\"owned_edges\":%lu,\"semantic_edges\":%lu,"
            "\"semantic_stick_samples\":%lu,"
            "\"native_button_samples\":%lu,"
            "\"native_stick_samples\":%lu,"
            "\"raw_duplicate_edges\":%lu}\n",
            receipt_sequence, loaded_source, owned_edges, semantic_edges,
            semantic_stick_samples, native_button_samples,
            native_stick_samples, raw_duplicate_edges);
}

unsigned long sf_gptk_raw_duplicate_count(void)
{
    return raw_duplicate_edges;
}

void sf_gptk_close(void)
{
    if (!initialized)
        return;
    fprintf(stderr,
            "GPTK-INPUT-FINAL {\"schema\":\"sf-gptk-input-v1\","
            "\"source\":\"%s\",\"owned_edges\":%lu,"
            "\"semantic_edges\":%lu,\"semantic_stick_samples\":%lu,"
            "\"raw_duplicate_edges\":%lu}\n",
            loaded_source, owned_edges, semantic_edges,
            semantic_stick_samples, raw_duplicate_edges);
    initialized = 0;
    memset(&mapping, 0, sizeof mapping);
    memset(&dispatcher, 0, sizeof dispatcher);
    deliver_button = NULL;
    delivery_user = NULL;
}
