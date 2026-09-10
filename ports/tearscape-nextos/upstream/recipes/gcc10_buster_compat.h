/* Arm GNU 10.3 was configured against a newer glibc than Debian Buster.
 * Disable only timed pthread entry points absent from glibc 2.28; libstdc++
 * retains its portable fallback implementations. */
#ifdef __cplusplus
#include <bits/c++config.h>
#undef _GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT
#undef _GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK
#undef _GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK
#endif
