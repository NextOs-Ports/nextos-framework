// Carregador ELF64 aarch64 — ver so_util.h.
#include "nxgl_gles1.h"
#include "so_util.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE_SIZE_4K 4096
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

static const so_import_t *g_imports;
static size_t g_num_imports;

// Stub para simbolo que o .so importa mas nao implementamos. Loga o nome na
// primeira chamada — assim um import esquecido aparece como mensagem clara em
// vez de pulo pro endereco 0.
static const char *g_missing_name[512];
static int g_missing_count;

static void missing_stub_impl(int idx) {
    fprintf(stderr, "[so_util] IMPORT AUSENTE chamado: %s\n",
            idx < g_missing_count ? g_missing_name[idx] : "?");
    fflush(stderr);
    abort();
}

// Gera um trampolim por simbolo ausente para saber QUAL foi chamado.
// aarch64: mov w0, #idx ; ldr x16, [pc, #8] ; br x16 ; <ptr 8 bytes>
static uintptr_t make_missing_stub(const char *name) {
    if (g_missing_count >= 512) return 0;
    int idx = g_missing_count;
    g_missing_name[idx] = strdup(name);
    g_missing_count++;

    uint32_t *code = mmap(NULL, PAGE_SIZE_4K, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 0;
    code[0] = 0x52800000 | ((idx & 0xffff) << 5); // mov w0, #idx
    code[1] = 0x58000050;                          // ldr x16, [pc, #8]
    code[2] = 0xd61f0200;                          // br x16
    *(uint64_t *)&code[3] = (uint64_t)(uintptr_t)missing_stub_impl;
    __builtin___clear_cache((char *)code, (char *)code + 32);
    mprotect(code, PAGE_SIZE_4K, PROT_READ | PROT_EXEC);
    return (uintptr_t)code;
}

static uintptr_t resolve_import(const char *name) {
    for (size_t i = 0; i < g_num_imports; i++) {
        if (!strcmp(g_imports[i].name, name)) return g_imports[i].addr;
    }
    // GLES1 nao esta' na tabela estatica: vem do resolvedor do nxgl, porque o
    // binario nao linka libGLESv1_CM (SONAME ausente em varias CFWs).
    {
        void *gles1 = nxgl_gles1_lookup(name);
        if (gles1) return (uintptr_t)gles1;
    }
    return make_missing_stub(name);
}

// Procura um simbolo DEFINIDO no proprio modulo (para relocacoes internas que
// referenciam simbolos globais da lib e para so_symbol()).
static Elf64_Sym *find_local_sym(so_module_t *mod, const char *name) {
    Elf64_Sym *syms = mod->dynsym;
    for (size_t i = 0; i < mod->num_dynsym; i++) {
        if (syms[i].st_shndx == SHN_UNDEF) continue;
        if (!strcmp((char *)mod->dynstr + syms[i].st_name, name)) return &syms[i];
    }
    return NULL;
}

uintptr_t so_symbol(so_module_t *mod, const char *name) {
    Elf64_Sym *s = find_local_sym(mod, name);
    return s ? (uintptr_t)mod->base + s->st_value : 0;
}

static int apply_relocations(so_module_t *mod, Elf64_Rela *rela, size_t count) {
    Elf64_Sym *syms = mod->dynsym;
    uint8_t *base = mod->base;

    for (size_t i = 0; i < count; i++) {
        Elf64_Rela *r = &rela[i];
        uint32_t type = ELF64_R_TYPE(r->r_info);
        uint32_t sym_idx = ELF64_R_SYM(r->r_info);
        uint64_t *slot;
        /* Onda v2 (AUD-20): .so malformado nao vira escrita fora do mapa nem
         * leitura de simbolo fora do .dynsym (mesmos checks do nxloader). */
        if (r->r_offset + sizeof(uint64_t) > mod->size) {
            fprintf(stderr, "[so_util] r_offset fora do mapa (0x%llx)\n",
                    (unsigned long long)r->r_offset);
            return -1;
        }
        if (sym_idx >= mod->num_dynsym) {
            fprintf(stderr, "[so_util] indice de simbolo fora do .dynsym (%u)\n",
                    (unsigned)sym_idx);
            return -1;
        }
        slot = (uint64_t *)(base + r->r_offset);

        switch (type) {
        case R_AARCH64_RELATIVE:
            *slot = (uint64_t)(uintptr_t)base + r->r_addend;
            break;

        case R_AARCH64_ABS64:
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT: {
            Elf64_Sym *sym = &syms[sym_idx];
            const char *name = (char *)mod->dynstr + sym->st_name;
            uint64_t value;
            if (sym->st_shndx == SHN_UNDEF) {
                value = resolve_import(name);
            } else {
                value = (uint64_t)(uintptr_t)base + sym->st_value;
            }
            *slot = value + (type == R_AARCH64_ABS64 ? r->r_addend : 0);
            break;
        }

        case R_AARCH64_COPY:
            fprintf(stderr, "[so_util] R_AARCH64_COPY nao suportado\n");
            return -1;

        // Numericos: os nomes variam entre versoes do elf.h.
        case 1030: // R_AARCH64_TLS_TPREL64
        case 1031: // R_AARCH64_TLSDESC
            // O libff3 nao usa TLS proprio; se aparecer, precisa de tratamento.
            fprintf(stderr, "[so_util] relocacao TLS inesperada (tipo %u)\n", type);
            break;

        default:
            fprintf(stderr, "[so_util] relocacao desconhecida tipo %u\n", type);
            return -1;
        }
    }
    return 0;
}

int so_load(so_module_t *mod, const char *path, const so_import_t *imports, size_t num_imports) {
    memset(mod, 0, sizeof *mod);
    g_imports = imports;
    g_num_imports = num_imports;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    uint8_t *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED) {
        perror("mmap arquivo");
        return -1;
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_machine != EM_AARCH64) {
        fprintf(stderr, "[so_util] %s nao e ELF64 aarch64\n", path);
        munmap(file, st.st_size);
        return -1;
    }

    // Extensao total do espaco virtual pedido pelos PT_LOAD.
    Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t min_va = UINT64_MAX, max_va = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < min_va) min_va = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > max_va) max_va = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (min_va == UINT64_MAX) {
        fprintf(stderr, "[so_util] sem PT_LOAD\n");
        munmap(file, st.st_size);
        return -1;
    }
    min_va = ALIGN_DOWN(min_va, PAGE_SIZE_4K);
    max_va = ALIGN_UP(max_va, PAGE_SIZE_4K);
    size_t span = max_va - min_va;

    // Reserva o espaco inteiro de uma vez para os segmentos ficarem contiguos
    // (os deslocamentos relativos do codigo dependem disso).
    uint8_t *base = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap reserva");
        munmap(file, st.st_size);
        return -1;
    }
    mod->base = base;
    mod->size = span;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t seg_start = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE_4K);
        uint64_t seg_end = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE_4K);
        uint8_t *dst = base + (seg_start - min_va);

        if (mmap(dst, seg_end - seg_start, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
            perror("mmap segmento");
            munmap(file, st.st_size);
            return -1;
        }
        memcpy(base + (ph[i].p_vaddr - min_va), file + ph[i].p_offset, ph[i].p_filesz);
        // O resto (bss) ja vem zerado do MAP_ANONYMOUS.
    }

    // Tabela dinamica.
    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)(base + (ph[i].p_vaddr - min_va));
            break;
        }
    }
    if (!dyn) {
        fprintf(stderr, "[so_util] sem PT_DYNAMIC\n");
        munmap(file, st.st_size);
        return -1;
    }

    size_t rela_size = 0, rela_ent = sizeof(Elf64_Rela);
    size_t plt_rela_size = 0;
    size_t init_array_size = 0;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: mod->dynstr = base + d->d_un.d_ptr; break;
        case DT_SYMTAB: mod->dynsym = base + d->d_un.d_ptr; break;
        case DT_RELA: mod->rela = base + d->d_un.d_ptr; break;
        case DT_RELASZ: rela_size = d->d_un.d_val; break;
        case DT_RELAENT: rela_ent = d->d_un.d_val; break;
        case DT_JMPREL: mod->plt_rela = base + d->d_un.d_ptr; break;
        case DT_PLTRELSZ: plt_rela_size = d->d_un.d_val; break;
        case DT_INIT_ARRAY: mod->init_array = (void *)(base + d->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: init_array_size = d->d_un.d_val; break;
        default: break;
        }
    }
    mod->rela_count = rela_ent ? rela_size / rela_ent : 0;
    mod->plt_rela_count = plt_rela_size / sizeof(Elf64_Rela);
    mod->init_array_count = init_array_size / sizeof(void *);

    // num_dynsym nao vem no DT_; derivamos do fim do strtab (layout usual:
    // dynsym vem imediatamente antes de dynstr).
    if (mod->dynsym && mod->dynstr) {
        mod->num_dynsym = ((uint8_t *)mod->dynstr - (uint8_t *)mod->dynsym) / sizeof(Elf64_Sym);
    }

    fprintf(stderr, "[so_util] base=%p span=%zu minva=0x%lx dynsym=%p dynstr=%p nsym=%zu\n", base,
            span, (unsigned long)min_va, mod->dynsym, mod->dynstr, mod->num_dynsym);
    fprintf(stderr, "[so_util] rela=%p n=%zu  pltrela=%p n=%zu  initarr=%p n=%zu\n", mod->rela,
            mod->rela_count, mod->plt_rela, mod->plt_rela_count, (void *)mod->init_array,
            mod->init_array_count);

    if (mod->rela && apply_relocations(mod, mod->rela, mod->rela_count) != 0) return -1;
    fprintf(stderr, "[so_util] .rela.dyn aplicada\n");
    if (mod->plt_rela && apply_relocations(mod, mod->plt_rela, mod->plt_rela_count) != 0) return -1;
    fprintf(stderr, "[so_util] .rela.plt aplicada\n");

    // Aplica as permissoes finais por segmento (texto precisa de PROT_EXEC).
    // ⚠️ O flush de cache tem que ser POR SEGMENTO EXECUTAVEL, nunca no span
    // inteiro: o span cobre buracos PROT_NONE entre segmentos, e as instrucoes
    // dc/ic emitidas pelo clear_cache faltam nessas paginas (core dump).
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t seg_start = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE_4K);
        uint64_t seg_end = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE_4K);
        uint8_t *seg = base + (seg_start - min_va);
        size_t seg_len = seg_end - seg_start;

        if (ph[i].p_flags & PF_X) {
            __builtin___clear_cache((char *)seg, (char *)seg + seg_len);
        }
        int prot = 0;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        mprotect(seg, seg_len, prot);
    }

    munmap(file, st.st_size);

    fprintf(stderr, "[so_util] %s carregado em %p (%zu KB, %zu simbolos, %zu+%zu relocs)\n", path,
            base, span / 1024, mod->num_dynsym, mod->rela_count, mod->plt_rela_count);
    return 0;
}

void so_run_init(so_module_t *mod) {
    for (size_t i = 0; i < mod->init_array_count; i++) {
        if (mod->init_array[i] && (uintptr_t)mod->init_array[i] != (uintptr_t)-1) {
            mod->init_array[i]();
        }
    }
}
