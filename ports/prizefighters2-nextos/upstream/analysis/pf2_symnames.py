#!/usr/bin/env python3
"""Name every original import of the PairIP-protected libunity.so.

Two independent constraints pin each hidden name down:
  * the bucket chain of the original SysV .hash (which survives untouched), and
  * the exact name length, from the gaps between the original st_name offsets
    (the original .dynsym also survives; only the head of .dynstr was rewritten).

Ambiguities are broken with a bionic-plausibility filter: Android's libc has no
glibc-only aliases (*f32/*f64/*f128, _IO_*, __libc_*, _nss_*, ...).
"""
import re, sys, os, json, struct, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pf2_imports import Recovered

GLIBC_ONLY = re.compile(
    r'(^_IO_|^__libc_|^_nss_|^__nss|^_dl_|^__gnu|^_obstack|^__old_|^_mcount|^mcheck|'
    r'f32x?$|f64x?$|f128$|^compoundn|^setpayload|^totalorder|^canonicalize|'
    r'^__.*_c[0-9]$|^__.*_chk_|l$)')

def bionic_plausible(n):
    if GLIBC_ONLY.search(n):
        return False
    return True

def build_dict():
    names = set()
    for lib in ['/usr/lib/libc.so.6', '/usr/lib/libm.so.6', '/usr/lib/libz.so.1',
                '/usr/lib/libdl.so.2', '/usr/lib/libpthread.so.0', '/usr/lib/librt.so.1']:
        if not os.path.exists(lib):
            continue
        out = subprocess.run(['nm', '-D', '--defined-only', lib],
                             capture_output=True, text=True).stdout
        for l in out.splitlines():
            p = l.split()
            if len(p) >= 3:
                names.add(p[2].split('@')[0])
    names = {n for n in names if bionic_plausible(n)}
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, 'android_syms.txt')) as f:
        for l in f:
            l = l.strip()
            if l and not l.startswith('#'):
                names.add(l)
    return names

# Functions Unity's libunity is known to pull from bionic; used only to break a
# tie between two same-length, same-bucket candidates.
PREFER = set("""memcpy memmove memset memcmp memchr strlen strcpy strncpy strcat strcmp strncmp
strchr strrchr strstr strdup snprintf vsnprintf sprintf printf fprintf sscanf
malloc free calloc realloc memalign posix_memalign abort exit atexit getenv
open open64 close read write lseek lseek64 pread pwrite fstat stat lstat access
mmap mmap64 munmap mprotect madvise msync fopen fdopen fclose fread fwrite fseek ftell
fflush fgets fputs fputc fgetc feof ferror clearerr fileno setbuf setvbuf remove rename
mkdir rmdir unlink readlink opendir readdir closedir realpath getcwd dup dup2 pipe
poll select ioctl fcntl usleep nanosleep sched_yield sched_getaffinity sched_setaffinity
gettimeofday clock clock_gettime time localtime localtime_r gmtime_r mktime strftime difftime
pthread_create pthread_join pthread_detach pthread_self pthread_equal pthread_exit
pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_unlock
pthread_mutex_trylock pthread_mutexattr_init pthread_mutexattr_destroy pthread_mutexattr_settype
pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait
pthread_cond_signal pthread_cond_broadcast pthread_condattr_init pthread_condattr_setclock
pthread_attr_init pthread_attr_destroy pthread_attr_setstacksize pthread_attr_setdetachstate
pthread_getattr_np pthread_key_create pthread_key_delete pthread_getspecific pthread_setspecific
pthread_once pthread_setname_np pthread_rwlock_init pthread_rwlock_destroy pthread_rwlock_rdlock
pthread_rwlock_wrlock pthread_rwlock_unlock sem_init sem_destroy sem_post sem_wait sem_trywait
sem_timedwait socket connect bind listen accept send recv sendto recvfrom setsockopt getsockopt
shutdown getsockname getpeername inet_pton inet_ntop gethostbyname gethostbyaddr if_nametoindex
signal sigaction sigemptyset sigfillset sigaddset sigdelset sigprocmask sigsuspend sigaltstack
setjmp longjmp raise getpid getuid geteuid getpwuid_r sysconf getpagesize getauxval setpriority
qsort bsearch atoi atol strtol strtoll strtoul strtoull strtod strtof abs labs lldiv ldiv
isspace isdigit toupper tolower basename fnmatch ptrace utimes syscall prctl
crc32 adler32 inflate inflateInit2_ inflateEnd deflate deflateEnd compress uncompress
acos acosf asin asinf atan atanf atan2 atan2f cos cosf sin sinf tan tanf exp expf exp2f
log logf log2 log2f log10 log10f pow powf sqrt sqrtf cbrt cbrtf fmod fmodf modf modff
ldexp ldexpf frexp scalbn hypot logb sincos sincosf truncf floorf ceilf
strerror strerror_r strtok_r strspn strcspn memrchr strnlen wmemcpy wmemmove wmemset
mbrlen mbrtowc mbsrtowcs mbsnrtowcs mbtowc wcrtomb wcslen wcsnrtombs vsscanf vasprintf
vprintf vfprintf newlocale freelocale uselocale strtoll_l strtoull_l strtold_l strftime_l
isdigit_l islower_l isupper_l iswlower_l isxdigit_l openlog closelog syslog
dl_iterate_phdr dlopen dlsym dlclose dlerror dladdr srand48 lrand48 utimes uname
""".split())

def resolve(r):
    """symidx -> name for every original undefined symbol."""
    lo = sorted((struct.unpack_from('<I', r.d, r.symbase + i * r.ent)[0], i)
                for i in range(1, r.nsyms))
    lens = {}
    low = [(o, i) for o, i in lo if 0 < o < r.boundary]
    for k, (o, i) in enumerate(low):
        end = low[k + 1][0] if k + 1 < len(low) else r.boundary
        lens[i] = end - o - 1
    names = build_dict()
    known = dict(r.symname)
    taken = set(known.values())
    cand = {}
    for n in names:
        if n in taken:
            continue
        for i in r.chainof(n):
            if i in known:
                continue
            if i in lens and lens[i] != len(n):
                continue
            cand.setdefault(i, set()).add(n)
    out = dict(known)
    unresolved, ambiguous = [], {}
    for i, v in cand.items():
        if len(v) == 1:
            out[i] = next(iter(v))
        else:
            pref = v & PREFER
            if len(pref) == 1:
                out[i] = next(iter(pref))
            else:
                ambiguous[i] = sorted(v)
    for i in lens:
        if i not in out and i not in ambiguous:
            unresolved.append(i)
    return out, ambiguous, unresolved, lens

if __name__ == '__main__':
    r = Recovered('split/lib/arm64-v8a/libunity.so', True)
    names, amb, unres, lens = resolve(r)
    print('named symbols: %d / %d' % (len(names), r.nsyms - 1))
    print('ambiguous: %d  unresolved: %d' % (len(amb), len(unres)))
    for i in sorted(amb):
        print('  AMB idx=%d len=%s %s' % (i, lens.get(i), amb[i]))
    for i in sorted(unres):
        print('  UNRES idx=%d len=%s' % (i, lens.get(i)))
    slotname = {}
    for s, n in r.survivors.items():
        slotname[s] = n
    for s in r.hidden:
        si = r.leftover.get(s)
        if si is not None and si in names:
            slotname[s] = names[si]
    unknown = [s for s in r.hidden if s not in slotname]
    print('slots named: %d / %d ; unknown slots: %d' % (len(slotname), r.nslots - 3, len(unknown)))
    json.dump({'slotname': slotname, 'symname': names, 'unknown_slots': unknown,
               'ambiguous': amb}, open('work/pf2_unity_symbols.json', 'w'), indent=1)
