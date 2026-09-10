#!/usr/bin/env bash
# gera src/imports_unity.gen.c — passthrough libc resolvido via dlsym (runtime),
# resto = stub que loga. Zero conflito de declaracao.
set -euo pipefail
SOS="$*"; [ -n "$SOS" ] || { echo "uso: gen-unity-imports.sh lib1.so [lib2.so...]"; exit 1; }
OUT=src/imports.gen.c
for SO in $SOS; do readelf --dyn-syms -W "$SO" 2>/dev/null | awk '$7=="UND" && $8!="" {print $8}' | sed 's/@.*//'; done | sort -u > /tmp/u.txt
SAFE='^(mem(cpy|move|set|cmp|chr|rchr)|str(len|cpy|ncpy|cat|ncat|cmp|ncmp|chr|rchr|str|dup|ndup|tok|tol|toul|casecmp|ncasecmp|coll|error|spn|cspn|pbrk)|sn?printf|vsn?printf|v?f?printf|s?scanf|fwrite|fread|fopen|fclose|fseeko?|ftello?|fflush|fputs|fgets|fputc|fgetc|putc|getc|ungetc|puts|fdopen|fileno|rewind|setvbuf|setbuf|perror|clearerr|feof|ferror|malloc|calloc|realloc|free|posix_memalign|aligned_alloc|valloc|abort|exit|_exit|atexit|getenv|setenv|unsetenv|putenv|system|qsort|bsearch|rand|srand|rand_r|abs|labs|llabs|div|ldiv|atoi|atol|atoll|atof|strtol|strtoul|strtoll|strtoull|strtod|strtof|isalpha|isdigit|isalnum|isspace|isupper|islower|isprint|ispunct|iscntrl|isxdigit|isgraph|isblank|toupper|tolower|sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|sqrt|cbrt|pow|exp|exp2|expm1|log|log2|log10|log1p|floor|ceil|round|lround|trunc|rint|nearbyint|fmod|remainder|fabsf?|fmin|fmax|fdim|fma|hypotf?|ldexp|frexp|modf|copysign|sinf|cosf|tanf|asinf|acosf|atanf|atan2f|sqrtf|cbrtf|powf|expf|logf|log2f|log10f|floorf|ceilf|roundf|truncf|fmodf|nanf?|isnan|isinf|signbit|open|open64|close|read|pread|write|pwrite|lseek|lseek64|stat|stat64|fstat|fstat64|lstat|statfs|statfs64|statvfs|statvfs64|fstatfs|fstatfs64|fstatvfs|fstatvfs64|access|mkdir|rmdir|unlink|remove|rename|opendir|readdir|readdir64|closedir|rewinddir|readlink|readlinkat|symlink|link|getcwd|chdir|realpath|dup|dup2|pipe|fcntl|ftruncate|truncate|ioctl|select|poll|mmap|mmap64|munmap|mprotect|madvise|msync|sysconf|getpagesize|sched_yield|getpid|getppid|getuid|getgid|gettimeofday|clock|clock_gettime|clock_getres|time|localtime|localtime_r|gmtime|gmtime_r|mktime|strftime|difftime|nanosleep|usleep|sleep|setlocale|localeconv|memmem|getauxval|tolower|toupper|dlopen|dlsym|dlclose|dlerror|pthread_[a-z_]+|sem_[a-z]+|egl[A-Za-z]+|_?setjmp|_?longjmp|sigsetjmp|siglongjmp|__sigsetjmp|sigemptyset|sigfillset|sigaddset|sigdelset|sigismember|sigprocmask|sigaltstack|gettid|prctl|newlocale|freelocale|uselocale|duplocale|wcrtomb|mbrtowc|wcslen|wcscmp|wcsncmp|wcscpy|wcsncpy|wcscat|wcschr|wcsrchr|wcsstr|wcscoll|wcsxfrm|wcsdup|wcstok|wmemcpy|wmemset|wmemmove|wmemcmp|wmemchr|swprintf|vswprintf|fwprintf|vfwprintf|wprintf|vwprintf|swscanf|wcsftime|wcstol|wcstoul|wcstoll|wcstoull|wcstod|wcstof|wcstombs|mbstowcs|wcsnrtombs|mbsnrtowcs|wcrtombs|mbrtowcs|btowc|wctob|mbsinit|mbrlen|towlower|towupper|towctrans|wctrans|iswctype|wctype|wcwidth|iswalpha|iswdigit|iswalnum|iswspace|iswupper|iswlower|iswpunct|iswcntrl|iswprint|iswxdigit|iswgraph|iswblank|__ctype_b_loc|__ctype_tolower_loc|__ctype_toupper_loc|getentropy|getrandom|pipe2|eventfd|epoll_create1|epoll_ctl|epoll_wait|timerfd_create|timerfd_settime|inotify_init1|getcpu|(isalpha|isalnum|isdigit|islower|isupper|isspace|isprint|ispunct|iscntrl|isgraph|isblank|isxdigit|iswalpha|iswalnum|iswdigit|iswlower|iswupper|iswspace|iswprint|iswpunct|iswcntrl|iswgraph|iswblank|iswxdigit|tolower|toupper|towlower|towupper|strcoll|strxfrm|strftime|wcscoll|wcsxfrm|strtol|strtoul|strtoll|strtoull|strtod|strtof|strtold|wcstold)_l|strtold|wcstold|__ctype_get_mb_cur_max|inflate|inflateEnd|inflateInit_|inflateInit2_|inflateReset|inflateReset2|inflateCopy|inflateSync|inflateGetHeader|deflate|deflateEnd|deflateInit_|deflateInit2_|deflateReset|deflateBound|deflateSetDictionary|compress|compress2|compressBound|uncompress|crc32|crc32_z|adler32|adler32_z|zlibVersion|zError|get_crc_table|writev|readv|fsync|fdatasync|isatty|strnlen|strtok_r|strerror_r|strcasestr|strlcpy|strlcat|vasprintf|asprintf|vsscanf|fscanf|uname|mbtowc|wctomb|mbsrtowcs|wcsrtombs|mbstowcs|sincos|sincosf|ldexpf|exp2f|expm1f|log1pf|modff|logb|logbf|scalbn|scalbnf|scalbln|nearbyintf|lldiv|remquo|remquof|srand48|lrand48|drand48|seed48|nrand48|mrand48|jrand48|basename|dirname|fnmatch|flock|chmod|fchmod|fchmodat|futimens|utime|utimes|umask|getpwuid|getpwuid_r|getgrgid|getegid|geteuid|getpriority|setpriority|sched_getaffinity|sched_setaffinity|sched_getparam|sched_setparam|sched_getscheduler|sched_setscheduler|sched_get_priority_max|sched_get_priority_min|sendfile|sync|syncfs|fork|vfork|waitpid|wait|execl|execv|execvp|kill|signal|sigsuspend|sigwait|pthread_sigmask|__cxa_atexit|__cxa_finalize|__cxa_thread_atexit_impl|socket|bind|listen|accept|accept4|connect|send|recv|sendto|recvfrom|sendmsg|recvmsg|shutdown|setsockopt|getsockopt|getsockname|getpeername|socketpair|inet_addr|inet_ntoa|inet_ntop|inet_pton|if_nametoindex|if_indextoname|getaddrinfo|freeaddrinfo|gai_strerror|getnameinfo|gethostbyname|gethostbyaddr|gethostname|htons|htonl|ntohs|ntohl|openlog|closelog|syslog|vsyslog)$'
{
echo "// imports_unity.gen.c — GERADO. passthrough via dlsym; resto = stub log."
echo '#include "so_util.h"'
echo '#include <stdio.h>'
echo '#include <stdint.h>'
echo '#include <dlfcn.h>'
echo
echo "// stubs (logam nome, 1as 2 vezes, retornam 0)"
grep -vE "$SAFE" /tmp/u.txt | while read s; do
  echo "long stub_${s}(void){ static int n=0; if(n++<2) fprintf(stderr,\"[STUB] ${s}\\\\n\"); return 0; }"
done
echo
echo "// flag: 1 = passthrough (resolver via dlsym), 0 = stub ja setado"
echo "static const char *passthrough_names[] = {"
grep -E "$SAFE" /tmp/u.txt | while read s; do echo "  \"$s\","; done
echo "  0 };"
echo
echo "DynLibFunction dynlib_functions[] = {"
while read s; do
  if echo "$s" | grep -qE "$SAFE"; then echo "  {\"$s\", 0},"; else echo "  {\"$s\", (uintptr_t)&stub_${s}},"; fi
done < /tmp/u.txt
echo "};"
echo "size_t dynlib_numfunctions = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);"
echo
echo "// resolve os passthrough via dlsym(RTLD_DEFAULT) em runtime"
echo "void recon_fill_passthrough(void){"
echo "  for(size_t i=0;i<dynlib_numfunctions;i++){"
echo "    if(dynlib_functions[i].func==0){"
echo "      void *p = dlsym(RTLD_DEFAULT, dynlib_functions[i].symbol);"
echo "      if(p) dynlib_functions[i].func=(uintptr_t)p;"
echo "      else fprintf(stderr,\"[passthrough FALHOU dlsym] %s\\\\n\", dynlib_functions[i].symbol);"
echo "    }"
echo "  }"
echo "}"
} > "$OUT"
echo "gerado: $(grep -c '^  {' "$OUT") entradas, $(grep -c '^long stub_' "$OUT") stubs, $(($(grep -c '"' <<< "$(sed -n '/passthrough_names/,/0 };/p' "$OUT")")-0)) passthrough"
