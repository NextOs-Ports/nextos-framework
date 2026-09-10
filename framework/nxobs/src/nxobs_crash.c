/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE

#include "nxobs_crash.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define NXOBS_TEXT_MAX 64
#define NXOBS_BUILD_ID_MAX 128
#define NXOBS_MODULE_MAX 96
#define NXOBS_RECEIPT_MAX 4096
#define NXOBS_ALTSTACK_BYTES (64U * 1024U)

struct nxobs_text_slot {
    char value[2][NXOBS_TEXT_MAX];
    volatile sig_atomic_t active;
};

struct nxobs_module {
    uintptr_t start;
    uintptr_t end;
    uintptr_t bias;
    char name[NXOBS_TEXT_MAX];
    char build_id[NXOBS_BUILD_ID_MAX + 1];
};

struct nxobs_buffer {
    char value[NXOBS_RECEIPT_MAX];
    size_t used;
};

static struct nxobs_text_slot g_phase;
static struct nxobs_text_slot g_frame;
static struct nxobs_text_slot g_asset;
static struct nxobs_text_slot g_graphics;
static struct nxobs_text_slot g_provider;
static struct nxobs_module g_modules[NXOBS_MODULE_MAX];
static size_t g_module_count;
static int g_receipt_fd = -1;
static char g_receipt_path[PATH_MAX];
static char g_maps_path[PATH_MAX];
static char g_maps_name[NXOBS_TEXT_MAX];
static volatile sig_atomic_t g_installed;
static volatile sig_atomic_t g_crashed;
static volatile int g_crashed_gate; /* test-and-set: 1a thread vence */
static unsigned char g_altstack[NXOBS_ALTSTACK_BYTES];
static stack_t g_previous_stack;
static int g_stack_installed;

static const int g_signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
static struct sigaction g_previous_handlers[
    sizeof(g_signals) / sizeof(g_signals[0])
];
static size_t g_handler_count;

static int nxobs_safe_character(unsigned char value)
{
    return ((value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '_' ||
            value == ':' || value == '+' || value == '@' || value == '-');
}

static void nxobs_sanitize(char *output, size_t output_size,
                           const char *input, const char *fallback)
{
    size_t index = 0;
    const char *source = input;

    if (output_size == 0) {
        return;
    }
    if (source == NULL || source[0] == '\0') {
        source = fallback;
    }
    while (source != NULL && source[index] != '\0' &&
           index + 1 < output_size) {
        unsigned char value = (unsigned char)source[index];
        output[index] = nxobs_safe_character(value) ? (char)value : '_';
        index += 1;
    }
    if (index == 0 && fallback != NULL) {
        while (fallback[index] != '\0' && index + 1 < output_size) {
            unsigned char value = (unsigned char)fallback[index];
            output[index] = nxobs_safe_character(value) ? (char)value : '_';
            index += 1;
        }
    }
    output[index] = '\0';
}

static int nxobs_valid_port_id(const char *value)
{
    size_t index;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    for (index = 0; value[index] != '\0'; index += 1) {
        unsigned char character = (unsigned char)value[index];
        if (index >= 63 ||
            !((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '.' ||
              character == '_' || character == '-')) {
            return 0;
        }
    }
    return 1;
}

static void nxobs_set_slot(struct nxobs_text_slot *slot, const char *value,
                           const char *fallback)
{
    sig_atomic_t current = slot->active;
    sig_atomic_t next = current == 0 ? 1 : 0;

    nxobs_sanitize(slot->value[next], sizeof(slot->value[next]), value,
                   fallback);
    __sync_synchronize();
    slot->active = next;
}

static const char *nxobs_get_slot(const struct nxobs_text_slot *slot)
{
    sig_atomic_t selected = slot->active;
    if (selected != 0 && selected != 1) {
        selected = 0;
    }
    return slot->value[selected];
}

static const char *nxobs_basename(const char *path)
{
    const char *result = path;
    const char *cursor;

    if (path == NULL || path[0] == '\0') {
        return "main";
    }
    for (cursor = path; *cursor != '\0'; cursor += 1) {
        if (*cursor == '/') {
            result = cursor + 1;
        }
    }
    return result[0] == '\0' ? "main" : result;
}

static size_t nxobs_align4(size_t value)
{
    return (value + 3U) & ~(size_t)3U;
}

static void nxobs_hex_bytes(char *output, size_t output_size,
                            const unsigned char *bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    size_t maximum = (output_size - 1U) / 2U;

    if (count > maximum) {
        count = maximum;
    }
    for (index = 0; index < count; index += 1) {
        output[index * 2U] = digits[(bytes[index] >> 4) & 15U];
        output[index * 2U + 1U] = digits[bytes[index] & 15U];
    }
    output[count * 2U] = '\0';
}

static void nxobs_read_build_id(const struct dl_phdr_info *info,
                                char *output, size_t output_size)
{
    ElfW(Half) index;

    output[0] = '\0';
    for (index = 0; index < info->dlpi_phnum; index += 1) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[index];
        const unsigned char *cursor;
        size_t remaining;

        if (header->p_type != PT_NOTE || header->p_memsz < sizeof(ElfW(Nhdr))) {
            continue;
        }
        cursor = (const unsigned char *)(uintptr_t)(
            info->dlpi_addr + header->p_vaddr
        );
        remaining = (size_t)header->p_memsz;
        while (remaining >= sizeof(ElfW(Nhdr))) {
            const ElfW(Nhdr) *note = (const ElfW(Nhdr) *)(const void *)cursor;
            size_t name_bytes = nxobs_align4((size_t)note->n_namesz);
            size_t desc_bytes = nxobs_align4((size_t)note->n_descsz);
            size_t total = sizeof(*note) + name_bytes + desc_bytes;
            const unsigned char *name;
            const unsigned char *description;

            if (total > remaining || total < sizeof(*note)) {
                break;
            }
            name = cursor + sizeof(*note);
            description = name + name_bytes;
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz >= 3 &&
                name[0] == 'G' && name[1] == 'N' && name[2] == 'U') {
                nxobs_hex_bytes(output, output_size, description,
                                (size_t)note->n_descsz);
                return;
            }
            cursor += total;
            remaining -= total;
        }
    }
}

static int nxobs_collect_module(struct dl_phdr_info *info, size_t size,
                                void *unused)
{
    struct nxobs_module *module;
    uintptr_t start = UINTPTR_MAX;
    uintptr_t end = 0;
    ElfW(Half) index;
    char executable[PATH_MAX];
    const char *name = info->dlpi_name;

    (void)size;
    (void)unused;
    if (g_module_count >= NXOBS_MODULE_MAX) {
        return 1;
    }
    for (index = 0; index < info->dlpi_phnum; index += 1) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[index];
        uintptr_t segment_start;
        uintptr_t segment_end;
        if (header->p_type != PT_LOAD || header->p_memsz == 0) {
            continue;
        }
        segment_start = (uintptr_t)(info->dlpi_addr + header->p_vaddr);
        segment_end = segment_start + (uintptr_t)header->p_memsz;
        if (segment_end < segment_start) {
            continue;
        }
        if (segment_start < start) {
            start = segment_start;
        }
        if (segment_end > end) {
            end = segment_end;
        }
    }
    if (start == UINTPTR_MAX || end <= start) {
        return 0;
    }
    if (name == NULL || name[0] == '\0') {
        ssize_t length = readlink("/proc/self/exe", executable,
                                  sizeof(executable) - 1U);
        if (length > 0) {
            executable[length] = '\0';
            name = executable;
        } else {
            name = "main";
        }
    }
    module = &g_modules[g_module_count];
    module->start = start;
    module->end = end;
    module->bias = (uintptr_t)info->dlpi_addr;
    nxobs_sanitize(module->name, sizeof(module->name), nxobs_basename(name),
                   "module");
    nxobs_read_build_id(info, module->build_id, sizeof(module->build_id));
    g_module_count += 1U;
    return 0;
}

static int nxobs_write_all(int descriptor, const char *value, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(descriptor, value + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int nxobs_write_maps(void)
{
    int descriptor;
    size_t index;

    descriptor = open(g_maps_path,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    if (descriptor < 0) {
        return -1;
    }
    for (index = 0; index < g_module_count; index += 1) {
        char line[512];
        const struct nxobs_module *module = &g_modules[index];
        int length = snprintf(
            line, sizeof(line),
            "%lx-%lx bias=%lx module=%s build_id=%s\n",
            (unsigned long)module->start, (unsigned long)module->end,
            (unsigned long)module->bias, module->name,
            module->build_id[0] == '\0' ? "unavailable" : module->build_id
        );
        if (length <= 0 || (size_t)length >= sizeof(line) ||
            nxobs_write_all(descriptor, line, (size_t)length) != 0) {
            int saved = errno;
            close(descriptor);
            unlink(g_maps_path);
            errno = saved == 0 ? EIO : saved;
            return -1;
        }
    }
    if (fsync(descriptor) != 0 || close(descriptor) != 0) {
        int saved = errno;
        unlink(g_maps_path);
        errno = saved;
        return -1;
    }
    return 0;
}

static void nxobs_append(struct nxobs_buffer *buffer, const char *value)
{
    while (*value != '\0' && buffer->used + 1U < sizeof(buffer->value)) {
        buffer->value[buffer->used] = *value;
        buffer->used += 1U;
        value += 1;
    }
}

static void nxobs_append_u64(struct nxobs_buffer *buffer, uint64_t value)
{
    char digits[32];
    size_t used = 0;
    do {
        digits[used] = (char)('0' + (value % 10U));
        value /= 10U;
        used += 1U;
    } while (value != 0 && used < sizeof(digits));
    while (used > 0) {
        char text[2];
        used -= 1U;
        text[0] = digits[used];
        text[1] = '\0';
        nxobs_append(buffer, text);
    }
}

static void nxobs_append_hex(struct nxobs_buffer *buffer, uintptr_t value)
{
    static const char digits[] = "0123456789abcdef";
    char reversed[2U * sizeof(uintptr_t)];
    size_t used = 0;
    nxobs_append(buffer, "\"0x");
    do {
        reversed[used] = digits[value & 15U];
        value >>= 4U;
        used += 1U;
    } while (value != 0 && used < sizeof(reversed));
    while (used > 0) {
        char text[2];
        used -= 1U;
        text[0] = reversed[used];
        text[1] = '\0';
        nxobs_append(buffer, text);
    }
    nxobs_append(buffer, "\"");
}

static const struct nxobs_module *nxobs_module_for(uintptr_t address)
{
    size_t index;
    for (index = 0; index < g_module_count; index += 1) {
        if (address >= g_modules[index].start &&
            address < g_modules[index].end) {
            return &g_modules[index];
        }
    }
    return NULL;
}

static void nxobs_registers(void *context, uintptr_t *pc, uintptr_t *lr,
                            uintptr_t *sp)
{
    *pc = 0;
    *lr = 0;
    *sp = 0;
#if defined(__x86_64__) && defined(REG_RIP) && defined(REG_RSP)
    if (context != NULL) {
        ucontext_t *machine = (ucontext_t *)context;
        *pc = (uintptr_t)machine->uc_mcontext.gregs[REG_RIP];
        *sp = (uintptr_t)machine->uc_mcontext.gregs[REG_RSP];
    }
#elif defined(__aarch64__)
    if (context != NULL) {
        ucontext_t *machine = (ucontext_t *)context;
        *pc = (uintptr_t)machine->uc_mcontext.pc;
        *lr = (uintptr_t)machine->uc_mcontext.regs[30];
        *sp = (uintptr_t)machine->uc_mcontext.sp;
    }
#elif defined(__arm__)
    if (context != NULL) {
        ucontext_t *machine = (ucontext_t *)context;
        *pc = (uintptr_t)machine->uc_mcontext.arm_pc;
        *lr = (uintptr_t)machine->uc_mcontext.arm_lr;
        *sp = (uintptr_t)machine->uc_mcontext.arm_sp;
    }
#else
    (void)context;
#endif
}

static void nxobs_crash_handler(int signal_number, siginfo_t *information,
                                void *context)
{
    struct nxobs_buffer output;
    const struct nxobs_module *module;
    uintptr_t pc;
    uintptr_t lr;
    uintptr_t sp;
    uintptr_t fault = information == NULL ? 0 :
        (uintptr_t)information->si_addr;
    uintptr_t offset = 0;
    long thread_id = 0;
    int saved_errno = errno;

    if (__sync_lock_test_and_set(&g_crashed_gate, 1) != 0) {
        _exit(128 + signal_number);
    }
    g_crashed = 1;
    nxobs_registers(context, &pc, &lr, &sp);
    module = nxobs_module_for(pc);
#ifdef SYS_gettid
    thread_id = syscall(SYS_gettid);
#endif
    if (module != NULL && pc >= module->bias) {
        offset = pc - module->bias;
    }
    output.used = 0;
    nxobs_append(&output, "{\"build_id\":\"");
    nxobs_append(&output, module != NULL && module->build_id[0] != '\0' ?
                 module->build_id : "unavailable");
    nxobs_append(&output, "\",\"fault_address\":");
    nxobs_append_hex(&output, fault);
    nxobs_append(&output, ",\"frame\":");
    nxobs_append(&output, nxobs_get_slot(&g_frame));
    nxobs_append(&output, ",\"last_asset\":\"");
    nxobs_append(&output, nxobs_get_slot(&g_asset));
    nxobs_append(&output, "\",\"last_graphics_call\":\"");
    nxobs_append(&output, nxobs_get_slot(&g_graphics));
    nxobs_append(&output, "\",\"lr\":");
    nxobs_append_hex(&output, lr);
    nxobs_append(&output, ",\"maps\":\"");
    nxobs_append(&output, g_maps_name);
    nxobs_append(&output, "\",\"module\":\"");
    nxobs_append(&output, module == NULL ? "unavailable" : module->name);
    nxobs_append(&output, "\",\"module_offset\":");
    nxobs_append_hex(&output, offset);
    nxobs_append(&output, ",\"pc\":");
    nxobs_append_hex(&output, pc);
    nxobs_append(&output, ",\"phase\":\"");
    nxobs_append(&output, nxobs_get_slot(&g_phase));
    nxobs_append(&output, "\",\"provider\":\"");
    nxobs_append(&output, nxobs_get_slot(&g_provider));
    nxobs_append(&output, "\",\"schema\":\"nx-crash-v1\","
                 "\"schema_version\":1,\"signal\":");
    nxobs_append_u64(&output, (uint64_t)signal_number);
    nxobs_append(&output, ",\"sp\":");
    nxobs_append_hex(&output, sp);
    nxobs_append(&output, ",\"status\":");
    nxobs_append_u64(&output, (uint64_t)(128 + signal_number));
    nxobs_append(&output, ",\"thread\":");
    nxobs_append_u64(&output, thread_id < 0 ? 0U : (uint64_t)thread_id);
    nxobs_append(&output, "}\n");
    if (g_receipt_fd >= 0) {
        (void)nxobs_write_all(g_receipt_fd, output.value, output.used);
        (void)fsync(g_receipt_fd);
    }
    errno = saved_errno;
    if (kill(getpid(), signal_number) != 0) {
        _exit(128 + signal_number);
    }
}

static void nxobs_restore_handlers(void)
{
    while (g_handler_count > 0) {
        size_t index = g_handler_count - 1U;
        (void)sigaction(g_signals[index], &g_previous_handlers[index], NULL);
        g_handler_count = index;
    }
}

/* Onda v2: o snapshot de modulos era tirado UMA vez no install -- tudo que
 * chega por dlopen/so-loader depois (o provedor grafico, a engine do jogo)
 * caia fora e um SIGSEGV dentro do blob saia com module=unavailable, o caso
 * exatamente interessante. Chamar depois de cada dlopen/so_load relevante.
 * Async-signal-safe nao e' (usa dl_iterate_phdr): chamar do caminho normal,
 * nunca de dentro de um handler. */
void nxobs_crash_refresh_modules(void)
{
    if (g_installed == 0) {
        return;
    }
    g_module_count = 0;
    (void)dl_iterate_phdr(nxobs_collect_module, NULL);
}

int nxobs_crash_install(const struct nxobs_crash_config *config)
{
    struct stat directory_info;
    struct sigaction action;
    stack_t stack;
    pid_t process_id = getpid();
    size_t index;

    if (g_installed != 0 || config == NULL ||
        !nxobs_valid_port_id(config->port_id) ||
        config->runtime_dir == NULL || config->runtime_dir[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (lstat(config->runtime_dir, &directory_info) != 0 ||
        !S_ISDIR(directory_info.st_mode) ||
        directory_info.st_uid != geteuid() ||
        (directory_info.st_mode & 0077) != 0) {
        errno = EPERM;
        return -1;
    }
    if (snprintf(g_receipt_path, sizeof(g_receipt_path),
                 "%s/%s-crash.%ld.jsonl", config->runtime_dir,
                 config->port_id, (long)process_id) >=
        (int)sizeof(g_receipt_path) ||
        snprintf(g_maps_path, sizeof(g_maps_path),
                 "%s/%s-maps.%ld.txt", config->runtime_dir,
                 config->port_id, (long)process_id) >=
        (int)sizeof(g_maps_path) ||
        snprintf(g_maps_name, sizeof(g_maps_name), "%s-maps.%ld.txt",
                 config->port_id, (long)process_id) >=
        (int)sizeof(g_maps_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    nxobs_set_slot(&g_phase, config->initial_phase, "startup");
    nxobs_set_slot(&g_frame, "0", "0");
    nxobs_set_slot(&g_asset, "unavailable", "unavailable");
    nxobs_set_slot(&g_graphics, "unavailable", "unavailable");
    nxobs_set_slot(&g_provider, config->initial_provider, "unavailable");
    g_module_count = 0;
    (void)dl_iterate_phdr(nxobs_collect_module, NULL);
    if (g_module_count == 0 || nxobs_write_maps() != 0) {
        return -1;
    }
    g_receipt_fd = open(
        g_receipt_path,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600
    );
    if (g_receipt_fd < 0) {
        unlink(g_maps_path);
        return -1;
    }
    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = g_altstack;
    stack.ss_size = sizeof(g_altstack);
    if (sigaltstack(&stack, &g_previous_stack) != 0) {
        close(g_receipt_fd);
        g_receipt_fd = -1;
        unlink(g_receipt_path);
        unlink(g_maps_path);
        return -1;
    }
    g_stack_installed = 1;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = nxobs_crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigfillset(&action.sa_mask);
    for (index = 0; index < sizeof(g_signals) / sizeof(g_signals[0]);
         index += 1) {
        if (sigaction(g_signals[index], &action,
                      &g_previous_handlers[index]) != 0) {
            nxobs_restore_handlers();
            (void)sigaltstack(&g_previous_stack, NULL);
            g_stack_installed = 0;
            close(g_receipt_fd);
            g_receipt_fd = -1;
            unlink(g_receipt_path);
            unlink(g_maps_path);
            return -1;
        }
        g_handler_count += 1U;
    }
    g_crashed = 0;
    g_installed = 1;
    return 0;
}

void nxobs_crash_uninstall(void)
{
    if (g_installed == 0) {
        return;
    }
    nxobs_restore_handlers();
    if (g_stack_installed != 0) {
        (void)sigaltstack(&g_previous_stack, NULL);
        g_stack_installed = 0;
    }
    if (g_receipt_fd >= 0) {
        (void)close(g_receipt_fd);
        g_receipt_fd = -1;
    }
    if (g_crashed == 0) {
        (void)unlink(g_receipt_path);
        (void)unlink(g_maps_path);
    }
    g_installed = 0;
}

void nxobs_crash_set_phase(const char *phase)
{
    nxobs_set_slot(&g_phase, phase, "unknown");
}

void nxobs_crash_set_frame(uint64_t frame)
{
    char value[32];
    (void)snprintf(value, sizeof(value), "%llu",
                   (unsigned long long)frame);
    nxobs_set_slot(&g_frame, value, "0");
}

void nxobs_crash_set_last_asset(const char *asset_id)
{
    nxobs_set_slot(&g_asset, asset_id, "unavailable");
}

void nxobs_crash_set_last_graphics_call(const char *call_id)
{
    nxobs_set_slot(&g_graphics, call_id, "unavailable");
}

void nxobs_crash_set_provider(const char *provider_id)
{
    nxobs_set_slot(&g_provider, provider_id, "unavailable");
}

const char *nxobs_crash_receipt_path(void)
{
    return g_receipt_path;
}

const char *nxobs_crash_maps_path(void)
{
    return g_maps_path;
}
