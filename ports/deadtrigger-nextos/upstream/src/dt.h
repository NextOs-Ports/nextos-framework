#ifndef DEADTRIGGER_DT_H
#define DEADTRIGGER_DT_H

#define _GNU_SOURCE

#include <stddef.h>
#include <stdint.h>

#include "so_util.h"

typedef struct dt_module dt_module;

void dt_set_game_root(const char *root);
const char *dt_game_root(void);

int dt_loader_init(void);
dt_module *dt_module_load(const char *name);
dt_module *dt_module_find(const char *name);
void *dt_module_symbol(dt_module *module, const char *name);
void *dt_module_base(dt_module *module);
size_t dt_module_size(dt_module *module);
const char *dt_module_name(dt_module *module);
void dt_loader_set_imports(DynLibFunction *imports, int count);

void *dt_dlopen(const char *filename, int flags);
void *dt_dlsym(void *handle, const char *name);
int dt_dlclose(void *handle);
char *dt_dlerror(void);
int dt_dladdr(const void *address, void *info);

DynLibFunction *dt_imports(void);
int dt_import_count(void);
void dt_imports_init(void);

const char *dt_map_path(const char *path, char *buffer, size_t buffer_size);

uintptr_t dt_unity_base(void);
uintptr_t dt_il2cpp_base(void);
void dt_set_unity_module(dt_module *module);
void dt_set_il2cpp_module(dt_module *module);

void dt_android_set_size(int width, int height);
int dt_android_width(void);
int dt_android_height(void);
void dt_audio_pump(void);
void dt_jni_install_android_contract(void);
void dt_cursor_update(float x, float y, int visible);

#endif
