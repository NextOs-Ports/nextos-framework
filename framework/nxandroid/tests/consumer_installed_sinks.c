/* SPDX-License-Identifier: GPL-3.0-only
 * Proves the INSTALLED nxandroid library exposes the V3 input-sink API. */
#include <nxandroid_input_sinks.h>
#include <stdio.h>

static int hits;
static void sink(void *u, const char *action, int pressed, float v)
{ (void)u; (void)action; (void)pressed; (void)v; hits++; }

int main(void)
{
    nxandroid_input_sink_registry reg;
    nxandroid_input_sinks_init(&reg);
    if (nxandroid_input_sinks_register(&reg, "ui.confirm",
            NXANDROID_SINK_INTERNAL_API, sink, NULL, "ui") != NXANDROID_INPUT_OK) {
        printf("consumer: register failed\n");
        return 1;
    }
    int n = nxandroid_input_sinks_deliver(&reg, "ui.confirm", 1, 1.0f);
    nxandroid_touch_geometry g = { 640, 480, 0, 0, 0, 0, 0 };
    int px = 0, py = 0;
    char err[64];
    if (nxandroid_touch_resolve(&g, 0.5f, 0.5f, &px, &py, err, sizeof err) != 0) {
        printf("consumer: touch resolve failed: %s\n", err);
        return 1;
    }
    if (n != 1) { printf("consumer: deliver count %d\n", n); return 1; }
    printf("nxandroid installed-consumer OK: delivered=%d center=(%d,%d)\n",
           n, px, py);
    return 0;
}
