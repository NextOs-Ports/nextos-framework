/* util.h -- misc utility functions (Linux/SDL port)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(const char *text, ...);

/* no-op on the Amlogic/Linux target (the Switch build raised clocks during
 * shader compilation); kept so shared code compiles unchanged */
void cpu_boost(int on);

int ret0(void);
int retm1(void);

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif
