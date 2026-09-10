/* SPDX-License-Identifier: GPL-3.0-only */
/* Receipt de saúde run-bound do nxbootstrap (org.nextos.nxruntime.health/1).
 *
 * O launcher gerado (>=0.7.5) só promove a geração pendente quando o PRÓPRIO
 * jogo publica este receipt; sem ele, cada execução — mesmo perfeita — conta
 * como falha pré-health e a terceira recusa o boot (NXU0009, visto em campo
 * no dArkOS .137 em 30/08/2026 com o 1.2.0).  Publicar depois de frames REAIS
 * apresentados fecha o ciclo: primeira execução promove, e todo boot seguinte
 * pega o caminho rápido saudável do launcher, sem contadores.
 *
 * Contrato (validado byte a byte pelo launcher): UMA linha exata em
 * $NXBOOTSTRAP_HEALTH_FILE, modo 0600, dono do processo, 1 hardlink, rename
 * atômico.  Fora do launcher (envs ausentes) é no-op silencioso.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void st_health_publish_once(void)
{
    static int attempted;
    if (attempted)
        return;
    attempted = 1;

    const char *file = getenv("NXBOOTSTRAP_HEALTH_FILE");
    const char *schema = getenv("NXBOOTSTRAP_HEALTH_SCHEMA");
    const char *version = getenv("NXBOOTSTRAP_HEALTH_SCHEMA_VERSION");
    const char *run_id = getenv("NXBOOTSTRAP_HEALTH_RUN_ID");
    const char *generation = getenv("NXBOOTSTRAP_HEALTH_GENERATION");
    const char *port_id = getenv("NXBOOTSTRAP_HEALTH_PORT_ID");
    if (!file || !*file || !schema || !*schema || !version || !*version ||
        !run_id || !*run_id || !generation || !*generation ||
        !port_id || !*port_id)
        return;

    char line[640];
    int length = snprintf(line, sizeof line,
                          "{\"schema\":\"%s\",\"schema_version\":%s,"
                          "\"run_id\":\"%s\",\"generation\":\"%s\","
                          "\"port_id\":\"%s\",\"status\":\"ready\"}\n",
                          schema, version, run_id, generation, port_id);
    if (length <= 0 || (size_t)length >= sizeof line)
        return;

    char tmp[576];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%d", file, (int)getpid()) >=
        (int)sizeof tmp)
        return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    if (fchmod(fd, 0600) != 0 ||
        write(fd, line, (size_t)length) != (ssize_t)length ||
        fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return;
    }
    close(fd);
    if (rename(tmp, file) != 0) {
        unlink(tmp);
        return;
    }
    fprintf(stderr,
            "[st/health] receipt ready publicado (generation=%.16s...)\n",
            generation);
}
