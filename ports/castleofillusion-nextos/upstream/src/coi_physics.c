/* coi_physics.c — intercepta btDiscreteDynamicsWorld::stepSimulation (virtual)
 * pela vtable, sem patch de codigo.
 *
 * Por que: a 19 fps o Bullet entra na espiral classica — dt de ~52 ms com
 * maxSubSteps>1 vira 3 sub-passos de fisica POR FRAME (quanto mais lento,
 * mais fisica roda). Limitar maxSubSteps corta a fisica para 1 passo por
 * frame mantendo a velocidade do jogo, ao custo de precisao de colisao.
 *
 * Enderecos (VADDR na libViewer_GP.so, somados a g_load_base em runtime):
 *   _ZTV23btDiscreteDynamicsWorld               @ 0x87dcb0  (vtable)
 *   _ZN23btDiscreteDynamicsWorld14stepSimulationEfif @ 0x40ad84
 *
 * Controle por ambiente:
 *   COI_PHYS_LOG=1  -> loga (dt, maxSubSteps, fixedTimeStep) dos 1os frames.
 *   COI_PHYS_CAP=N  -> limita maxSubSteps a N (1 recomendado; 0 = passo unico).
 * Sem nenhum dos dois, o hook NAO e' instalado (comportamento 1.0.1 intacto).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define PHYS_VTABLE_VADDR 0x87dcb0u
#define PHYS_STEP_VADDR   0x40ad84u

typedef int (*coi_step_fn)(void *world, float dt, int maxSubSteps,
                           float fixedTimeStep);

static coi_step_fn coi_orig_step;
static int coi_phys_cap = -1;      /* -1 = passthrough; >=0 = limite */
static int coi_phys_log;
static unsigned coi_phys_calls;

static int coi_step_wrapper(void *world, float dt, int maxSubSteps,
                            float fixedTimeStep)
{
    if (coi_phys_log && coi_phys_calls < 120)
        fprintf(stderr, "[coi/phys] step dt=%.4f maxSubSteps=%d fixed=%.4f\n",
                (double)dt, maxSubSteps, (double)fixedTimeStep);
    coi_phys_calls++;
    int ms = maxSubSteps;
    if (coi_phys_cap >= 0 && ms > coi_phys_cap)
        ms = coi_phys_cap;
    return coi_orig_step(world, dt, ms, fixedTimeStep);
}

void coi_physics_install(uintptr_t load_base)
{
    const char *cap = getenv("COI_PHYS_CAP");
    coi_phys_cap = (cap && *cap) ? atoi(cap) : -1;
    coi_phys_log = getenv("COI_PHYS_LOG") ? 1 : 0;
    if (coi_phys_cap < 0 && !coi_phys_log)
        return;                    /* nada pedido: nao toca na vtable */

    uintptr_t target = load_base + PHYS_STEP_VADDR;
    uintptr_t *vt = (uintptr_t *)(load_base + PHYS_VTABLE_VADDR);
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if (vt[i] == target) { slot = i; break; }
    }
    if (slot < 0) {
        fprintf(stderr, "[coi/phys] stepSimulation nao esta' na vtable "
                        "esperada (base=0x%lx) -- sem hook, jogo intacto\n",
                (unsigned long)load_base);
        return;
    }

    coi_orig_step = (coi_step_fn)vt[slot];
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t page = (uintptr_t)&vt[slot] & ~(uintptr_t)(ps - 1);
    if (mprotect((void *)page, (size_t)ps * 2,
                 PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "[coi/phys] mprotect da vtable falhou -- sem hook\n");
        return;
    }
    vt[slot] = (uintptr_t)coi_step_wrapper;
    mprotect((void *)page, (size_t)ps * 2, PROT_READ);
    fprintf(stderr, "[coi/phys] stepSimulation hookado (slot %d, cap=%d, "
                    "log=%d)\n", slot, coi_phys_cap, coi_phys_log);
}
