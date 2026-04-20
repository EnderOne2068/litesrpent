/* lseval.h -- evaluator interface used by other internal modules. */
#ifndef LS_EVAL_H
#define LS_EVAL_H
#include "lscore.h"

ls_env_t *ls_env_new(ls_state_t *L, ls_env_t *parent);
void      ls_env_bind(ls_state_t *L, ls_env_t *e, ls_symbol_t *sym, ls_value_t val);
int       ls_env_lookup(ls_env_t *e, ls_symbol_t *sym, ls_value_t *out);
int       ls_env_set(ls_env_t *e, ls_symbol_t *sym, ls_value_t val);

void      ls_dyn_push(ls_state_t *L, ls_symbol_t *sym, ls_value_t new_val);
void      ls_dyn_pop (ls_state_t *L);

#endif
