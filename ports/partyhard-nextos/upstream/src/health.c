/* SPDX-License-Identifier: GPL-3.0-only */
/* Publish nxbootstrap's run-bound health receipt only after real presents.
 *
 * This is deliberately independent from the framebuffer/video receipt.  A
 * pending generation is promoted only when the game itself proves that it
 * reached a stable runtime boundary.  The launcher owns all values below and
 * pre-creates the destination inside its private runtime directory; outside
 * that contract this helper is a silent no-op.
 *
 * The implementation follows the already published and physically approved
 * Nameless Cat 1.2.7 port-local boundary: one exact JSON line, mode 0600 and
 * an atomic rename.  Party Hard calls it only after 30 successful real page
 * flips on either presentation backend.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

    char temporary[576];
    if (snprintf(temporary, sizeof temporary, "%s.tmp.%d", file,
                 (int)getpid()) >= (int)sizeof temporary)
        return;
    int fd = open(temporary,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0)
        return;
    if (fchmod(fd, 0600) != 0 ||
        write(fd, line, (size_t)length) != (ssize_t)length ||
        fsync(fd) != 0) {
        close(fd);
        unlink(temporary);
        return;
    }
    if (close(fd) != 0) {
        unlink(temporary);
        return;
    }
    if (rename(temporary, file) != 0) {
        unlink(temporary);
        return;
    }
    fprintf(stderr,
            "[st/health] receipt ready published (generation=%.16s...)\n",
            generation);
}
