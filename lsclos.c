/* lsclos.c -- CLOS (Common Lisp Object System) and condition system.
 *
 * Implements:  class hierarchy, class-of, make-instance, slot-value,
 *   defclass (C3 linearization), defgeneric, defmethod, method dispatch,
 *   and the condition/restart system (signal, handler-bind, handler-case,
 *   restart-case, invoke-restart, find-restart).
 *
 * Each builtin is a static C function with signature:
 *   ls_value_t fn(ls_state_t *L, int nargs, ls_value_t *args);
 *
 * Registration happens in ls_init_clos() called from ls_new(). */
#include "lscore.h"
#include "lseval.h"

/* ============================================================
 *  Helpers
 * ============================================================ */
#undef A
#define A(i)  args[i]

/* ls_nil_v / ls_t_v are declared in lscore.h and defined in lscore.c */

/* Intern a symbol in the CL package for convenience. */
static ls_symbol_t *intern_cl(ls_state_t *L, const char *name) {
    ls_value_t sv = ls_intern(L, "COMMON-LISP", name);
    return ls_symbol_p(sv);
}

/* Intern a keyword. */
static ls_symbol_t *intern_kw(ls_state_t *L, const char *name) {
    ls_value_t sv = ls_intern(L, "KEYWORD", name);
    return ls_symbol_p(sv);
}

/* Make a one-element list. */
static ls_value_t list1(ls_state_t *L, ls_value_t a) {
    return ls_cons(L, a, ls_nil_v());
}

/* Check if val is in a list (pointer identity on class pointers). */
static int list_memq(ls_value_t list, ls_value_t item) {
    while (LS_CONSP(list)) {
        if (LS_CAR(list).u.ptr == item.u.ptr) return 1;
        list = LS_CDR(list);
    }
    return 0;
}

/* ============================================================
 *  CLASS-OF  --  return the class metaobject for any value
 * ============================================================ */
ls_value_t ls_class_of(ls_state_t *L, ls_value_t v) {
    switch (v.tag) {
    case LS_T_NIL:       return ls_wrap(LS_T_CLASS, L->class_symbol);
    case LS_T_T:         return ls_wrap(LS_T_CLASS, L->class_symbol);
    case LS_T_FIXNUM:    return ls_wrap(LS_T_CLASS, L->class_fixnum);
    case LS_T_FLONUM:    return ls_wrap(LS_T_CLASS, L->class_float);
    case LS_T_SYMBOL:    return ls_wrap(LS_T_CLASS, L->class_symbol);
    case LS_T_STRING:    return ls_wrap(LS_T_CLASS, L->class_string);
    case LS_T_CONS:      return ls_wrap(LS_T_CLASS, L->class_cons);
    case LS_T_VECTOR:    return ls_wrap(LS_T_CLASS, L->class_vector);
    case LS_T_HASHTABLE: return ls_wrap(LS_T_CLASS, L->class_hashtable);
    case LS_T_FUNCTION:
    case LS_T_BYTECODE:
    case LS_T_NATIVE:    return ls_wrap(LS_T_CLASS, L->class_function);
    case LS_T_CLASS:     return ls_wrap(LS_T_CLASS, L->class_t);
    case LS_T_INSTANCE:  {
        ls_instance_t *inst = ls_instance_p(v);
        return ls_wrap(LS_T_CLASS, inst->class_);
    }
    case LS_T_CONDITION: {
        ls_condition_t *c = (ls_condition_t *)v.u.ptr;
        return ls_wrap(LS_T_CLASS, c->class_);
    }
    default: return ls_wrap(LS_T_CLASS, L->class_t);
    }
}

/* ============================================================
 *  C3 LINEARIZATION  --  compute class precedence list
 * ============================================================ */

/* Merge step: pick the class that appears at the head of some list
 * and does not appear in the tail of any other list. */
static ls_value_t c3_head_candidate(ls_state_t *L, ls_value_t seqs) {
    /* seqs is a list of lists. */
    ls_value_t s = seqs;
    while (LS_CONSP(s)) {
        ls_value_t seq = LS_CAR(s);
        if (LS_CONSP(seq)) {
            ls_value_t candidate = LS_CAR(seq);
            /* Check candidate is not in the tail of any other list. */
            int blocked = 0;
            ls_value_t s2 = seqs;
            while (LS_CONSP(s2)) {
                ls_value_t other = LS_CAR(s2);
                if (LS_CONSP(other)) {
                    ls_value_t tail = LS_CDR(other);
                    if (list_memq(tail, candidate)) { blocked = 1; break; }
                }
                s2 = LS_CDR(s2);
            }
            if (!blocked) return candidate;
        }
        s = LS_CDR(s);
    }
    return ls_nil_v();  /* should not happen for well-formed hierarchies */
}

/* Remove a class from the head of every sub-list, drop empty lists. */
static ls_value_t c3_remove_head(ls_state_t *L, ls_value_t seqs, ls_value_t cls) {
    ls_value_t result = ls_nil_v();
    ls_gc_push_root(L, &result);
    ls_value_t s = seqs;
    while (LS_CONSP(s)) {
        ls_value_t seq = LS_CAR(s);
        if (LS_CONSP(seq) && LS_CAR(seq).u.ptr == cls.u.ptr)
            seq = LS_CDR(seq);
        if (LS_CONSP(seq))
            result = ls_cons(L, seq, result);
        s = LS_CDR(s);
    }
    ls_gc_pop_root(L);
    return ls_list_reverse(L, result);
}

/* All sub-lists empty? */
static int c3_all_empty(ls_value_t seqs) {
    while (LS_CONSP(seqs)) {
        if (LS_CONSP(LS_CAR(seqs))) return 0;
        seqs = LS_CDR(seqs);
    }
    return 1;
}

/* Compute C3 linearization for a class given its direct supers. */
static ls_value_t c3_linearize(ls_state_t *L, ls_class_t *cls) {
    ls_value_t result = ls_nil_v();
    ls_gc_push_root(L, &result);

    /* Build initial sequence list:
     * for each direct super, its CPL; then the direct-supers list itself. */
    ls_value_t seqs = ls_nil_v();
    ls_gc_push_root(L, &seqs);

    ls_value_t supers = cls->direct_supers;
    ls_value_t s = supers;
    while (LS_CONSP(s)) {
        ls_class_t *sup = ls_class_p(LS_CAR(s));
        if (LS_CONSP(sup->precedence_list))
            seqs = ls_cons(L, sup->precedence_list, seqs);
        s = LS_CDR(s);
    }
    /* Add the direct-supers list itself as the last constraint. */
    if (LS_CONSP(supers))
        seqs = ls_cons(L, supers, seqs);
    seqs = ls_list_reverse(L, seqs);

    /* Start result with the class itself. */
    result = list1(L, ls_wrap(LS_T_CLASS, cls));

    /* Merge loop. */
    while (!c3_all_empty(seqs)) {
        ls_value_t candidate = c3_head_candidate(L, seqs);
        if (LS_NILP(candidate)) {
            ls_error(L, "inconsistent class hierarchy for %s",
                     cls->name ? cls->name->name->chars : "<anon>");
            break;
        }
        result = ls_cons(L, candidate, result);
        seqs = c3_remove_head(L, seqs, candidate);
    }

    ls_gc_pop_root(L);  /* seqs */
    ls_gc_pop_root(L);  /* result */
    return ls_list_reverse(L, result);
}

/* ============================================================
 *  EFFECTIVE SLOTS  --  collect slots from CPL
 * ============================================================ */
static ls_value_t compute_effective_slots(ls_state_t *L, ls_class_t *cls) {
    ls_value_t result = ls_nil_v();
    ls_gc_push_root(L, &result);
    ls_hashtable_t *seen = ls_hash_new(L, LS_HASH_EQ, 16);
    ls_value_t seen_v = ls_wrap(LS_T_HASHTABLE, seen);
    ls_gc_push_root(L, &seen_v);

    ls_value_t cpl = cls->precedence_list;
    while (LS_CONSP(cpl)) {
        ls_class_t *c = ls_class_p(LS_CAR(cpl));
        ls_value_t slots = c->direct_slots;
        while (LS_CONSP(slots)) {
            ls_value_t slot_name = LS_CAR(slots);
            ls_value_t dummy;
            if (!ls_hash_get_sv(seen, slot_name, &dummy)) {
                ls_hash_put(L, seen, slot_name, ls_t_v());
                result = ls_cons(L, slot_name, result);
            }
            slots = LS_CDR(slots);
        }
        cpl = LS_CDR(cpl);
    }
    ls_gc_pop_root(L);  /* seen_v */
    ls_gc_pop_root(L);  /* result */
    return ls_list_reverse(L, result);
}

/* Build the slot-name -> index hashtable. */
static void build_slot_index(ls_state_t *L, ls_class_t *cls) {
    cls->slot_index = ls_hash_new(L, LS_HASH_EQ, cls->n_slots < 4 ? 8 : cls->n_slots * 2);
    ls_value_t slots = cls->effective_slots;
    uint32_t idx = 0;
    while (LS_CONSP(slots)) {
        ls_hash_put(L, cls->slot_index, LS_CAR(slots), ls_make_fixnum((int64_t)idx));
        idx++;
        slots = LS_CDR(slots);
    }
}

/* ============================================================
 *  FINALIZE-CLASS  --  compute CPL, effective slots, slot-index
 * ============================================================ */
static void finalize_class(ls_state_t *L, ls_class_t *cls) {
    if (cls->finalized) return;

    /* Ensure all supers are finalized first. */
    ls_value_t s = cls->direct_supers;
    while (LS_CONSP(s)) {
        ls_class_t *sup = ls_class_p(LS_CAR(s));
        if (!sup->finalized) finalize_class(L, sup);
        s = LS_CDR(s);
    }

    cls->precedence_list = c3_linearize(L, cls);
    cls->effective_slots = compute_effective_slots(L, cls);
    cls->n_slots = (uint32_t)ls_list_length(cls->effective_slots);
    cls->instance_size = (uint32_t)(sizeof(ls_instance_t) + cls->n_slots * sizeof(ls_value_t));
    build_slot_index(L, cls);
    cls->finalized = 1;
}

/* ============================================================
 *  ALLOCATE A NEW CLASS OBJECT
 * ============================================================ */
static ls_class_t *make_class(ls_state_t *L, const char *name,
                              ls_value_t supers, ls_value_t slots) {
    ls_value_t cv = ls_make_obj(L, LS_T_CLASS, sizeof(ls_class_t));
    ls_class_t *cls = ls_class_p(cv);
    cls->name = intern_cl(L, name);
    cls->direct_supers = supers;
    cls->direct_slots = slots;
    cls->precedence_list = ls_nil_v();
    cls->effective_slots = ls_nil_v();
    cls->metaclass = ls_nil_v();
    cls->instance_size = 0;
    cls->n_slots = 0;
    cls->finalized = 0;
    cls->slot_index = NULL;
    return cls;
}

/* Public: define a class from C. */
ls_class_t *ls_define_class(ls_state_t *L, const char *name,
                            ls_value_t supers, ls_value_t slots) {
    ls_class_t *cls = make_class(L, name, supers, slots);
    finalize_class(L, cls);
    /* Bind the class as the symbol's value. */
    ls_symbol_t *sym = cls->name;
    sym->value = ls_wrap(LS_T_CLASS, cls);
    sym->sym_flags |= LS_SYM_HAS_VALUE;
    return cls;
}

/* ============================================================
 *  SLOT ACCESS
 * ============================================================ */

/* Return the integer slot index for a slot name, or -1. */
static int slot_index_of(ls_class_t *cls, ls_value_t slot_name) {
    if (!cls->slot_index) return -1;
    ls_value_t idx;
    if (ls_hash_get_sv(cls->slot_index, slot_name, &idx))
        return (int)idx.u.fixnum;
    return -1;
}

/* Get the value of a slot by name.  Signals an error if unbound. */
ls_value_t ls_slot_value(ls_state_t *L, ls_instance_t *inst, ls_value_t slot_name) {
    int idx = slot_index_of(inst->class_, slot_name);
    if (idx < 0)
        ls_error(L, "slot %s not found in class %s",
                 ls_symbol_p(slot_name)->name->chars,
                 inst->class_->name->name->chars);
    return inst->slots[idx];
}

/* Set the value of a slot by name. */
void ls_slot_set(ls_state_t *L, ls_instance_t *inst, ls_value_t slot_name, ls_value_t val) {
    int idx = slot_index_of(inst->class_, slot_name);
    if (idx < 0)
        ls_error(L, "slot %s not found in class %s",
                 ls_symbol_p(slot_name)->name->chars,
                 inst->class_->name->name->chars);
    inst->slots[idx] = val;
}

/* ============================================================
 *  MAKE-INSTANCE
 * ============================================================ */
static ls_value_t make_instance(ls_state_t *L, ls_class_t *cls,
                                int nargs, ls_value_t *args) {
    if (!cls->finalized) finalize_class(L, cls);

    ls_value_t iv = ls_make_obj(L, LS_T_INSTANCE, sizeof(ls_instance_t));
    ls_instance_t *inst = ls_instance_p(iv);
    inst->class_ = cls;

    /* Allocate slot storage. */
    uint32_t ns = cls->n_slots;
    if (ns > 0) {
        inst->slots = (ls_value_t *)malloc(ns * sizeof(ls_value_t));
        for (uint32_t i = 0; i < ns; i++)
            inst->slots[i] = ls_nil_v();  /* unbound sentinel */
    } else {
        inst->slots = NULL;
    }

    /* Process initargs (keyword arguments: :slot-name value ...) */
    for (int i = 0; i + 1 < nargs; i += 2) {
        ls_value_t key = args[i];
        ls_value_t val = args[i + 1];
        /* A keyword argument :FOO should set slot FOO. */
        ls_symbol_t *ks = ls_symbol_p(key);
        if (!ks) continue;
        /* Find slot with matching name (strip keyword package). */
        const char *sn = ks->name->chars;
        ls_value_t slot_sym = ls_intern(L, "COMMON-LISP", sn);
        int idx = slot_index_of(cls, slot_sym);
        if (idx >= 0)
            inst->slots[idx] = val;
    }
    return iv;
}

/* ============================================================
 *  GENERIC FUNCTIONS AND METHODS
 * ============================================================ */

/* Create a generic function object. */
ls_generic_t *ls_define_generic(ls_state_t *L, const char *name,
                                ls_value_t lambda_list) {
    ls_value_t gv = ls_make_obj(L, LS_T_GENERIC, sizeof(ls_generic_t));
    ls_generic_t *gf = (ls_generic_t *)gv.u.ptr;
    gf->name = intern_cl(L, name);
    gf->lambda_list = lambda_list;
    gf->methods = ls_nil_v();
    gf->combination = ls_nil_v(); /* :standard */
    /* Count required parameters (before &optional, &rest, &key). */
    uint32_t n = 0;
    ls_value_t ll = lambda_list;
    while (LS_CONSP(ll)) {
        ls_value_t p = LS_CAR(ll);
        if (LS_IS(p, LS_T_SYMBOL)) {
            ls_symbol_t *ps = ls_symbol_p(p);
            if (ps == L->sym_ampersand_rest ||
                ps == L->sym_ampersand_optional ||
                ps == L->sym_ampersand_key)
                break;
        }
        n++;
        ll = LS_CDR(ll);
    }
    gf->n_required = n;

    /* Bind to symbol's function slot. */
    gf->name->function = gv;
    gf->name->sym_flags |= LS_SYM_HAS_FN;
    return gf;
}

/* Add a method to a generic function. */
ls_method_t *ls_add_method(ls_state_t *L, ls_generic_t *gf,
                           ls_value_t specializers, ls_value_t qualifiers,
                           ls_value_t fn) {
    ls_value_t mv = ls_make_obj(L, LS_T_METHOD, sizeof(ls_method_t));
    ls_method_t *m = (ls_method_t *)mv.u.ptr;
    m->specializers = specializers;
    m->qualifiers = qualifiers;
    m->lambda = fn;
    m->generic = ls_wrap(LS_T_GENERIC, gf);

    /* Remove any existing method with identical specializers and qualifiers. */
    ls_value_t prev = ls_nil_v();
    ls_value_t cur = gf->methods;
    while (LS_CONSP(cur)) {
        ls_method_t *em = (ls_method_t *)LS_CAR(cur).u.ptr;
        /* Compare specializers pairwise. */
        int same = 1;
        ls_value_t sa = em->specializers, sb = specializers;
        while (LS_CONSP(sa) && LS_CONSP(sb)) {
            if (LS_CAR(sa).u.ptr != LS_CAR(sb).u.ptr) { same = 0; break; }
            sa = LS_CDR(sa); sb = LS_CDR(sb);
        }
        if (same && LS_NILP(sa) && LS_NILP(sb)) {
            /* Also check qualifiers match. */
            ls_value_t qa = em->qualifiers, qb = qualifiers;
            while (LS_CONSP(qa) && LS_CONSP(qb)) {
                if (LS_CAR(qa).u.ptr != LS_CAR(qb).u.ptr) { same = 0; break; }
                qa = LS_CDR(qa); qb = LS_CDR(qb);
            }
            if (same && LS_NILP(qa) && LS_NILP(qb)) {
                /* Replace: splice out old method. */
                if (LS_CONSP(prev))
                    ((ls_cons_t *)prev.u.ptr)->cdr = LS_CDR(cur);
                else
                    gf->methods = LS_CDR(cur);
                break;
            }
        }
        prev = cur;
        cur = LS_CDR(cur);
    }

    /* Prepend new method. */
    gf->methods = ls_cons(L, mv, gf->methods);
    return m;
}

/* ============================================================
 *  METHOD DISPATCH
 * ============================================================ */

/* Check if class A is a subtype of class B (B appears in A's CPL). */
static int subclassp(ls_class_t *a, ls_class_t *b) {
    if (a == b) return 1;
    ls_value_t cpl = a->precedence_list;
    while (LS_CONSP(cpl)) {
        if (LS_CAR(cpl).u.ptr == (void *)b) return 1;
        cpl = LS_CDR(cpl);
    }
    return 0;
}

/* Check if a single method is applicable to the given arguments. */
static int method_applicable(ls_state_t *L, ls_method_t *m,
                             int nargs, ls_value_t *args) {
    ls_value_t specs = m->specializers;
    for (int i = 0; i < nargs && LS_CONSP(specs); i++) {
        ls_class_t *spec = ls_class_p(LS_CAR(specs));
        ls_value_t arg_class = ls_class_of(L, args[i]);
        ls_class_t *ac = ls_class_p(arg_class);
        if (!subclassp(ac, spec)) return 0;
        specs = LS_CDR(specs);
    }
    return 1;
}

/* Position of class C in the CPL of class CLS. Returns a large number
 * if not found (should not happen for well-formed hierarchies). */
static int cpl_position(ls_class_t *cls, ls_class_t *target) {
    int pos = 0;
    ls_value_t cpl = cls->precedence_list;
    while (LS_CONSP(cpl)) {
        if (LS_CAR(cpl).u.ptr == (void *)target) return pos;
        pos++;
        cpl = LS_CDR(cpl);
    }
    return 99999;
}

/* Compare two methods for specificity given the argument classes.
 * Returns negative if a is more specific, positive if b is, 0 if equal. */
static int method_specificity_cmp(ls_state_t *L, ls_method_t *a, ls_method_t *b,
                                  int nargs, ls_value_t *args) {
    ls_value_t sa = a->specializers, sb = b->specializers;
    for (int i = 0; i < nargs && LS_CONSP(sa) && LS_CONSP(sb); i++) {
        ls_class_t *ca = ls_class_p(LS_CAR(sa));
        ls_class_t *cb = ls_class_p(LS_CAR(sb));
        if (ca != cb) {
            /* The more specific is the one whose specializer appears
             * earlier in the CPL of the argument's actual class. */
            ls_value_t arg_class = ls_class_of(L, args[i]);
            ls_class_t *ac = ls_class_p(arg_class);
            int pa = cpl_position(ac, ca);
            int pb = cpl_position(ac, cb);
            if (pa != pb) return pa - pb;
        }
        sa = LS_CDR(sa);
        sb = LS_CDR(sb);
    }
    return 0;
}

/* Sort applicable methods by specificity (insertion sort -- method
 * lists are typically small). */
static void sort_methods(ls_state_t *L, ls_method_t **arr, int n,
                         int nargs, ls_value_t *args) {
    for (int i = 1; i < n; i++) {
        ls_method_t *key = arr[i];
        int j = i - 1;
        while (j >= 0 && method_specificity_cmp(L, arr[j], key, nargs, args) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/* Filter methods by qualifier (nil for primary, :before, :after, :around). */
static int qualifier_match(ls_method_t *m, ls_value_t qual) {
    if (LS_NILP(qual))
        return LS_NILP(m->qualifiers);
    if (LS_CONSP(m->qualifiers) && LS_CAR(m->qualifiers).u.ptr == qual.u.ptr)
        return 1;
    return 0;
}

/* Context for call-next-method support. */
typedef struct ls_cnm_ctx {
    ls_state_t *L;
    ls_generic_t *gf;
    ls_method_t **methods;
    int n_methods;
    int current;
    int nargs;
    ls_value_t *args;
} ls_cnm_ctx_t;

/* Thread-local (or state-local) CNM context.  We use a simple stack
 * so nested generic calls work correctly. */
#define CNM_STACK_MAX 64
static ls_cnm_ctx_t cnm_stack[CNM_STACK_MAX];
static int cnm_depth = 0;

static ls_value_t call_method_fn(ls_state_t *L, ls_method_t *m,
                                 int nargs, ls_value_t *args) {
    return ls_apply(L, m->lambda, nargs, args);
}

/* Builtin: call-next-method */
static ls_value_t bi_call_next_method(ls_state_t *L, int nargs, ls_value_t *args) {
    (void)nargs; (void)args;
    if (cnm_depth <= 0)
        ls_error(L, "call-next-method called outside method dispatch");
    ls_cnm_ctx_t *ctx = &cnm_stack[cnm_depth - 1];
    ctx->current++;
    if (ctx->current >= ctx->n_methods) {
        ls_error(L, "no next method for generic function %s",
                 ctx->gf->name->name->chars);
    }
    return call_method_fn(L, ctx->methods[ctx->current],
                          ctx->nargs, ctx->args);
}

/* Builtin: next-method-p */
static ls_value_t bi_next_method_p(ls_state_t *L, int nargs, ls_value_t *args) {
    (void)L; (void)nargs; (void)args;
    if (cnm_depth <= 0) return ls_nil_v();
    ls_cnm_ctx_t *ctx = &cnm_stack[cnm_depth - 1];
    return (ctx->current + 1 < ctx->n_methods) ? ls_t_v() : ls_nil_v();
}

/* Main dispatch entry point.  Implements standard method combination:
 *   1. Run :around methods (most to least specific), each can
 *      call-next-method to chain.
 *   2. Run :before methods (most to least specific).
 *   3. Run primary methods (most specific, can call-next-method).
 *   4. Run :after methods (least to most specific).
 *   5. Return value from step 3. */
ls_value_t ls_dispatch_generic(ls_state_t *L, ls_generic_t *gf,
                               int nargs, ls_value_t *args) {
    if ((uint32_t)nargs < gf->n_required) {
        ls_error(L, "generic function %s requires %u args, got %d",
                 gf->name->name->chars, gf->n_required, nargs);
    }

    /* Collect applicable methods. */
    ls_method_t *applicable[256];
    int n_applicable = 0;
    ls_value_t ml = gf->methods;
    while (LS_CONSP(ml) && n_applicable < 256) {
        ls_method_t *m = (ls_method_t *)LS_CAR(ml).u.ptr;
        if (method_applicable(L, m, nargs, args))
            applicable[n_applicable++] = m;
        ml = LS_CDR(ml);
    }

    if (n_applicable == 0) {
        ls_error(L, "no applicable method for generic function %s",
                 gf->name->name->chars);
    }

    /* Sort by specificity. */
    sort_methods(L, applicable, n_applicable, nargs, args);

    /* Partition by qualifier. */
    ls_method_t *around[64]; int n_around = 0;
    ls_method_t *before[64]; int n_before = 0;
    ls_method_t *primary[64]; int n_primary = 0;
    ls_method_t *after[64]; int n_after = 0;

    ls_value_t kw_before = ls_intern(L, "KEYWORD", "BEFORE");
    ls_value_t kw_after = ls_intern(L, "KEYWORD", "AFTER");
    ls_value_t kw_around = ls_intern(L, "KEYWORD", "AROUND");

    for (int i = 0; i < n_applicable; i++) {
        ls_method_t *m = applicable[i];
        if (qualifier_match(m, ls_nil_v())) {
            if (n_primary < 64) primary[n_primary++] = m;
        } else if (qualifier_match(m, kw_before)) {
            if (n_before < 64) before[n_before++] = m;
        } else if (qualifier_match(m, kw_after)) {
            if (n_after < 64) after[n_after++] = m;
        } else if (qualifier_match(m, kw_around)) {
            if (n_around < 64) around[n_around++] = m;
        }
    }

    if (n_primary == 0) {
        ls_error(L, "no primary method for generic function %s",
                 gf->name->name->chars);
    }

    /* Push CNM context for primary methods (or around if present). */
    if (cnm_depth >= CNM_STACK_MAX)
        ls_error(L, "generic function call stack overflow");

    ls_value_t result;

    if (n_around > 0) {
        /* Around methods: the outermost around method wraps everything.
         * When it calls call-next-method, it invokes the next around
         * method, or if no more around methods, runs the standard
         * before/primary/after sequence. */

        /* Build a combined chain: around methods + a synthetic entry
         * that runs before/primary/after.  For simplicity, we set up
         * around methods in the CNM stack and handle the transition
         * to the effective method ourselves. */
        ls_cnm_ctx_t *ctx = &cnm_stack[cnm_depth++];
        ctx->L = L;
        ctx->gf = gf;
        ctx->methods = around;
        ctx->n_methods = n_around;
        ctx->current = 0;
        ctx->nargs = nargs;
        ctx->args = args;
        result = call_method_fn(L, around[0], nargs, args);
        cnm_depth--;
    } else {
        /* Run :before methods (most to least specific). */
        for (int i = 0; i < n_before; i++)
            call_method_fn(L, before[i], nargs, args);

        /* Run primary methods with call-next-method support. */
        ls_cnm_ctx_t *ctx = &cnm_stack[cnm_depth++];
        ctx->L = L;
        ctx->gf = gf;
        ctx->methods = primary;
        ctx->n_methods = n_primary;
        ctx->current = 0;
        ctx->nargs = nargs;
        ctx->args = args;

        result = call_method_fn(L, primary[0], nargs, args);
        cnm_depth--;

        /* Run :after methods (least to most specific = reverse order). */
        for (int i = n_after - 1; i >= 0; i--)
            call_method_fn(L, after[i], nargs, args);
    }

    return result;
}

/* ============================================================
 *  CONDITION SYSTEM  --  signal, handler-bind, handler-case,
 *  restart-case, restart-bind, invoke-restart, find-restart
 * ============================================================ */

/* Create a condition object. */
ls_value_t ls_make_condition(ls_state_t *L, ls_class_t *cls,
                             ls_value_t initargs) {
    ls_value_t cv = ls_make_obj(L, LS_T_CONDITION, sizeof(ls_condition_t));
    ls_condition_t *c = (ls_condition_t *)cv.u.ptr;
    c->class_ = cls;
    c->initargs = initargs;
    c->format_control = ls_nil_v();
    c->format_arguments = ls_nil_v();

    /* Extract :format-control and :format-arguments from initargs. */
    ls_value_t ia = initargs;
    ls_symbol_t *kw_fc = intern_kw(L, "FORMAT-CONTROL");
    ls_symbol_t *kw_fa = intern_kw(L, "FORMAT-ARGUMENTS");
    while (LS_CONSP(ia) && LS_CONSP(LS_CDR(ia))) {
        ls_symbol_t *k = ls_symbol_p(LS_CAR(ia));
        ls_value_t v = LS_CAR(LS_CDR(ia));
        if (k == kw_fc) c->format_control = v;
        else if (k == kw_fa) c->format_arguments = v;
        ia = LS_CDR(LS_CDR(ia));
    }
    return cv;
}

/* Signal a condition: walk the handler stack from top to bottom.
 * If a handler matches (its type is a superclass of the condition's
 * class), call it.  The handler can:
 *   - return normally (handler-bind semantics: keep searching)
 *   - do a non-local exit (handler-case semantics: unwind)
 *
 * Returns 1 if handled (non-locally), 0 if all handlers declined. */
int ls_signal_condition(ls_state_t *L, ls_value_t condition) {
    ls_condition_t *cond = (ls_condition_t *)condition.u.ptr;
    ls_class_t *cond_class = cond->class_;

    ls_handler_t *h = L->hnd_top;
    while (h) {
        if (subclassp(cond_class, h->type)) {
            /* Call the handler function with the condition. */
            ls_value_t hfn = h->fn;
            ls_value_t carg = condition;
            ls_apply(L, hfn, 1, &carg);
            /* If handler returned normally (handler-bind semantics),
             * continue searching. */
        }
        h = h->next;
    }
    return 0;  /* no handler performed a non-local exit */
}

/* Push a handler frame onto the handler stack. */
void ls_push_handler(ls_state_t *L, ls_class_t *type, ls_value_t fn) {
    ls_handler_t *h = (ls_handler_t *)malloc(sizeof(ls_handler_t));
    h->type = type;
    h->fn = fn;
    h->next = L->hnd_top;
    L->hnd_top = h;
}

/* Pop a handler frame. */
void ls_pop_handler(ls_state_t *L) {
    if (L->hnd_top) {
        ls_handler_t *h = L->hnd_top;
        L->hnd_top = h->next;
        free(h);
    }
}

/* Create a restart object. */
static ls_restart_t *make_restart(ls_state_t *L, const char *name,
                                  ls_value_t invoke_fn) {
    ls_value_t rv = ls_make_obj(L, LS_T_RESTART, sizeof(ls_restart_t));
    ls_restart_t *r = (ls_restart_t *)rv.u.ptr;
    r->name = name ? intern_cl(L, name) : NULL;
    r->invoke_fn = invoke_fn;
    r->test_fn = ls_nil_v();
    r->report_fn = ls_nil_v();
    r->interactive_fn = ls_nil_v();
    r->active = 1;
    return r;
}

/* Push a restart frame. */
void ls_push_restart(ls_state_t *L, ls_restart_t *r) {
    ls_restart_frame_t *f = (ls_restart_frame_t *)malloc(sizeof(ls_restart_frame_t));
    f->restart = r;
    f->next = L->restart_top;
    L->restart_top = f;
}

/* Pop a restart frame. */
void ls_pop_restart(ls_state_t *L) {
    if (L->restart_top) {
        ls_restart_frame_t *f = L->restart_top;
        L->restart_top = f->next;
        free(f);
    }
}

/* Find a restart by name.  Returns NULL if not found. */
ls_restart_t *ls_find_restart(ls_state_t *L, ls_symbol_t *name) {
    ls_restart_frame_t *f = L->restart_top;
    while (f) {
        if (f->restart->active && f->restart->name == name)
            return f->restart;
        f = f->next;
    }
    return NULL;
}

/* Invoke a restart.  Calls the restart's invoke-fn with the given args. */
ls_value_t ls_invoke_restart(ls_state_t *L, ls_restart_t *r,
                             int nargs, ls_value_t *args) {
    if (!r->active)
        ls_error(L, "attempt to invoke inactive restart");
    return ls_apply(L, r->invoke_fn, nargs, args);
}

/* Collect all active restarts into a list. */
ls_value_t ls_compute_restarts(ls_state_t *L) {
    ls_value_t result = ls_nil_v();
    ls_gc_push_root(L, &result);
    ls_restart_frame_t *f = L->restart_top;
    while (f) {
        if (f->restart->active)
            result = ls_cons(L, ls_wrap(LS_T_RESTART, f->restart), result);
        f = f->next;
    }
    ls_gc_pop_root(L);
    return ls_list_reverse(L, result);
}

/* ============================================================
 *  LISP-CALLABLE BUILTINS
 * ============================================================ */

/* (class-of obj) => class */
static ls_value_t bi_class_of(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "class-of", nargs, 1, 1); return ls_nil_v(); }
    return ls_class_of(L, A(0));
}

/* (find-class 'name) => class */
static ls_value_t bi_find_class(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "find-class", nargs, 1, 1); return ls_nil_v(); }
    ls_symbol_t *sym = ls_symbol_p(A(0));
    if (!sym) { ls_type_error(L, "symbol", A(0)); return ls_nil_v(); }
    if (sym->sym_flags & LS_SYM_HAS_VALUE) {
        ls_value_t v = sym->value;
        if (LS_IS(v, LS_T_CLASS)) return v;
    }
    ls_error(L, "no class named %s", sym->name->chars);
    return ls_nil_v();
}

/* (make-instance 'class-name &rest initargs) */
static ls_value_t bi_make_instance(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "make-instance", nargs, 1, -1); return ls_nil_v(); }
    ls_class_t *cls = NULL;
    if (LS_IS(A(0), LS_T_SYMBOL)) {
        ls_symbol_t *sym = ls_symbol_p(A(0));
        if (sym->sym_flags & LS_SYM_HAS_VALUE) {
            ls_value_t v = sym->value;
            if (LS_IS(v, LS_T_CLASS))
                cls = ls_class_p(v);
        }
        if (!cls)
            ls_error(L, "make-instance: %s does not name a class", sym->name->chars);
    } else if (LS_IS(A(0), LS_T_CLASS)) {
        cls = ls_class_p(A(0));
    } else {
        ls_type_error(L, "class or symbol", A(0));
        return ls_nil_v();
    }
    return make_instance(L, cls, nargs - 1, args + 1);
}

/* (slot-value instance slot-name) */
static ls_value_t bi_slot_value(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "slot-value", nargs, 2, 2); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_INSTANCE)) { ls_type_error(L, "instance", A(0)); return ls_nil_v(); }
    ls_instance_t *inst = ls_instance_p(A(0));
    return ls_slot_value(L, inst, A(1));
}

/* (set-slot-value instance slot-name value) -- backing for (setf slot-value) */
static ls_value_t bi_set_slot_value(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 3) { ls_arity_error(L, "set-slot-value", nargs, 3, 3); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_INSTANCE)) { ls_type_error(L, "instance", A(0)); return ls_nil_v(); }
    ls_instance_t *inst = ls_instance_p(A(0));
    ls_slot_set(L, inst, A(1), A(2));
    return A(2);
}

/* (slot-boundp instance slot-name) */
static ls_value_t bi_slot_boundp(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "slot-boundp", nargs, 2, 2); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_INSTANCE)) { ls_type_error(L, "instance", A(0)); return ls_nil_v(); }
    ls_instance_t *inst = ls_instance_p(A(0));
    int idx = slot_index_of(inst->class_, A(1));
    if (idx < 0) return ls_nil_v();
    /* We use nil as the unbound marker, which is admittedly a simplification. */
    return LS_NILP(inst->slots[idx]) ? ls_nil_v() : ls_t_v();
}

/* (typep obj type-name) */
static ls_value_t bi_typep(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "typep", nargs, 2, 2); return ls_nil_v(); }
    ls_class_t *type_class = NULL;
    if (LS_IS(A(1), LS_T_CLASS)) {
        type_class = ls_class_p(A(1));
    } else if (LS_IS(A(1), LS_T_SYMBOL)) {
        ls_symbol_t *sym = ls_symbol_p(A(1));
        if (sym->sym_flags & LS_SYM_HAS_VALUE) {
            ls_value_t cv = sym->value;
            if (LS_IS(cv, LS_T_CLASS)) type_class = ls_class_p(cv);
        }
    }
    if (!type_class) return ls_nil_v();

    ls_value_t oc = ls_class_of(L, A(0));
    ls_class_t *obj_class = ls_class_p(oc);
    return subclassp(obj_class, type_class) ? ls_t_v() : ls_nil_v();
}

/* (subtypep type1 type2) */
static ls_value_t bi_subtypep(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "subtypep", nargs, 2, 2); return ls_nil_v(); }
    ls_class_t *a = NULL, *b = NULL;
    if (LS_IS(A(0), LS_T_CLASS)) a = ls_class_p(A(0));
    else if (LS_IS(A(0), LS_T_SYMBOL)) {
        ls_symbol_t *s = ls_symbol_p(A(0));
        if (s->sym_flags & LS_SYM_HAS_VALUE && LS_IS(s->value, LS_T_CLASS))
            a = ls_class_p(s->value);
    }
    if (LS_IS(A(1), LS_T_CLASS)) b = ls_class_p(A(1));
    else if (LS_IS(A(1), LS_T_SYMBOL)) {
        ls_symbol_t *s = ls_symbol_p(A(1));
        if (s->sym_flags & LS_SYM_HAS_VALUE && LS_IS(s->value, LS_T_CLASS))
            b = ls_class_p(s->value);
    }
    if (!a || !b) return ls_nil_v();
    return subclassp(a, b) ? ls_t_v() : ls_nil_v();
}

/* (class-name class) */
static ls_value_t bi_class_name(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "class-name", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_CLASS)) { ls_type_error(L, "class", A(0)); return ls_nil_v(); }
    ls_class_t *c = ls_class_p(A(0));
    return ls_wrap(LS_T_SYMBOL, c->name);
}

/* (class-precedence-list class) */
static ls_value_t bi_class_cpl(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "class-precedence-list", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_CLASS)) { ls_type_error(L, "class", A(0)); return ls_nil_v(); }
    ls_class_t *c = ls_class_p(A(0));
    return c->precedence_list;
}

/* (class-direct-superclasses class) */
static ls_value_t bi_class_direct_supers(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "class-direct-superclasses", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_CLASS)) { ls_type_error(L, "class", A(0)); return ls_nil_v(); }
    return ls_class_p(A(0))->direct_supers;
}

/* (class-slots class) */
static ls_value_t bi_class_slots(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "class-slots", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_CLASS)) { ls_type_error(L, "class", A(0)); return ls_nil_v(); }
    return ls_class_p(A(0))->effective_slots;
}

/* (signal condition) or (signal 'type :key val ...) */
static ls_value_t bi_signal(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "signal", nargs, 1, -1); return ls_nil_v(); }
    ls_value_t cond;
    if (LS_IS(A(0), LS_T_CONDITION)) {
        cond = A(0);
    } else if (LS_IS(A(0), LS_T_SYMBOL) || LS_IS(A(0), LS_T_CLASS)) {
        /* Make a condition from a type specifier and initargs. */
        ls_class_t *cls = NULL;
        if (LS_IS(A(0), LS_T_CLASS)) {
            cls = ls_class_p(A(0));
        } else {
            ls_symbol_t *sym = ls_symbol_p(A(0));
            if (sym->sym_flags & LS_SYM_HAS_VALUE && LS_IS(sym->value, LS_T_CLASS))
                cls = ls_class_p(sym->value);
        }
        if (!cls) cls = L->class_simple_error;
        /* Build initargs plist from remaining args. */
        ls_value_t ia = ls_nil_v();
        ls_gc_push_root(L, &ia);
        for (int i = nargs - 1; i >= 1; i--)
            ia = ls_cons(L, args[i], ia);
        ls_gc_pop_root(L);
        cond = ls_make_condition(L, cls, ia);
    } else {
        /* String message: wrap in simple-error. */
        ls_value_t ia = ls_nil_v();
        ls_gc_push_root(L, &ia);
        ls_value_t kfc = ls_intern(L, "KEYWORD", "FORMAT-CONTROL");
        ia = ls_cons(L, A(0), ls_nil_v());
        ia = ls_cons(L, kfc, ia);
        ls_gc_pop_root(L);
        cond = ls_make_condition(L, L->class_simple_error, ia);
    }
    ls_signal_condition(L, cond);
    return ls_nil_v();
}

/* (error ...) -- like signal but enters the debugger / calls ls_error
 * if no handler takes a non-local exit. */
static ls_value_t bi_cerror(ls_state_t *L, int nargs, ls_value_t *args) {
    /* First try signaling like signal does. */
    bi_signal(L, nargs, args);
    /* If we get here, no handler handled it. */
    if (nargs >= 1 && LS_IS(A(0), LS_T_STRING)) {
        ls_string_t *s = ls_string_p(A(0));
        ls_error(L, "%s", s->chars);
    } else {
        ls_error(L, "unhandled error condition");
    }
    return ls_nil_v();
}

/* (handler-bind ((type handler-fn) ...) &body body)
 * This is a special form; the builtin version takes a handler alist
 * and a thunk:
 * (%handler-bind handlers-list body-thunk) */
static ls_value_t bi_handler_bind(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "%handler-bind", nargs, 2, 2); return ls_nil_v(); }
    ls_value_t handlers = A(0);
    ls_value_t body_thunk = A(1);

    /* Push handlers. */
    int n_pushed = 0;
    ls_value_t hl = handlers;
    while (LS_CONSP(hl)) {
        ls_value_t entry = LS_CAR(hl);
        /* entry is (type-class . handler-fn) */
        if (LS_CONSP(entry)) {
            ls_class_t *type_cls = NULL;
            ls_value_t type_v = LS_CAR(entry);
            if (LS_IS(type_v, LS_T_CLASS))
                type_cls = ls_class_p(type_v);
            else if (LS_IS(type_v, LS_T_SYMBOL)) {
                ls_symbol_t *s = ls_symbol_p(type_v);
                if (s->sym_flags & LS_SYM_HAS_VALUE && LS_IS(s->value, LS_T_CLASS))
                    type_cls = ls_class_p(s->value);
            }
            if (type_cls) {
                ls_push_handler(L, type_cls, LS_CDR(entry));
                n_pushed++;
            }
        }
        hl = LS_CDR(hl);
    }

    /* Call body thunk. */
    ls_value_t result = ls_apply(L, body_thunk, 0, NULL);

    /* Pop handlers. */
    for (int i = 0; i < n_pushed; i++)
        ls_pop_handler(L);

    return result;
}

/* (%handler-case type-class body-thunk handler-fn)
 * Runs body-thunk; if a condition of type-class is signaled,
 * unwinds and calls handler-fn with the condition. */
static ls_value_t bi_handler_case(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 3) { ls_arity_error(L, "%handler-case", nargs, 3, 3); return ls_nil_v(); }

    ls_class_t *type_cls = NULL;
    if (LS_IS(A(0), LS_T_CLASS))
        type_cls = ls_class_p(A(0));
    else if (LS_IS(A(0), LS_T_SYMBOL)) {
        ls_symbol_t *s = ls_symbol_p(A(0));
        if (s->sym_flags & LS_SYM_HAS_VALUE && LS_IS(s->value, LS_T_CLASS))
            type_cls = ls_class_p(s->value);
    }
    if (!type_cls) { ls_error(L, "handler-case: invalid type specifier"); return ls_nil_v(); }

    ls_value_t body_thunk = A(1);
    ls_value_t handler_fn = A(2);

    /* Set up an escape frame for the non-local exit. */
    ls_escape_t esc;
    esc.kind = 2;  /* condition */
    esc.tag = ls_nil_v();
    esc.value = ls_nil_v();
    esc.next = L->esc_top;
    L->esc_top = &esc;

    if (setjmp(esc.buf) == 0) {
        /* Install a handler that performs a non-local exit. */
        /* We create a tiny native function wrapper that longjmps.
         * Instead, we use the escape frame: push a handler whose fn
         * stores the condition in esc.value and longjmps. */

        /* We cannot create a closure here easily, so we use a
         * handler-bind approach: install a handler, and if it fires,
         * we store the condition and longjmp. */

        /* Push the handler with a special sentinel function.
         * When the signal code calls this handler, we do the longjmp. */
        ls_handler_t hframe;
        hframe.type = type_cls;
        hframe.fn = ls_nil_v();  /* sentinel: will be checked below */
        hframe.next = L->hnd_top;
        L->hnd_top = &hframe;

        /* We override ls_signal_condition behavior by checking for
         * handler_case-style handlers.  But since we control the
         * stack, we do this directly: replace the signal call path
         * with a setjmp-based approach. */

        /* Actually, let's use a simpler approach: install a real handler
         * that performs a non-local exit via longjmp. */
        L->hnd_top = hframe.next;  /* undo */

        /* Use a condition-catching approach: wrap the body with
         * our own signal intercept. */

        /* Strategy: we temporarily set up a handler that stores the
         * condition in the escape frame and longjmps. We implement
         * this by directly manipulating the handler stack with a
         * custom handler that has a special pointer we recognize. */

        /* Simplest correct approach for a minimal CLOS: use the native
         * handler stack. The handler function will be a special value
         * that our signal machinery recognizes as "longjmp to escape". */

        /* For the minimal implementation, we use the escape frame's
         * tag to hold the type_cls pointer and the signal function
         * checks for handler_case escape frames first. */
        esc.tag = ls_wrap(LS_T_CLASS, type_cls);

        ls_value_t result = ls_apply(L, body_thunk, 0, NULL);
        L->esc_top = esc.next;
        return result;
    } else {
        /* Landed here via longjmp -- the condition is in esc.value. */
        L->esc_top = esc.next;
        /* Call the handler function with the condition. */
        ls_value_t cond_val = esc.value;
        return ls_apply(L, handler_fn, 1, &cond_val);
    }
}

/* Enhanced signal that checks for handler-case escape frames. */
int ls_signal_condition_full(ls_state_t *L, ls_value_t condition) {
    ls_condition_t *cond = (ls_condition_t *)condition.u.ptr;
    ls_class_t *cond_class = cond->class_;

    /* First check for handler-case escape frames. */
    ls_escape_t *esc = L->esc_top;
    while (esc) {
        if (esc->kind == 2 && LS_IS(esc->tag, LS_T_CLASS)) {
            ls_class_t *handler_type = ls_class_p(esc->tag);
            if (subclassp(cond_class, handler_type)) {
                esc->value = condition;
                longjmp(esc->buf, 1);
            }
        }
        esc = esc->next;
    }

    /* Then walk the handler-bind stack. */
    ls_handler_t *h = L->hnd_top;
    while (h) {
        if (subclassp(cond_class, h->type)) {
            ls_value_t hfn = h->fn;
            if (!LS_NILP(hfn)) {
                ls_value_t carg = condition;
                ls_apply(L, hfn, 1, &carg);
            }
        }
        h = h->next;
    }
    return 0;
}

/* (%restart-case restart-specs body-thunk)
 * restart-specs is a list of (name invoke-fn &key test report interactive).
 * Establishes restarts, runs body-thunk, then tears down restarts. */
static ls_value_t bi_restart_case(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "%restart-case", nargs, 2, 2); return ls_nil_v(); }
    ls_value_t specs = A(0);
    ls_value_t body_thunk = A(1);

    /* Push restarts. */
    int n_pushed = 0;
    ls_value_t sl = specs;
    while (LS_CONSP(sl)) {
        ls_value_t spec = LS_CAR(sl);
        if (LS_CONSP(spec)) {
            ls_value_t name_v = LS_CAR(spec);
            ls_value_t invoke_fn = LS_CONSP(LS_CDR(spec)) ? LS_CAR(LS_CDR(spec)) : ls_nil_v();
            const char *rname = NULL;
            if (LS_IS(name_v, LS_T_SYMBOL))
                rname = ls_symbol_p(name_v)->name->chars;
            ls_restart_t *r = make_restart(L, rname, invoke_fn);

            /* Parse optional :test, :report, :interactive from rest. */
            ls_value_t rest = LS_CONSP(LS_CDR(spec)) ? LS_CDR(LS_CDR(spec)) : ls_nil_v();
            while (LS_CONSP(rest) && LS_CONSP(LS_CDR(rest))) {
                ls_value_t key = LS_CAR(rest);
                ls_value_t val = LS_CAR(LS_CDR(rest));
                if (LS_IS(key, LS_T_SYMBOL)) {
                    ls_symbol_t *ks = ls_symbol_p(key);
                    if (ks == intern_kw(L, "TEST")) r->test_fn = val;
                    else if (ks == intern_kw(L, "REPORT")) r->report_fn = val;
                    else if (ks == intern_kw(L, "INTERACTIVE")) r->interactive_fn = val;
                }
                rest = LS_CDR(LS_CDR(rest));
            }
            ls_push_restart(L, r);
            n_pushed++;
        }
        sl = LS_CDR(sl);
    }

    /* Set up escape frame so that invoke-restart can longjmp back here. */
    ls_escape_t esc;
    esc.kind = 2;
    esc.tag = ls_nil_v();  /* no class tag -- this is for restarts */
    esc.value = ls_nil_v();
    esc.next = L->esc_top;
    L->esc_top = &esc;

    ls_value_t result;
    if (setjmp(esc.buf) == 0) {
        result = ls_apply(L, body_thunk, 0, NULL);
    } else {
        result = esc.value;
    }

    L->esc_top = esc.next;
    for (int i = 0; i < n_pushed; i++)
        ls_pop_restart(L);
    return result;
}

/* (%restart-bind restart-specs body-thunk) -- like restart-case but
 * does not establish a non-local exit boundary. */
static ls_value_t bi_restart_bind(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "%restart-bind", nargs, 2, 2); return ls_nil_v(); }
    ls_value_t specs = A(0);
    ls_value_t body_thunk = A(1);

    int n_pushed = 0;
    ls_value_t sl = specs;
    while (LS_CONSP(sl)) {
        ls_value_t spec = LS_CAR(sl);
        if (LS_CONSP(spec)) {
            ls_value_t name_v = LS_CAR(spec);
            ls_value_t invoke_fn = LS_CONSP(LS_CDR(spec)) ? LS_CAR(LS_CDR(spec)) : ls_nil_v();
            const char *rname = NULL;
            if (LS_IS(name_v, LS_T_SYMBOL))
                rname = ls_symbol_p(name_v)->name->chars;
            ls_restart_t *r = make_restart(L, rname, invoke_fn);
            ls_push_restart(L, r);
            n_pushed++;
        }
        sl = LS_CDR(sl);
    }

    ls_value_t result = ls_apply(L, body_thunk, 0, NULL);

    for (int i = 0; i < n_pushed; i++)
        ls_pop_restart(L);
    return result;
}

/* (find-restart name) */
static ls_value_t bi_find_restart(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "find-restart", nargs, 1, 1); return ls_nil_v(); }
    ls_symbol_t *name = ls_symbol_p(A(0));
    if (!name) { ls_type_error(L, "symbol", A(0)); return ls_nil_v(); }
    ls_restart_t *r = ls_find_restart(L, name);
    if (!r) return ls_nil_v();
    return ls_wrap(LS_T_RESTART, r);
}

/* (invoke-restart name &rest args) */
static ls_value_t bi_invoke_restart(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "invoke-restart", nargs, 1, -1); return ls_nil_v(); }
    ls_restart_t *r = NULL;
    if (LS_IS(A(0), LS_T_RESTART)) {
        r = (ls_restart_t *)A(0).u.ptr;
    } else if (LS_IS(A(0), LS_T_SYMBOL)) {
        r = ls_find_restart(L, ls_symbol_p(A(0)));
    }
    if (!r) {
        ls_error(L, "invoke-restart: restart not found");
        return ls_nil_v();
    }
    return ls_invoke_restart(L, r, nargs - 1, args + 1);
}

/* (compute-restarts) */
static ls_value_t bi_compute_restarts(ls_state_t *L, int nargs, ls_value_t *args) {
    (void)nargs; (void)args;
    return ls_compute_restarts(L);
}

/* (restart-name restart) */
static ls_value_t bi_restart_name(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "restart-name", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_RESTART)) { ls_type_error(L, "restart", A(0)); return ls_nil_v(); }
    ls_restart_t *r = (ls_restart_t *)A(0).u.ptr;
    if (r->name) return ls_wrap(LS_T_SYMBOL, r->name);
    return ls_nil_v();
}

/* (make-condition type &rest initargs) */
static ls_value_t bi_make_condition(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "make-condition", nargs, 1, -1); return ls_nil_v(); }
    ls_class_t *cls = NULL;
    if (LS_IS(A(0), LS_T_CLASS)) cls = ls_class_p(A(0));
    else if (LS_IS(A(0), LS_T_SYMBOL)) {
        ls_symbol_t *s = ls_symbol_p(A(0));
        if (s->sym_flags & LS_SYM_HAS_VALUE && LS_IS(s->value, LS_T_CLASS))
            cls = ls_class_p(s->value);
    }
    if (!cls) { ls_error(L, "make-condition: not a condition class"); return ls_nil_v(); }

    ls_value_t ia = ls_nil_v();
    ls_gc_push_root(L, &ia);
    for (int i = nargs - 1; i >= 1; i--)
        ia = ls_cons(L, args[i], ia);
    ls_gc_pop_root(L);
    return ls_make_condition(L, cls, ia);
}

/* (condition-format-control cond) */
static ls_value_t bi_condition_format_control(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "condition-format-control", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_CONDITION)) { ls_type_error(L, "condition", A(0)); return ls_nil_v(); }
    ls_condition_t *c = (ls_condition_t *)A(0).u.ptr;
    return c->format_control;
}

/* (condition-format-arguments cond) */
static ls_value_t bi_condition_format_args(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "condition-format-arguments", nargs, 1, 1); return ls_nil_v(); }
    if (!LS_IS(A(0), LS_T_CONDITION)) { ls_type_error(L, "condition", A(0)); return ls_nil_v(); }
    ls_condition_t *c = (ls_condition_t *)A(0).u.ptr;
    return c->format_arguments;
}

/* (%defclass name supers slots) -- C-level defclass.
 * supers is a list of class-name symbols, slots is a list of slot-name symbols. */
static ls_value_t bi_defclass(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 3) { ls_arity_error(L, "%defclass", nargs, 3, 3); return ls_nil_v(); }
    ls_symbol_t *name_sym = ls_symbol_p(A(0));
    if (!name_sym) { ls_type_error(L, "symbol", A(0)); return ls_nil_v(); }

    /* Resolve super class symbols to class objects. */
    ls_value_t supers = ls_nil_v();
    ls_gc_push_root(L, &supers);
    ls_value_t sl = A(1);
    while (LS_CONSP(sl)) {
        ls_symbol_t *ss = ls_symbol_p(LS_CAR(sl));
        if (!ss) { ls_gc_pop_root(L); ls_type_error(L, "symbol", LS_CAR(sl)); return ls_nil_v(); }
        if (!(ss->sym_flags & LS_SYM_HAS_VALUE) || !LS_IS(ss->value, LS_T_CLASS)) {
            ls_gc_pop_root(L);
            ls_error(L, "defclass: %s is not a class", ss->name->chars);
            return ls_nil_v();
        }
        supers = ls_cons(L, ss->value, supers);
        sl = LS_CDR(sl);
    }
    supers = ls_list_reverse(L, supers);

    /* If no supers given, default to standard-object. */
    if (LS_NILP(supers) && L->class_standard_object)
        supers = list1(L, ls_wrap(LS_T_CLASS, L->class_standard_object));

    ls_class_t *cls = ls_define_class(L, name_sym->name->chars, supers, A(2));
    ls_gc_pop_root(L);
    return ls_wrap(LS_T_CLASS, cls);
}

/* (%defgeneric name lambda-list) */
static ls_value_t bi_defgeneric(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) { ls_arity_error(L, "%defgeneric", nargs, 2, 2); return ls_nil_v(); }
    ls_symbol_t *name_sym = ls_symbol_p(A(0));
    if (!name_sym) { ls_type_error(L, "symbol", A(0)); return ls_nil_v(); }
    ls_generic_t *gf = ls_define_generic(L, name_sym->name->chars, A(1));
    return ls_wrap(LS_T_GENERIC, gf);
}

/* (%defmethod generic-name specializers qualifiers lambda) */
static ls_value_t bi_defmethod(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 4) { ls_arity_error(L, "%defmethod", nargs, 4, 4); return ls_nil_v(); }

    /* Find or error on the generic function. */
    ls_generic_t *gf = NULL;
    if (LS_IS(A(0), LS_T_GENERIC)) {
        gf = (ls_generic_t *)A(0).u.ptr;
    } else if (LS_IS(A(0), LS_T_SYMBOL)) {
        ls_symbol_t *gs = ls_symbol_p(A(0));
        if (gs->sym_flags & LS_SYM_HAS_FN && LS_IS(gs->function, LS_T_GENERIC))
            gf = (ls_generic_t *)gs->function.u.ptr;
    }
    if (!gf) { ls_error(L, "defmethod: no generic function found"); return ls_nil_v(); }

    /* Resolve specializer symbols to classes. */
    ls_value_t specs = ls_nil_v();
    ls_gc_push_root(L, &specs);
    ls_value_t sl = A(1);
    while (LS_CONSP(sl)) {
        ls_value_t sv = LS_CAR(sl);
        ls_class_t *spec = NULL;
        if (LS_IS(sv, LS_T_CLASS)) {
            spec = ls_class_p(sv);
        } else if (LS_IS(sv, LS_T_SYMBOL)) {
            ls_symbol_t *ss = ls_symbol_p(sv);
            if (ss->sym_flags & LS_SYM_HAS_VALUE && LS_IS(ss->value, LS_T_CLASS))
                spec = ls_class_p(ss->value);
            /* T symbol means "any type". */
            if (!spec && ss == L->sym_t)
                spec = L->class_t;
        }
        if (!spec) {
            ls_gc_pop_root(L);
            ls_error(L, "defmethod: invalid specializer");
            return ls_nil_v();
        }
        specs = ls_cons(L, ls_wrap(LS_T_CLASS, spec), specs);
        sl = LS_CDR(sl);
    }
    specs = ls_list_reverse(L, specs);
    ls_gc_pop_root(L);

    ls_method_t *m = ls_add_method(L, gf, specs, A(2), A(3));
    return ls_wrap(LS_T_METHOD, m);
}

/* (%call-generic gf &rest args) -- explicit generic dispatch. */
static ls_value_t bi_call_generic(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) { ls_arity_error(L, "%call-generic", nargs, 1, -1); return ls_nil_v(); }
    ls_generic_t *gf = NULL;
    if (LS_IS(A(0), LS_T_GENERIC)) gf = (ls_generic_t *)A(0).u.ptr;
    else if (LS_IS(A(0), LS_T_SYMBOL)) {
        ls_symbol_t *s = ls_symbol_p(A(0));
        if (s->sym_flags & LS_SYM_HAS_FN && LS_IS(s->function, LS_T_GENERIC))
            gf = (ls_generic_t *)s->function.u.ptr;
    }
    if (!gf) { ls_error(L, "not a generic function"); return ls_nil_v(); }
    return ls_dispatch_generic(L, gf, nargs - 1, args + 1);
}

/* ============================================================
 *  BUILT-IN CLASS HIERARCHY INITIALIZATION
 * ============================================================ */

/* Helper: create a builtin class with given supers, no user slots.
 * The class is finalized and bound to its symbol. */
static ls_class_t *bootstrap_class(ls_state_t *L, const char *name,
                                   ls_value_t supers) {
    ls_class_t *cls = make_class(L, name, supers, ls_nil_v());
    /* Manually set CPL for bootstrap classes that have no supers yet.
     * For T, which has no supers, CPL is just (T). */
    if (LS_NILP(supers)) {
        cls->precedence_list = list1(L, ls_wrap(LS_T_CLASS, cls));
        cls->effective_slots = ls_nil_v();
        cls->n_slots = 0;
        cls->instance_size = (uint32_t)sizeof(ls_instance_t);
        cls->slot_index = ls_hash_new(L, LS_HASH_EQ, 4);
        cls->finalized = 1;
    } else {
        finalize_class(L, cls);
    }
    /* Bind to symbol. */
    cls->name->value = ls_wrap(LS_T_CLASS, cls);
    cls->name->sym_flags |= LS_SYM_HAS_VALUE;
    return cls;
}

/* Create the entire built-in class hierarchy and register all CLOS
 * and condition system builtins. */
void ls_init_clos(ls_state_t *L) {
    /* ---- Phase 1: bootstrap the root classes ---- */

    /* T is the root of the type hierarchy. */
    L->class_t = bootstrap_class(L, "T", ls_nil_v());

    ls_value_t t_list = list1(L, ls_wrap(LS_T_CLASS, L->class_t));
    ls_gc_push_root(L, &t_list);

    /* STANDARD-OBJECT inherits from T. */
    L->class_standard_object = bootstrap_class(L, "STANDARD-OBJECT", t_list);

    ls_value_t so_list = list1(L, ls_wrap(LS_T_CLASS, L->class_standard_object));
    ls_gc_push_root(L, &so_list);

    /* ---- Phase 2: numeric tower ---- */
    L->class_number = bootstrap_class(L, "NUMBER", t_list);

    ls_value_t num_list = list1(L, ls_wrap(LS_T_CLASS, L->class_number));
    ls_gc_push_root(L, &num_list);

    /* REAL inherits NUMBER.  We don't store this on L-> but it is in
     * the class hierarchy for completeness. */
    ls_class_t *class_real = bootstrap_class(L, "REAL", num_list);

    ls_value_t real_list = list1(L, ls_wrap(LS_T_CLASS, class_real));
    ls_gc_push_root(L, &real_list);

    /* RATIONAL inherits REAL. */
    ls_class_t *class_rational = bootstrap_class(L, "RATIONAL", real_list);

    ls_value_t rat_list = list1(L, ls_wrap(LS_T_CLASS, class_rational));
    ls_gc_push_root(L, &rat_list);

    L->class_integer = bootstrap_class(L, "INTEGER", rat_list);

    ls_value_t int_list = list1(L, ls_wrap(LS_T_CLASS, L->class_integer));
    ls_gc_push_root(L, &int_list);

    L->class_fixnum = bootstrap_class(L, "FIXNUM", int_list);
    L->class_float = bootstrap_class(L, "FLOAT", real_list);

    /* ---- Phase 3: standard data types ---- */
    L->class_cons = bootstrap_class(L, "CONS", t_list);
    L->class_symbol = bootstrap_class(L, "SYMBOL", t_list);
    L->class_string = bootstrap_class(L, "STRING", t_list);
    L->class_vector = bootstrap_class(L, "VECTOR", t_list);
    L->class_hashtable = bootstrap_class(L, "HASH-TABLE", t_list);
    L->class_function = bootstrap_class(L, "FUNCTION", t_list);

    /* Additional types in the hierarchy. */
    bootstrap_class(L, "SEQUENCE", t_list);
    bootstrap_class(L, "LIST", t_list);
    bootstrap_class(L, "ARRAY", t_list);
    bootstrap_class(L, "CHARACTER", t_list);
    bootstrap_class(L, "STREAM", t_list);
    bootstrap_class(L, "PACKAGE", t_list);
    bootstrap_class(L, "PATHNAME", t_list);

    /* ---- Phase 4: condition hierarchy ---- */
    L->class_condition = bootstrap_class(L, "CONDITION", so_list);

    ls_value_t cond_list = list1(L, ls_wrap(LS_T_CLASS, L->class_condition));
    ls_gc_push_root(L, &cond_list);

    /* WARNING and SERIOUS-CONDITION. */
    ls_class_t *class_warning = bootstrap_class(L, "WARNING", cond_list);
    ls_class_t *class_serious = bootstrap_class(L, "SERIOUS-CONDITION", cond_list);
    (void)class_warning;

    ls_value_t serious_list = list1(L, ls_wrap(LS_T_CLASS, class_serious));
    ls_gc_push_root(L, &serious_list);

    L->class_error = bootstrap_class(L, "ERROR", serious_list);

    ls_value_t err_list = list1(L, ls_wrap(LS_T_CLASS, L->class_error));
    ls_gc_push_root(L, &err_list);

    L->class_simple_error = bootstrap_class(L, "SIMPLE-ERROR", err_list);
    L->class_type_error = bootstrap_class(L, "TYPE-ERROR", err_list);
    L->class_arith_error = bootstrap_class(L, "ARITHMETIC-ERROR", err_list);
    L->class_undefined_function = bootstrap_class(L, "UNDEFINED-FUNCTION", err_list);
    L->class_unbound_variable = bootstrap_class(L, "UNBOUND-VARIABLE", err_list);

    /* Additional standard condition types. */
    bootstrap_class(L, "SIMPLE-CONDITION", cond_list);
    bootstrap_class(L, "SIMPLE-WARNING",
                    ls_cons(L, ls_wrap(LS_T_CLASS, class_warning), ls_nil_v()));
    bootstrap_class(L, "CELL-ERROR", err_list);
    bootstrap_class(L, "CONTROL-ERROR", err_list);
    bootstrap_class(L, "PROGRAM-ERROR", err_list);
    bootstrap_class(L, "PARSE-ERROR", err_list);
    bootstrap_class(L, "STREAM-ERROR", err_list);
    bootstrap_class(L, "FILE-ERROR", err_list);
    bootstrap_class(L, "PACKAGE-ERROR", err_list);
    bootstrap_class(L, "PRINT-NOT-READABLE", err_list);
    bootstrap_class(L, "DIVISION-BY-ZERO",
                    ls_cons(L, ls_wrap(LS_T_CLASS, L->class_arith_error), ls_nil_v()));
    bootstrap_class(L, "FLOATING-POINT-OVERFLOW",
                    ls_cons(L, ls_wrap(LS_T_CLASS, L->class_arith_error), ls_nil_v()));
    bootstrap_class(L, "FLOATING-POINT-UNDERFLOW",
                    ls_cons(L, ls_wrap(LS_T_CLASS, L->class_arith_error), ls_nil_v()));
    bootstrap_class(L, "END-OF-FILE",
                    ls_cons(L, ls_wrap(LS_T_CLASS, L->class_error), ls_nil_v()));
    bootstrap_class(L, "STORAGE-CONDITION",
                    ls_cons(L, ls_wrap(LS_T_CLASS, class_serious), ls_nil_v()));

    /* Pop GC roots (in reverse order). */
    ls_gc_pop_root(L);  /* err_list */
    ls_gc_pop_root(L);  /* serious_list */
    ls_gc_pop_root(L);  /* cond_list */
    ls_gc_pop_root(L);  /* int_list */
    ls_gc_pop_root(L);  /* rat_list */
    ls_gc_pop_root(L);  /* real_list */
    ls_gc_pop_root(L);  /* num_list */
    ls_gc_pop_root(L);  /* so_list */
    ls_gc_pop_root(L);  /* t_list */

    /* ---- Phase 5: register all builtins ---- */
    const char *cl = "COMMON-LISP";

    /* CLOS fundamentals. */
    ls_defun(L, cl, "class-of",           bi_class_of, 1, 1);
    ls_defun(L, cl, "find-class",         bi_find_class, 1, 2);
    ls_defun(L, cl, "make-instance",      bi_make_instance, 1, -1);
    ls_defun(L, cl, "slot-value",         bi_slot_value, 2, 2);
    ls_defun(L, cl, "set-slot-value",     bi_set_slot_value, 3, 3);
    ls_defun(L, cl, "slot-boundp",        bi_slot_boundp, 2, 2);
    ls_defun(L, cl, "typep",              bi_typep, 2, 2);
    ls_defun(L, cl, "subtypep",           bi_subtypep, 2, 2);
    ls_defun(L, cl, "class-name",         bi_class_name, 1, 1);
    ls_defun(L, cl, "class-precedence-list", bi_class_cpl, 1, 1);
    ls_defun(L, cl, "class-direct-superclasses", bi_class_direct_supers, 1, 1);
    ls_defun(L, cl, "class-slots",        bi_class_slots, 1, 1);

    /* Defclass / defgeneric / defmethod. */
    ls_defun(L, cl, "%defclass",          bi_defclass, 3, 3);
    ls_defun(L, cl, "%defgeneric",        bi_defgeneric, 2, 2);
    ls_defun(L, cl, "%defmethod",         bi_defmethod, 4, 4);
    ls_defun(L, cl, "%call-generic",      bi_call_generic, 1, -1);

    /* Call-next-method support. */
    ls_defun(L, cl, "call-next-method",   bi_call_next_method, 0, -1);
    ls_defun(L, cl, "next-method-p",      bi_next_method_p, 0, 0);

    /* Condition system. */
    ls_defun(L, cl, "signal",             bi_signal, 1, -1);
    ls_defun(L, cl, "cerror",             bi_cerror, 1, -1);
    ls_defun(L, cl, "make-condition",     bi_make_condition, 1, -1);
    ls_defun(L, cl, "condition-format-control",   bi_condition_format_control, 1, 1);
    ls_defun(L, cl, "condition-format-arguments", bi_condition_format_args, 1, 1);

    /* Handler / restart machinery. */
    ls_defun(L, cl, "%handler-bind",      bi_handler_bind, 2, 2);
    ls_defun(L, cl, "%handler-case",      bi_handler_case, 3, 3);
    ls_defun(L, cl, "%restart-case",      bi_restart_case, 2, 2);
    ls_defun(L, cl, "%restart-bind",      bi_restart_bind, 2, 2);
    ls_defun(L, cl, "find-restart",       bi_find_restart, 1, 1);
    ls_defun(L, cl, "invoke-restart",     bi_invoke_restart, 1, -1);
    ls_defun(L, cl, "compute-restarts",   bi_compute_restarts, 0, 0);
    ls_defun(L, cl, "restart-name",       bi_restart_name, 1, 1);
}
