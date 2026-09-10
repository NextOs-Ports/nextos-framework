/* SPDX-License-Identifier: GPL-3.0-only
 * Proves the INSTALLED nxcompat library exposes the V3 language/settings
 * APIs: includes only installed headers, links only the installed archive. */
#include <nxcompat_language_v2.h>
#include <nxcompat_settings.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* settings parser reachable from the installed lib */
    static const char cfg[] = "# NEXTOS_SETTINGS/1\nlanguage=pt-BR\n";
    nxcompat_settings s;
    if (nxcompat_settings_parse(cfg, strlen(cfg), &s, NULL, NULL) != 0) {
        printf("consumer: settings parse failed\n");
        return 1;
    }
    /* language resolver reachable; pt-BR must resolve against a supported set */
    static const char *supported[] = { "en", "pt-BR" };
    nxcompat_language_snapshot snap;
    int rc = nxcompat_language_resolve_v2(NULL, s.language, NULL, NULL,
                                          supported, 2, "en", &snap);
    if (rc != 0) { printf("consumer: resolve failed\n"); return 1; }
    printf("nxcompat installed-consumer OK: settings_lang=%s resolved=%s\n",
           s.language, snap.canonical_tag);
    return 0;
}
