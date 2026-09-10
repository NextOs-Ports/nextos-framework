/* FP2-CONTROLS-LIVE — casos dirigidos do runtime vivo NEXTOSCONTROLLERS/3.
 *
 * Prova, no host e sem SDL, que o arquivo do dono GOVERNA o consumidor:
 * pad -> GPTK decide -> runtime vivo (ACK) -> sink real.  Só o glue puro do
 * port + o nxinput 0.10.1 vendorizado.
 *
 * argv[1] = diretório de trabalho; argv[2] = default gerado
 * (generated/fp2/defaults/NEXTOSCONTROLLERS.gptk).
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "input_gptk.h"
#include "nxinput_gptk.h"

static int failures;
#define CHECK(cond, name) do { \
    if (cond) { printf("ok    %s\n", (name)); } \
    else { printf("FALHA %s (%s:%d)\n", (name), __FILE__, __LINE__); failures++; } \
} while (0)

/* Sinks de teste: contam press/release por keycode e vetores. */
static int presses[256], releases[256];
static int vectors;
static float last_x, last_y;
static int fail_ack;

static int sink_key(void *user, const char *action, int pressed, float value)
{
    (void)action; (void)value;
    int k = (int)(intptr_t)user;
    if (fail_ack)
        return -1;
    if (pressed) presses[k]++; else releases[k]++;
    return 0;
}

static int sink_motion(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    vectors++; last_x = x; last_y = y;
    return 0;
}

static void reset_counts(void)
{
    memset(presses, 0, sizeof presses);
    memset(releases, 0, sizeof releases);
    vectors = 0;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static int register_all(void)
{
    struct { const char *a; int k; } b[] = {
        {"fp2.jump",97},{"fp2.attack",96},{"fp2.special",99},{"fp2.guard",100},
        {"fp2.pause",107},{"fp2.confirm",97},{"fp2.cancel",96},
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (fp2_gptk_register_button(b[i].a, "adapter.input.android-keyevent",
                                     sink_key, (void *)(intptr_t)b[i].k) != 0)
            return -1;
    if (fp2_gptk_register_vector("fp2.move", "adapter.input.android-motion",
                                 sink_motion, NULL) != 0)
        return -1;
    return 0;
}

/* Cada cenário roda num processo filho: o glue tem estado estático único. */
typedef void (*scenario_fn)(const char *gamedir);

static void run_scenario(const char *name, scenario_fn fn, const char *gamedir)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fn(gamedir);
        exit(failures ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s cenário %s\n", ok ? "PASS " : "FAIL ", name);
    if (!ok) failures++;
}

static const char *gamedir_default;

static void prepare(const char *gamedir, const char *owner_text)
{
    char path[512];
    mkdir(gamedir, 0700);
    snprintf(path, sizeof path, "%s/defaults", gamedir);
    mkdir(path, 0700);
    size_t n = 0;
    char *def = read_file(gamedir_default, &n);
    snprintf(path, sizeof path, "%s/defaults/NEXTOSCONTROLLERS.gptk", gamedir);
    write_file(path, def);
    free(def);
    snprintf(path, sizeof path, "%s/NEXTOSCONTROLLERS.gptk", gamedir);
    if (owner_text) write_file(path, owner_text); else unlink(path);
}

/* 1. default: A pula no gameplay (uma press, uma release); menu A confirma. */
static void sc_default(const char *gamedir)
{
    prepare(gamedir, NULL);
    CHECK(fp2_gptk_preinit(gamedir) == 0 && fp2_gptk_loaded(), "default carregado");
    CHECK(fp2_gptk_schema() == 3, "schema V3");
    CHECK(register_all() == 0 && fp2_gptk_seal() == 0, "sinks registrados e selados");
    CHECK(fp2_gptk_context() == -1, "contexto nasce UNPROVEN");
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f) == FP2_GPTK_LIVE_PASSTHROUGH,
          "sem contexto: PASSTHROUGH");
    CHECK(!fp2_gptk_should_consume(NXINPUT_GPTK_A), "sem contexto: não consome");
    fp2_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    CHECK(fp2_gptk_context() == FP2_GPTK_CONTEXT_GAMEPLAY, "contexto gameplay provado");
    reset_counts();
    /* layout nativo do FP2: B = Jump (JoystickButton1 = 97), A = Attack (96),
     * X = Special (99), Y = Guard (100) — lidos de InputControl.mKeysList. */
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_B, 1, 1.0f) == FP2_GPTK_LIVE_DELIVERED, "B press DELIVERED");
    fp2_gptk_feed_button(NXINPUT_GPTK_B, 1, 1.0f);   /* amostra repetida */
    CHECK(presses[97] == 1, "jump (B) -> exatamente uma press (97 = JoystickButton1, o Jump nativo)");
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_B, 0, 0.0f) == FP2_GPTK_LIVE_DELIVERED, "B release DELIVERED");
    CHECK(releases[97] == 1, "jump: exatamente uma release");
    CHECK(fp2_gptk_should_consume(NXINPUT_GPTK_B), "B consumido (nativo suprimido)");
    CHECK(!fp2_gptk_should_consume(NXINPUT_GPTK_L1), "L1 native: não consumido");
    reset_counts();
    fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f); fp2_gptk_feed_button(NXINPUT_GPTK_A, 0, 0);
    fp2_gptk_feed_button(NXINPUT_GPTK_X, 1, 1.0f); fp2_gptk_feed_button(NXINPUT_GPTK_X, 0, 0);
    fp2_gptk_feed_button(NXINPUT_GPTK_Y, 1, 1.0f); fp2_gptk_feed_button(NXINPUT_GPTK_Y, 0, 0);
    CHECK(presses[96] == 1 && presses[99] == 1 && presses[100] == 1, "attack/special/guard -> 96/99/100 (A/X/Y nativos do FP2)");
    /* vetores */
    CHECK(fp2_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK), "LEFT_STICK consumido no gameplay");
    CHECK(fp2_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 0.5f, -0.25f) == FP2_GPTK_LIVE_DELIVERED, "fp2.move DELIVERED");
    CHECK(vectors == 1 && last_x == 0.5f && last_y == -0.25f, "fp2.move recebe o vetor");
    CHECK(!fp2_gptk_should_consume(NXINPUT_GPTK_RIGHT_STICK), "RIGHT_STICK native");
    /* menu */
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_MENU, "stage:no-player");
    reset_counts();
    fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f); fp2_gptk_feed_button(NXINPUT_GPTK_A, 0, 0);
    CHECK(presses[97] == 1 && releases[97] == 1, "menu: A -> fp2.confirm -> 97 (Jump = confirmar nativo)");
    CHECK(fp2_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, 1.0f, 0.0f) == FP2_GPTK_LIVE_PASSTHROUGH, "menu: LEFT_STICK PASSTHROUGH");
    CHECK(!fp2_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK), "menu: LEFT_STICK não consumido");
    CHECK(fp2_gptk_decision(NXINPUT_GPTK_L2, NULL) == FP2_GPTK_DECIDE_NATIVE, "L2 native no menu");
}

/* 2. owner: A=null, R2=fp2.jump. */
static void sc_owner_remap(const char *gamedir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/defaults/NEXTOSCONTROLLERS.gptk", gamedir_default);
    size_t n = 0;
    char *def = read_file(gamedir_default, &n);
    /* substituição textual nas seções (A = fp2.attack -> A = null; R2 = native -> R2 = fp2.attack só no gameplay) */
    char *gp = strstr(def, "[gameplay]");
    char *a = strstr(gp, "A = fp2.attack");
    char *r2 = strstr(gp, "R2 = native");
    CHECK(a && r2, "default tem A/R2 no gameplay");
    char owner[8192]; owner[0] = '\0';
    /* reconstrói o texto com as duas linhas trocadas */
    size_t pos = 0;
    for (char *line = strtok(def, "\n"); line; line = strtok(NULL, "\n")) {
        const char *out = line;
        static int in_gp;
        if (line[0] == '[') in_gp = strcmp(line, "[gameplay]") == 0;
        if (in_gp && strcmp(line, "A = fp2.attack") == 0) out = "A = null";
        if (in_gp && strcmp(line, "R2 = native") == 0) out = "R2 = fp2.attack";
        pos += (size_t)snprintf(owner + pos, sizeof owner - pos, "%s\n", out);
    }
    free(def);
    prepare(gamedir, owner);
    CHECK(fp2_gptk_preinit(gamedir) == 0 && fp2_gptk_loaded(), "owner carregado");
    CHECK(strcmp(fp2_gptk_source_name(), "owner") == 0, "fonte = owner");
    CHECK(register_all() == 0 && fp2_gptk_seal() == 0, "selado");
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    reset_counts();
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f) == FP2_GPTK_LIVE_SUPPRESSED, "A=null: SUPPRESSED");
    CHECK(fp2_gptk_should_consume(NXINPUT_GPTK_A), "A=null consumido (nativo fechado)");
    fp2_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
    CHECK(presses[96] == 0, "A=null: nenhum sink");
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_R2, 1, 0.9f) == FP2_GPTK_LIVE_DELIVERED, "R2 -> fp2.attack DELIVERED");
    fp2_gptk_feed_button(NXINPUT_GPTK_R2, 0, 0.0f);
    CHECK(presses[96] == 1 && releases[96] == 1, "R2 entrega o ataque (96) uma vez");
    /* evidência */
    char rec[512]; snprintf(rec, sizeof rec, "%s/receipt.jsonl", gamedir);
    size_t rn = 0; char *r = read_file(rec, &rn);
    CHECK(r && strstr(r, "nxinput-gptk-event-evidence/1")&&
          strstr(r, "\"action\":\"fp2.attack\"") && strstr(r, "\"sink\":\"adapter.input.android-keyevent\"") &&
          strstr(r, "\"kind\":\"runtime\"") && strstr(r, "nxinput-gptk-runtime/3"), "evidência JSONL escrita");
    free(r);
}

/* 3. START segurado através de gameplay -> menu: release na troca, sem 2ª press. */
static void sc_start_latch(const char *gamedir)
{
    prepare(gamedir, NULL);
    fp2_gptk_preinit(gamedir); register_all(); fp2_gptk_seal();
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    reset_counts();
    fp2_gptk_feed_button(NXINPUT_GPTK_START, 1, 1.0f);
    CHECK(presses[107] == 1, "START press -> 107 (Pause nativo = Joystick Button 10)");
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_MENU, "stage:paused");
    CHECK(releases[107] == 1, "troca de contexto solta START");
    for (int i = 0; i < 32; i++) fp2_gptk_feed_button(NXINPUT_GPTK_START, 1, 1.0f);
    CHECK(presses[107] == 1, "START ainda segurado: nenhuma 2ª press (quarentena)");
    fp2_gptk_feed_button(NXINPUT_GPTK_START, 0, 0.0f);
    CHECK(releases[107] == 1, "soltura física após quarentena: nada a mais");
    fp2_gptk_feed_button(NXINPUT_GPTK_START, 1, 1.0f);
    CHECK(presses[107] == 2, "nova pressão física: nova press");
    fp2_gptk_feed_button(NXINPUT_GPTK_START, 0, 0.0f);
    fp2_gptk_clear_context("scene:loading-or-unknown");
    CHECK(fp2_gptk_context() == -1 && !fp2_gptk_should_consume(NXINPUT_GPTK_A), "clear -> passthrough");
}

/* 4. sink faltando: selo recusado, nada consumido. */
static void sc_missing_sink(const char *gamedir)
{
    prepare(gamedir, NULL);
    fp2_gptk_preinit(gamedir);
    fp2_gptk_register_button("fp2.jump", "adapter.input.android-keyevent", sink_key, (void *)(intptr_t)96);
    CHECK(fp2_gptk_seal() != 0, "selo recusado sem cobertura total");
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    CHECK(fp2_gptk_context() == -1, "sem selo: contexto não prova");
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f) == FP2_GPTK_LIVE_PASSTHROUGH, "sem selo: PASSTHROUGH");
    CHECK(!fp2_gptk_should_consume(NXINPUT_GPTK_A), "sem selo: não consome");
}

/* 5. ACK falho: FATAL, runtime invalidado, nada reproduzido. */
static void sc_fatal(const char *gamedir)
{
    prepare(gamedir, NULL);
    fp2_gptk_preinit(gamedir); register_all(); fp2_gptk_seal();
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    fail_ack = 1;
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f) == FP2_GPTK_LIVE_FATAL, "ACK falho -> FATAL");
    CHECK(fp2_gptk_fatal(), "fatal registrado");
    fail_ack = 0;
    fp2_gptk_feed_button(NXINPUT_GPTK_A, 0, 0.0f);
    CHECK(fp2_gptk_feed_button(NXINPUT_GPTK_B, 1, 1.0f) == FP2_GPTK_LIVE_PASSTHROUGH, "após FATAL: PASSTHROUGH");
    CHECK(!fp2_gptk_should_consume(NXINPUT_GPTK_B), "após FATAL: não consome (nunca replay)");
}

/* 6. owner inválido: preservado byte a byte, default selecionado. */
static void sc_invalid_owner(const char *gamedir)
{
    const char *bad = "format = NEXTOS_CONTROLLERS/3\nport = fp2\nFACE_LAYOUT = auto\n[menu]\nA = fp2.confirm\n304 = fp2.jump\n[gameplay]\nA = fp2.attack\n";
    prepare(gamedir, bad);
    CHECK(fp2_gptk_preinit(gamedir) == 0 && fp2_gptk_loaded(), "default selecionado");
    CHECK(strcmp(fp2_gptk_source_name(), "default_owner_rejected") == 0, "fonte = default_owner_rejected");
    char path[512]; snprintf(path, sizeof path, "%s/NEXTOSCONTROLLERS.gptk", gamedir);
    size_t n = 0; char *after = read_file(path, &n);
    CHECK(after && strcmp(after, bad) == 0, "owner inválido preservado byte a byte");
    free(after);
    CHECK(register_all() == 0 && fp2_gptk_seal() == 0, "default selado");
    fp2_gptk_set_context(FP2_GPTK_CONTEXT_GAMEPLAY, "stage:player-alive");
    reset_counts();
    fp2_gptk_feed_button(NXINPUT_GPTK_A, 1, 1.0f);
    CHECK(presses[96] == 1, "default governa após owner rejeitado");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "uso: %s WORKDIR DEFAULT_GPTK\n", argv[0]); return 2; }
    gamedir_default = argv[2];
    char dir[512];
    for (int i = 0; i < 6; i++) {
        snprintf(dir, sizeof dir, "%s/g%d", argv[1], i);
        mkdir(dir, 0700);
        char rec[600]; snprintf(rec, sizeof rec, "%s/receipt.jsonl", dir);
        setenv("NXGPTK_RECEIPT", rec, 1);
        switch (i) {
        case 0: run_scenario("default", sc_default, dir); break;
        case 1: run_scenario("owner-remap-null", sc_owner_remap, dir); break;
        case 2: run_scenario("start-latch", sc_start_latch, dir); break;
        case 3: run_scenario("missing-sink", sc_missing_sink, dir); break;
        case 4: run_scenario("fatal-ack", sc_fatal, dir); break;
        case 5: run_scenario("invalid-owner", sc_invalid_owner, dir); break;
        }
    }
    printf("%s (%d falhas)\n", failures ? "GPTK-LIVE: RED" : "GPTK-LIVE: GREEN", failures);
    return failures ? 1 : 0;
}
