/* Produtor do health receipt org.nextos.nxruntime.health/1 (nxbootstrap 0.6.32).
 *
 * O launcher só promove pending→active depois de o RUNTIME gravar, no caminho
 * privado NXBOOTSTRAP_HEALTH_FILE, a linha JSON exata ligada a ESTA execução e
 * a ESTA geração. Log, PID vivo, splash e exit 0 nunca contam como saúde — o
 * receipt só é escrito depois do primeiro present com drawable real, que é a
 * primeira prova de que a geração chegou ao jogo de verdade.
 *
 * Contrato do produtor: temporário exclusivo com umask 077 no MESMO diretório,
 * uma linha só, rename atômico sobre o destino.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void nx_log(const char *fmt, ...);

static int st_health_field_ok(const char *value)
{
    const char *p;
    if (!value || !value[0] || strlen(value) > 160)
        return 0;
    for (p = value; *p; p++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
            return 0;
    }
    return 1;
}

void st_health_receipt_publish(void)
{
    static int done;
    const char *path = getenv("NXBOOTSTRAP_HEALTH_FILE");
    const char *run = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
    const char *generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
    const char *port = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
    char tmp[4104];
    mode_t previous_umask;
    FILE *out;

    if (done)
        return;
    done = 1;
    if (!path || !path[0] || path[0] != '/' || strlen(path) > 4000)
        return;
    if (!st_health_field_ok(run) || !st_health_field_ok(generation) ||
        !st_health_field_ok(port))
        return;

    (void)snprintf(tmp, sizeof(tmp), "%s.st%ld", path, (long)getpid());
    previous_umask = umask(077);
    out = fopen(tmp, "wx");
    umask(previous_umask);
    if (!out)
        return;
    fprintf(out,
            "{\"schema\":\"org.nextos.nxruntime.health\",\"schema_version\":1,"
            "\"run_id\":\"%s\",\"generation\":\"%s\",\"port_id\":\"%s\","
            "\"status\":\"ready\"}\n",
            run, generation, port);
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
        fclose(out);
        unlink(tmp);
        return;
    }
    fclose(out);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return;
    }
    nx_log("health receipt published run=%s generation=%s", run, generation);
}
