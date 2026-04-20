/* lsenv.c -- lexical environments.
 *
 * An ls_env is a frame holding (symbol -> value) bindings, with a
 * parent pointer.  Lookup walks the chain.  Special variables
 * (declared via defvar/defparameter or marked SPECIAL) bypass the
 * lexical chain and use the dynamic binding stack instead. */
#include "lscore.h"

ls_env_t *ls_env_new(ls_state_t *L, ls_env_t *parent) {
    ls_value_t v = ls_make_obj(L, LS_T_SPECIAL, sizeof(ls_env_t));
    /* tag isn't really used for env -- it's an internal type */
    ls_env_t *e = (ls_env_t *)v.u.ptr;
    e->parent = parent;
    e->count = 0;
    e->cap = 0;
    e->bindings = NULL;
    return e;
}

void ls_env_bind(ls_state_t *L, ls_env_t *e, ls_symbol_t *sym, ls_value_t val) {
    (void)L;
    if (e->count == e->cap) {
        size_t nc = e->cap ? e->cap * 2 : 4;
        e->bindings = (ls_env_binding_t *)realloc(e->bindings, nc * sizeof *e->bindings);
        e->cap = nc;
    }
    e->bindings[e->count].sym = sym;
    e->bindings[e->count].val = val;
    e->bindings[e->count].special = (sym->sym_flags & LS_SYM_SPECIAL) ? 1 : 0;
    e->count++;
}

int ls_env_lookup(ls_env_t *e, ls_symbol_t *sym, ls_value_t *out) {
    while (e) {
        for (size_t i = e->count; i-- > 0; )
            if (e->bindings[i].sym == sym) { *out = e->bindings[i].val; return 1; }
        e = e->parent;
    }
    return 0;
}

int ls_env_set(ls_env_t *e, ls_symbol_t *sym, ls_value_t val) {
    while (e) {
        for (size_t i = e->count; i-- > 0; )
            if (e->bindings[i].sym == sym) { e->bindings[i].val = val; return 1; }
        e = e->parent;
    }
    return 0;
}

/* Dynamic bindings: pushed when binding a SPECIAL variable. */
void ls_dyn_push(ls_state_t *L, ls_symbol_t *sym, ls_value_t new_val) {
    ls_dynframe_t *d = (ls_dynframe_t *)calloc(1, sizeof *d);
    d->sym = sym;
    d->old_val = sym->value;
    d->had_value = (sym->sym_flags & LS_SYM_HAS_VALUE) ? 1 : 0;
    d->next = L->dyn_top;
    L->dyn_top = d;
    sym->value = new_val;
    sym->sym_flags |= LS_SYM_HAS_VALUE;
}
void ls_dyn_pop(ls_state_t *L) {
    ls_dynframe_t *d = L->dyn_top;
    if (!d) return;
    L->dyn_top = d->next;
    if (d->had_value) d->sym->value = d->old_val;
    else { d->sym->value = ls_nil_v(); d->sym->sym_flags &= ~LS_SYM_HAS_VALUE; }
    free(d);
}
