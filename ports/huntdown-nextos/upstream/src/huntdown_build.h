#ifndef HUNTDOWN_BUILD_H
#define HUNTDOWN_BUILD_H

#include <stddef.h>

typedef enum HdBuild {
  HD_BUILD_UNKNOWN = 0,
  HD_BUILD_200023,
  HD_BUILD_200036,
} HdBuild;

/* Selects a profile from the correlated engine library sizes.  The extractor
 * separately pins their hashes; the runtime repeats the profile check so a
 * partially copied installation can never receive private RVA patches. */
int hd_build_detect(const char *game_dir);
HdBuild hd_build_current(void);
int hd_build_is_unity6(void);
const char *hd_build_version_name(void);
int hd_build_version_code(void);
const char *hd_build_unity_version(void);
const char *hd_build_label(void);

#endif
