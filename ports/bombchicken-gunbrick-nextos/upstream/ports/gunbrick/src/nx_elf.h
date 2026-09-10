/*
 * Gunbrick's narrow multi-module adapter for the framework nxloader.
 *
 * This file deliberately exposes only the lifecycle needed by the audited
 * Unity guest.  Mapping, relocation bounds, symbol resolution, W^X and
 * initializer/JNI transitions remain owned by nxloader.
 */

#ifndef GUNBRICK_NX_ELF_H
#define GUNBRICK_NX_ELF_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#include <nxloader.h>

typedef struct {
    const char *name;
    void *addr;
} nx_import;

typedef struct nx_mod {
    char name[64];
    char path[512];
    uint8_t *base;
    size_t span;
    Elf64_Phdr *phdr;
    size_t phnum;
    nxloader_module *module;
    size_t load_index;
    int relocated;
    int resolved;
    int inited;
    int ready;
    int32_t jni_version;
    struct nx_mod *next;
} nx_mod;

/* The table must be name-sorted and remain alive for the process lifetime.
 * Registration is transactional and rejects ambiguous duplicate addresses. */
int nx_set_imports(const nx_import *tab, size_t n);

nx_mod *nx_load(const char *path, const char *soname);
int nx_relocate(nx_mod *module);
int nx_resolve(nx_mod *module);
int nx_run_init(nx_mod *module);
int nx_call_jni_onload(nx_mod *module, void *java_vm,
                       int32_t *returned_version);
nx_mod *nx_find_mod(const char *soname);
void *nx_lookup(const char *symbol);
void *nx_lookup_in(nx_mod *module, const char *symbol);
void *nx_resolve_import(const char *symbol);

extern int nx_verbose;
void nx_log(const char *fmt, ...);
void nx_die(const char *fmt, ...) __attribute__((noreturn));

#endif /* GUNBRICK_NX_ELF_H */
