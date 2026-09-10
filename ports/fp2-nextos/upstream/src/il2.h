/* Acesso enxuto ao IL2CPP do jogo pela API exportada do libil2cpp.
 *
 * Tudo aqui e' resolvido por NOME de classe/metodo em tempo de execucao —
 * nenhum RVA cravado, entao nao herda offset de outro jogo (licao do Hitman
 * GO).  Usado pela sonda de diagnostico (probe.c) e pela ligacao do controle
 * (pad.c).
 */
#ifndef FP2_IL2_H
#define FP2_IL2_H

#include <stddef.h>
#include <stdint.h>

typedef void Il2CppClass;
typedef void Il2CppObject;
typedef void MethodInfo;
typedef void FieldInfo;

/* Resolve a tabela de funcoes; devolve 1 quando o libil2cpp esta carregado e
 * todos os simbolos existem.  Idempotente e barato depois da 1a vez. */
int il2_load(void);

/* Prende a thread corrente ao dominio (obrigatorio antes de invocar C#). */
void il2_attach(void);

/* Classe por (namespace, nome) varrendo todos os assemblies. NULL se ausente
 * (e loga).  Classes ANINHADAS nao sao achadas assim — use il2_class_of. */
Il2CppClass *il2_class(const char *ns, const char *name);
Il2CppClass *il2_class_of(Il2CppObject *obj);
const char *il2_class_name(Il2CppClass *k);

/* Metodo por nome + aridade, pulando definicoes genericas (nao invocaveis).
 * NULL se ausente (e loga). */
const MethodInfo *il2_method(Il2CppClass *k, const char *name, unsigned argc);
/* Desempata sobrecargas de mesma aridade pela classe do 1o parametro
 * ("String", "Int32", "Type"...). */
const MethodInfo *il2_method_p(Il2CppClass *k, const char *name, unsigned argc,
                               const char *param0_class);
/* Igual, mas subindo a cadeia de heranca (propriedade de classe base). */
const MethodInfo *il2_method_deep(Il2CppClass *k, const char *name,
                                  unsigned argc);
Il2CppClass *il2_class_parent(Il2CppClass *k);

/* Invoca; em excecao gerenciada loga o tipo e devolve NULL. */
Il2CppObject *il2_call(const MethodInfo *m, void *self, void **args,
                       const char *what);

FieldInfo *il2_field(Il2CppClass *k, const char *name);
void il2_field_get(Il2CppObject *obj, FieldInfo *f, void *out);
/* Para campo de tipo-referencia passe o PROPRIO objeto (nao &obj): o il2cpp
 * grava `value` direto.  Para tipo-valor passe o ponteiro para o valor. */
void il2_field_set(Il2CppObject *obj, FieldInfo *f, void *value);
void il2_static_get(FieldInfo *f, void *out);

void *il2_unbox(Il2CppObject *boxed);
/* System.Type de uma classe (para FindObjectsOfType/GetComponent). */
Il2CppObject *il2_type(Il2CppClass *k);

/* Arrays e strings gerenciadas (layout de 32 bits). */
uint32_t il2_arr_len(Il2CppObject *arr);
void *il2_arr_at(Il2CppObject *arr, uint32_t i);
const char *il2_str_utf8(Il2CppObject *s, char *out, size_t size);
Il2CppObject *il2_str_new(const char *utf8);
/* Raiz de GC permanente para um objeto guardado so' do lado nativo. */
void il2_pin(Il2CppObject *obj);

#endif
