#define _GNU_SOURCE

#include "dt.h"
#include "prefs_dt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define DT_PREFS_VERSION 1u
#define DT_PREFS_HEADER_SIZE 32u
#define DT_PREFS_RECORD_HEADER_SIZE 12u
#define DT_PREFS_MAX_FILE (2u * 1024u * 1024u)
#define DT_PREFS_MAX_RECORDS 2048u
#define DT_PREFS_MAX_KEY 4096u
#define DT_PREFS_MAX_VALUE (1024u * 1024u)

enum dt_pref_type {
    DT_PREF_STRING = 1,
    DT_PREF_INT = 2,
    DT_PREF_FLOAT = 3,
    DT_PREF_BOOL = 4,
    DT_PREF_LONG = 5,
};

struct dt_pref_entry {
    char *key;
    enum dt_pref_type type;
    union {
        char *string_value;
        int32_t int_value;
        float float_value;
        int bool_value;
        int64_t long_value;
    } value;
};

static const unsigned char g_magic[8] =
    {'D', 'T', 'P', 'R', 'E', 'F', '1', 0};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dt_pref_entry *g_entries;
static size_t g_count;
static size_t g_capacity;
static uint64_t g_generation;
static int g_loaded;
static int g_dirty;
static char g_path[1024];
static char g_temp_path[1040];
static char g_backup_path[1040];

static uint32_t get_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const unsigned char *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static void put_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *p, uint64_t value) {
    put_u32(p, (uint32_t)value);
    put_u32(p + 4, (uint32_t)(value >> 32));
}

static void free_value(struct dt_pref_entry *entry) {
    if (entry->type == DT_PREF_STRING)
        free(entry->value.string_value);
    memset(&entry->value, 0, sizeof entry->value);
}

static void free_entries(struct dt_pref_entry *entries, size_t count) {
    if (!entries)
        return;
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].key);
        free_value(&entries[i]);
    }
    free(entries);
}

static ssize_t find_entry(const char *key) {
    if (!key)
        return -1;
    for (size_t i = 0; i < g_count; ++i)
        if (!strcmp(g_entries[i].key, key))
            return (ssize_t)i;
    return -1;
}

static struct dt_pref_entry *prepare_entry(const char *key,
                                            enum dt_pref_type type) {
    if (!key || !*key || strlen(key) > DT_PREFS_MAX_KEY) {
        errno = EINVAL;
        return NULL;
    }
    ssize_t found = find_entry(key);
    if (found >= 0) {
        struct dt_pref_entry *entry = &g_entries[found];
        if (entry->type != type) {
            free_value(entry);
            entry->type = type;
        }
        return entry;
    }
    if (g_count >= DT_PREFS_MAX_RECORDS) {
        errno = E2BIG;
        return NULL;
    }
    if (g_count == g_capacity) {
        size_t capacity = g_capacity ? g_capacity * 2u : 32u;
        struct dt_pref_entry *resized =
            realloc(g_entries, capacity * sizeof *resized);
        if (!resized)
            return NULL;
        memset(resized + g_capacity, 0,
               (capacity - g_capacity) * sizeof *resized);
        g_entries = resized;
        g_capacity = capacity;
    }
    struct dt_pref_entry *entry = &g_entries[g_count++];
    memset(entry, 0, sizeof *entry);
    entry->key = strdup(key);
    if (!entry->key) {
        --g_count;
        return NULL;
    }
    entry->type = type;
    return entry;
}

static int read_all(int fd, unsigned char *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t amount = read(fd, data + done, size - done);
        if (amount < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (amount == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)amount;
    }
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t amount = write(fd, data + done, size - done);
        if (amount < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (amount == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)amount;
    }
    return 0;
}

static int load_snapshot(const char *path, struct dt_pref_entry **entries_out,
                         size_t *count_out, uint64_t *generation_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat status;
    if (fstat(fd, &status) != 0 || status.st_size < DT_PREFS_HEADER_SIZE ||
        status.st_size > DT_PREFS_MAX_FILE) {
        close(fd);
        return -2;
    }
    size_t size = (size_t)status.st_size;
    unsigned char *data = malloc(size);
    if (!data) {
        close(fd);
        return -1;
    }
    int result = read_all(fd, data, size);
    close(fd);
    if (result != 0) {
        free(data);
        return -1;
    }

    uint32_t version = get_u32(data + 8);
    uint32_t header_size = get_u32(data + 12);
    uint64_t generation = get_u64(data + 16);
    uint32_t count = get_u32(data + 24);
    uint32_t expected_crc = get_u32(data + 28);
    if (memcmp(data, g_magic, sizeof g_magic) ||
        version != DT_PREFS_VERSION ||
        header_size != DT_PREFS_HEADER_SIZE ||
        count > DT_PREFS_MAX_RECORDS ||
        crc32(0, data + header_size, size - header_size) != expected_crc) {
        free(data);
        return -2;
    }

    struct dt_pref_entry *entries =
        calloc(count ? count : 1u, sizeof *entries);
    if (!entries) {
        free(data);
        return -1;
    }
    size_t position = header_size;
    size_t parsed = 0;
    while (parsed < count) {
        if (position + DT_PREFS_RECORD_HEADER_SIZE > size)
            goto invalid;
        enum dt_pref_type type = (enum dt_pref_type)data[position];
        uint32_t key_size = get_u32(data + position + 4);
        uint32_t value_size = get_u32(data + position + 8);
        position += DT_PREFS_RECORD_HEADER_SIZE;
        if (type < DT_PREF_STRING || type > DT_PREF_LONG ||
            key_size == 0 || key_size > DT_PREFS_MAX_KEY ||
            value_size > DT_PREFS_MAX_VALUE ||
            position + (size_t)key_size + value_size > size)
            goto invalid;
        entries[parsed].key = malloc((size_t)key_size + 1u);
        if (!entries[parsed].key)
            goto no_memory;
        memcpy(entries[parsed].key, data + position, key_size);
        entries[parsed].key[key_size] = '\0';
        if (memchr(entries[parsed].key, '\0', key_size))
            goto invalid;
        position += key_size;
        entries[parsed].type = type;
        const unsigned char *value = data + position;
        switch (type) {
            case DT_PREF_STRING:
                entries[parsed].value.string_value =
                    malloc((size_t)value_size + 1u);
                if (!entries[parsed].value.string_value)
                    goto no_memory;
                memcpy(entries[parsed].value.string_value, value, value_size);
                entries[parsed].value.string_value[value_size] = '\0';
                if (memchr(entries[parsed].value.string_value, '\0',
                           value_size))
                    goto invalid;
                break;
            case DT_PREF_INT:
                if (value_size != 4u) goto invalid;
                entries[parsed].value.int_value = (int32_t)get_u32(value);
                break;
            case DT_PREF_FLOAT: {
                if (value_size != 4u) goto invalid;
                uint32_t bits = get_u32(value);
                memcpy(&entries[parsed].value.float_value, &bits, sizeof bits);
                break;
            }
            case DT_PREF_BOOL:
                if (value_size != 1u || value[0] > 1u) goto invalid;
                entries[parsed].value.bool_value = value[0] != 0;
                break;
            case DT_PREF_LONG:
                if (value_size != 8u) goto invalid;
                entries[parsed].value.long_value = (int64_t)get_u64(value);
                break;
        }
        position += value_size;
        ++parsed;
    }
    if (position != size)
        goto invalid;
    free(data);
    *entries_out = entries;
    *count_out = count;
    *generation_out = generation;
    return 1;

no_memory:
    free(data);
    free_entries(entries, parsed + 1u);
    return -1;
invalid:
    free(data);
    free_entries(entries, parsed + 1u);
    return -2;
}

static int ensure_loaded_locked(void) {
    if (g_loaded)
        return 0;
    int n = snprintf(g_path, sizeof g_path, "%s/userdata/playerprefs.dt.bin",
                     dt_game_root());
    if (n < 0 || (size_t)n >= sizeof g_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(g_temp_path, sizeof g_temp_path, "%s.tmp", g_path);
    snprintf(g_backup_path, sizeof g_backup_path, "%s.bak", g_path);

    struct dt_pref_entry *primary = NULL, *backup = NULL;
    size_t primary_count = 0, backup_count = 0;
    uint64_t primary_generation = 0, backup_generation = 0;
    int primary_result =
        load_snapshot(g_path, &primary, &primary_count, &primary_generation);
    int backup_result = load_snapshot(g_backup_path, &backup, &backup_count,
                                      &backup_generation);
    if (primary_result < -1 || backup_result < -1)
        fprintf(stderr, "[prefs] snapshot invalido; tentando copia valida\n");
    if (primary_result < 0 && primary_result != -2)
        return -1;
    if (backup_result < 0 && backup_result != -2) {
        free_entries(primary, primary_count);
        return -1;
    }
    if (backup_result == 1 &&
        (primary_result != 1 || backup_generation > primary_generation)) {
        g_entries = backup;
        g_count = backup_count;
        g_capacity = backup_count;
        g_generation = backup_generation;
        free_entries(primary, primary_count);
        fprintf(stderr, "[prefs] recuperado do backup: %zu chaves\n", g_count);
    } else if (primary_result == 1) {
        g_entries = primary;
        g_count = primary_count;
        g_capacity = primary_count;
        g_generation = primary_generation;
        free_entries(backup, backup_count);
        fprintf(stderr, "[prefs] carregado: %zu chaves, geracao %llu\n",
                g_count, (unsigned long long)g_generation);
    } else {
        free_entries(primary, primary_count);
        free_entries(backup, backup_count);
        fprintf(stderr, "[prefs] novo armazenamento PlayerPrefs\n");
    }
    g_loaded = 1;
    g_dirty = 0;
    return 0;
}

static size_t value_size(const struct dt_pref_entry *entry) {
    switch (entry->type) {
        case DT_PREF_STRING:
            return strlen(entry->value.string_value);
        case DT_PREF_INT:
        case DT_PREF_FLOAT:
            return 4u;
        case DT_PREF_BOOL:
            return 1u;
        case DT_PREF_LONG:
            return 8u;
    }
    return 0;
}

static int sync_parent_directory(void) {
    char parent[sizeof g_path];
    snprintf(parent, sizeof parent, "%s", g_path);
    char *slash = strrchr(parent, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int result = fsync(fd);
    close(fd);
    return result;
}

static int flush_locked(const char *reason) {
    if (ensure_loaded_locked() != 0)
        return -1;
    if (!g_dirty)
        return 0;
    size_t payload_size = 0;
    for (size_t i = 0; i < g_count; ++i) {
        size_t key_size = strlen(g_entries[i].key);
        size_t item_value_size = value_size(&g_entries[i]);
        if (payload_size > DT_PREFS_MAX_FILE -
                               DT_PREFS_RECORD_HEADER_SIZE - key_size -
                               item_value_size) {
            errno = EFBIG;
            return -1;
        }
        payload_size += DT_PREFS_RECORD_HEADER_SIZE + key_size +
                        item_value_size;
    }
    size_t total_size = DT_PREFS_HEADER_SIZE + payload_size;
    if (total_size > DT_PREFS_MAX_FILE) {
        errno = EFBIG;
        return -1;
    }
    unsigned char *data = calloc(1, total_size);
    if (!data)
        return -1;
    memcpy(data, g_magic, sizeof g_magic);
    put_u32(data + 8, DT_PREFS_VERSION);
    put_u32(data + 12, DT_PREFS_HEADER_SIZE);
    put_u64(data + 16, g_generation + 1u);
    put_u32(data + 24, (uint32_t)g_count);
    size_t position = DT_PREFS_HEADER_SIZE;
    for (size_t i = 0; i < g_count; ++i) {
        const struct dt_pref_entry *entry = &g_entries[i];
        uint32_t key_size = (uint32_t)strlen(entry->key);
        uint32_t item_value_size = (uint32_t)value_size(entry);
        data[position] = (unsigned char)entry->type;
        put_u32(data + position + 4, key_size);
        put_u32(data + position + 8, item_value_size);
        position += DT_PREFS_RECORD_HEADER_SIZE;
        memcpy(data + position, entry->key, key_size);
        position += key_size;
        switch (entry->type) {
            case DT_PREF_STRING:
                memcpy(data + position, entry->value.string_value,
                       item_value_size);
                break;
            case DT_PREF_INT:
                put_u32(data + position, (uint32_t)entry->value.int_value);
                break;
            case DT_PREF_FLOAT: {
                uint32_t bits;
                memcpy(&bits, &entry->value.float_value, sizeof bits);
                put_u32(data + position, bits);
                break;
            }
            case DT_PREF_BOOL:
                data[position] = entry->value.bool_value ? 1u : 0u;
                break;
            case DT_PREF_LONG:
                put_u64(data + position,
                        (uint64_t)entry->value.long_value);
                break;
        }
        position += item_value_size;
    }
    put_u32(data + 28,
            crc32(0, data + DT_PREFS_HEADER_SIZE, payload_size));

    int fd = open(g_temp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int result = 0;
    if (fd < 0 || write_all(fd, data, total_size) != 0 || fsync(fd) != 0)
        result = -1;
    if (fd >= 0 && close(fd) != 0)
        result = -1;
    free(data);
    if (result != 0) {
        unlink(g_temp_path);
        return -1;
    }
    if (access(g_path, F_OK) == 0 &&
        rename(g_path, g_backup_path) != 0) {
        unlink(g_temp_path);
        return -1;
    }
    if (rename(g_temp_path, g_path) != 0)
        return -1;
    (void)sync_parent_directory();
    ++g_generation;
    g_dirty = 0;
    fprintf(stderr, "[prefs] %s: %zu chaves, geracao %llu\n",
            reason ? reason : "flush", g_count,
            (unsigned long long)g_generation);
    return 0;
}

int dt_prefs_ready(void) {
    pthread_mutex_lock(&g_lock);
    int result = ensure_loaded_locked();
    pthread_mutex_unlock(&g_lock);
    return result == 0;
}

int dt_prefs_contains(const char *key) {
    pthread_mutex_lock(&g_lock);
    int result = ensure_loaded_locked() == 0 && find_entry(key) >= 0;
    pthread_mutex_unlock(&g_lock);
    return result;
}

char *dt_prefs_get_string_copy(const char *key) {
    char *copy = NULL;
    pthread_mutex_lock(&g_lock);
    if (ensure_loaded_locked() == 0) {
        ssize_t found = find_entry(key);
        if (found >= 0 && g_entries[found].type == DT_PREF_STRING)
            copy = strdup(g_entries[found].value.string_value);
    }
    pthread_mutex_unlock(&g_lock);
    return copy;
}

#define DEFINE_GETTER(name, ctype, kind, member)                              \
    int name(const char *key, ctype *value) {                                 \
        int result = 0;                                                       \
        pthread_mutex_lock(&g_lock);                                          \
        if (ensure_loaded_locked() == 0) {                                    \
            ssize_t found = find_entry(key);                                  \
            if (found >= 0 && g_entries[found].type == (kind)) {              \
                if (value) *value = g_entries[found].value.member;            \
                result = 1;                                                   \
            }                                                                 \
        }                                                                     \
        pthread_mutex_unlock(&g_lock);                                        \
        return result;                                                        \
    }

DEFINE_GETTER(dt_prefs_get_int, int32_t, DT_PREF_INT, int_value)
DEFINE_GETTER(dt_prefs_get_float, float, DT_PREF_FLOAT, float_value)
DEFINE_GETTER(dt_prefs_get_bool, int, DT_PREF_BOOL, bool_value)
DEFINE_GETTER(dt_prefs_get_long, int64_t, DT_PREF_LONG, long_value)

int dt_prefs_set_string(const char *key, const char *value) {
    if (!value || strlen(value) > DT_PREFS_MAX_VALUE) {
        errno = EINVAL;
        return 0;
    }
    int result = 0;
    pthread_mutex_lock(&g_lock);
    if (ensure_loaded_locked() == 0) {
        struct dt_pref_entry *entry = prepare_entry(key, DT_PREF_STRING);
        if (entry) {
            if (!entry->value.string_value ||
                strcmp(entry->value.string_value, value)) {
                char *copy = strdup(value);
                if (copy) {
                    free(entry->value.string_value);
                    entry->value.string_value = copy;
                    g_dirty = result = 1;
                }
            } else {
                result = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

#define DEFINE_SETTER(name, ctype, kind, member)                              \
    int name(const char *key, ctype value) {                                  \
        int result = 0;                                                       \
        pthread_mutex_lock(&g_lock);                                          \
        if (ensure_loaded_locked() == 0) {                                    \
            struct dt_pref_entry *entry = prepare_entry(key, (kind));         \
            if (entry) {                                                      \
                if (entry->value.member != value) {                           \
                    entry->value.member = value;                              \
                    g_dirty = 1;                                              \
                }                                                             \
                result = 1;                                                   \
            }                                                                 \
        }                                                                     \
        pthread_mutex_unlock(&g_lock);                                        \
        return result;                                                        \
    }

DEFINE_SETTER(dt_prefs_set_int, int32_t, DT_PREF_INT, int_value)
DEFINE_SETTER(dt_prefs_set_float, float, DT_PREF_FLOAT, float_value)
DEFINE_SETTER(dt_prefs_set_bool, int, DT_PREF_BOOL, bool_value)
DEFINE_SETTER(dt_prefs_set_long, int64_t, DT_PREF_LONG, long_value)

int dt_prefs_remove(const char *key) {
    int result = 1;
    pthread_mutex_lock(&g_lock);
    if (ensure_loaded_locked() != 0) {
        result = 0;
    } else {
        ssize_t found = find_entry(key);
        if (found >= 0) {
            free(g_entries[found].key);
            free_value(&g_entries[found]);
            memmove(&g_entries[found], &g_entries[found + 1],
                    (g_count - (size_t)found - 1u) * sizeof *g_entries);
            --g_count;
            memset(&g_entries[g_count], 0, sizeof *g_entries);
            g_dirty = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

int dt_prefs_clear(void) {
    pthread_mutex_lock(&g_lock);
    int result = ensure_loaded_locked() == 0;
    if (result && g_count) {
        free_entries(g_entries, g_count);
        g_entries = NULL;
        g_count = g_capacity = 0;
        g_dirty = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

int dt_prefs_flush(const char *reason) {
    pthread_mutex_lock(&g_lock);
    int result = flush_locked(reason);
    if (result != 0)
        fprintf(stderr, "[prefs] falha em %s: %s\n",
                reason ? reason : "flush", strerror(errno));
    pthread_mutex_unlock(&g_lock);
    return result == 0;
}
