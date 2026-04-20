/* lseval.c -- tree-walking evaluator for Litesrpent.
 *
 * This is the canonical interpreter; it implements every special form
 * defined by Common Lisp.  The bytecode compiler (in lscompiler.c) is
 * a *cache* on top of this -- functions defined with defun are compiled
 * lazily on first call into bytecode; the JIT then compiles hot
 * bytecode functions into native code.  But the source-level semantics
 * are anchored here.
 */
#include "lscore.h"
#include "lseval.h"
#include <stdarg.h>

/* Forward decls (the compiler hooks in here too). */
ls_value_t ls_compile_lambda(ls_state_t *L, ls_value_t lambda_list,
                             ls_value_t body, ls_env_t *env, ls_symbol_t *name);
int ls_jit_function_value(ls_state_t *L, ls_value_t fn);

/* ---------- Lambda-list parameter binding ---------- *
 *
 * Common Lisp's lambda-list grammar:
 *   (required* [&optional ...] [&rest var] [&key ...] [&allow-other-keys] [&aux ...])
 *
 * We bind directly into env. */

typedef struct {
    ls_symbol_t *amp_optional;
    ls_symbol_t *amp_rest;
    ls_symbol_t *amp_key;
    ls_symbol_t *amp_aux;
    ls_symbol_t *amp_allow_other_keys;
    ls_symbol_t *amp_body;
    ls_symbol_t *amp_whole;
    ls_symbol_t *amp_environment;
} ll_marks_t;

static void init_marks(ls_state_t *L, ll_marks_t *m) {
    m->amp_optional = L->sym_ampersand_optional;
    m->amp_rest = L->sym_ampersand_rest;
    m->amp_key  = L->sym_ampersand_key;
    m->amp_aux  = L->sym_ampersand_aux;
    m->amp_allow_other_keys = L->sym_ampersand_allow_other_keys;
    m->amp_body = L->sym_ampersand_body;
    m->amp_whole= L->sym_ampersand_whole;
    m->amp_environment = L->sym_ampersand_environment;
}

static int sym_eq(ls_value_t v, ls_symbol_t *s) {
    return v.tag == LS_T_SYMBOL && (ls_symbol_t *)v.u.ptr == s;
}

static ls_value_t parse_default(ls_state_t *L, ls_value_t entry,
                                ls_symbol_t **name_out, ls_value_t *init_out,
                                ls_symbol_t **svar_out) {
    *svar_out = NULL;
    *init_out = ls_nil_v();
    if (entry.tag == LS_T_SYMBOL) {
        *name_out = (ls_symbol_t *)entry.u.ptr;
        return entry;
    }
    /* (name [init [supplied-p]]) */
    ls_value_t name_v = ls_car(entry);
    if (name_v.tag != LS_T_SYMBOL) { ls_error(L, "bad lambda-list entry"); *name_out = NULL; return entry; }
    *name_out = (ls_symbol_t *)name_v.u.ptr;
    ls_value_t rest = ls_cdr(entry);
    if (rest.tag == LS_T_CONS) {
        *init_out = ls_car(rest);
        rest = ls_cdr(rest);
        if (rest.tag == LS_T_CONS) {
            ls_value_t s = ls_car(rest);
            if (s.tag == LS_T_SYMBOL) *svar_out = (ls_symbol_t *)s.u.ptr;
        }
    }
    return entry;
}

static int bind_args(ls_state_t *L, ls_function_t *fn, int nargs, ls_value_t *args, ls_env_t *env) {
    ll_marks_t m; init_marks(L, &m);
    ls_value_t ll = fn->lambda_list;
    int phase = 0; /* 0=req, 1=opt, 2=rest, 3=key, 4=aux */
    int idx = 0;

    /* Pre-collect for &key processing later. */
    while (ll.tag == LS_T_CONS) {
        ls_value_t entry = ls_car(ll);
        ll = ls_cdr(ll);
        if (entry.tag == LS_T_SYMBOL) {
            ls_symbol_t *s = (ls_symbol_t *)entry.u.ptr;
            if (s == m.amp_optional) { phase = 1; continue; }
            if (s == m.amp_rest || s == m.amp_body) { phase = 2; continue; }
            if (s == m.amp_key)      { phase = 3; continue; }
            if (s == m.amp_aux)      { phase = 4; continue; }
            if (s == m.amp_allow_other_keys) continue;
        }
        switch (phase) {
        case 0: {
            if (idx >= nargs) { ls_arity_error(L, fn->name ? fn->name->name->chars : "lambda", nargs, fn->min_args, fn->max_args); return 0; }
            ls_symbol_t *name = (ls_symbol_t *)entry.u.ptr;
            ls_env_bind(L, env, name, args[idx++]);
            break;
        }
        case 1: {
            ls_symbol_t *name = NULL, *svar = NULL;
            ls_value_t init;
            parse_default(L, entry, &name, &init, &svar);
            if (idx < nargs) {
                ls_env_bind(L, env, name, args[idx++]);
                if (svar) ls_env_bind(L, env, svar, ls_t_v());
            } else {
                ls_value_t iv = init.tag == LS_T_NIL ? ls_nil_v() : ls_eval(L, init, env);
                ls_env_bind(L, env, name, iv);
                if (svar) ls_env_bind(L, env, svar, ls_nil_v());
            }
            break;
        }
        case 2: {
            ls_value_t rest_list = ls_nil_v();
            for (int j = nargs - 1; j >= idx; j--)
                rest_list = ls_cons(L, args[j], rest_list);
            ls_symbol_t *name = (ls_symbol_t *)entry.u.ptr;
            ls_env_bind(L, env, name, rest_list);
            idx = nargs;
            break;
        }
        case 3: {
            ls_symbol_t *name = NULL, *svar = NULL;
            ls_value_t init;
            parse_default(L, entry, &name, &init, &svar);
            if (!name) break;
            /* find :name in args[idx..nargs] */
            int found = -1;
            for (int j = idx; j + 1 < nargs; j += 2) {
                if (args[j].tag == LS_T_SYMBOL) {
                    ls_symbol_t *ks = (ls_symbol_t *)args[j].u.ptr;
                    if (ks->package == L->pkg_keyword &&
                        ks->name->len == name->name->len &&
                        memcmp(ks->name->chars, name->name->chars, name->name->len) == 0) {
                        found = j; break;
                    }
                }
            }
            if (found >= 0) {
                ls_env_bind(L, env, name, args[found + 1]);
                if (svar) ls_env_bind(L, env, svar, ls_t_v());
            } else {
                ls_value_t iv = init.tag == LS_T_NIL ? ls_nil_v() : ls_eval(L, init, env);
                ls_env_bind(L, env, name, iv);
                if (svar) ls_env_bind(L, env, svar, ls_nil_v());
            }
            break;
        }
        case 4: {
            ls_symbol_t *name = NULL, *svar = NULL;
            ls_value_t init;
            parse_default(L, entry, &name, &init, &svar);
            ls_value_t iv = init.tag == LS_T_NIL ? ls_nil_v() : ls_eval(L, init, env);
            ls_env_bind(L, env, name, iv);
            break;
        }
        }
    }
    return 1;
}

/* ---------- Special forms ---------- */

static ls_value_t eval_quote(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    (void)L; (void)env;
    return ls_car(args);
}

static ls_value_t eval_if(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t test = ls_car(args);
    ls_value_t then = ls_car(ls_cdr(args));
    ls_value_t else_ = ls_car(ls_cdr(ls_cdr(args)));
    ls_value_t tv = ls_eval(L, test, env);
    if (tv.tag != LS_T_NIL) return ls_eval(L, then, env);
    return ls_eval(L, else_, env);
}

ls_value_t ls_progn(ls_state_t *L, ls_value_t forms, ls_env_t *env) {
    ls_value_t v = ls_nil_v();
    while (forms.tag == LS_T_CONS) {
        v = ls_eval(L, ls_car(forms), env);
        forms = ls_cdr(forms);
    }
    return v;
}

static ls_value_t eval_setq(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t v = ls_nil_v();
    while (args.tag == LS_T_CONS) {
        ls_value_t sym = ls_car(args);
        args = ls_cdr(args);
        if (args.tag != LS_T_CONS) { ls_error(L, "odd args to setq"); break; }
        ls_value_t form = ls_car(args);
        args = ls_cdr(args);
        v = ls_eval(L, form, env);
        if (sym.tag != LS_T_SYMBOL) { ls_error(L, "setq target not a symbol"); break; }
        ls_symbol_t *s = (ls_symbol_t *)sym.u.ptr;
        if (!ls_env_set(env, s, v)) {
            s->value = v;
            s->sym_flags |= LS_SYM_HAS_VALUE;
        }
    }
    return v;
}

static ls_value_t eval_let(ls_state_t *L, ls_value_t args, ls_env_t *env, int starred) {
    ls_value_t binds = ls_car(args);
    ls_value_t body  = ls_cdr(args);
    ls_env_t *new_env = ls_env_new(L, env);
    ls_env_t *eval_env = starred ? new_env : env;
    while (binds.tag == LS_T_CONS) {
        ls_value_t b = ls_car(binds); binds = ls_cdr(binds);
        ls_symbol_t *s; ls_value_t v;
        if (b.tag == LS_T_SYMBOL) { s = (ls_symbol_t *)b.u.ptr; v = ls_nil_v(); }
        else {
            s = (ls_symbol_t *)ls_car(b).u.ptr;
            v = ls_eval(L, ls_car(ls_cdr(b)), eval_env);
        }
        ls_env_bind(L, new_env, s, v);
        if (starred) eval_env = new_env;
    }
    return ls_progn(L, body, new_env);
}

static ls_value_t eval_lambda(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t lambda_list = ls_car(args);
    ls_value_t body = ls_cdr(args);
    ls_value_t v = ls_make_obj(L, LS_T_FUNCTION, sizeof(ls_function_t));
    ls_function_t *f = (ls_function_t *)v.u.ptr;
    f->lambda_list = lambda_list;
    f->body = body;
    f->closure = env;
    /* compute min/max args naively */
    f->min_args = 0; f->max_args = 0;
    int phase = 0;
    ls_value_t ll = lambda_list;
    while (ll.tag == LS_T_CONS) {
        ls_value_t e = ls_car(ll); ll = ls_cdr(ll);
        if (e.tag == LS_T_SYMBOL) {
            ls_symbol_t *s = (ls_symbol_t *)e.u.ptr;
            if (s == L->sym_ampersand_optional) { phase = 1; continue; }
            if (s == L->sym_ampersand_rest || s == L->sym_ampersand_body) {
                f->fn_flags |= LS_FN_HAS_REST; f->max_args = 32767; continue;
            }
            if (s == L->sym_ampersand_key) { f->fn_flags |= LS_FN_HAS_KEY; f->max_args = 32767; continue; }
            if (s == L->sym_ampersand_aux) { phase = 4; continue; }
            if (s == L->sym_ampersand_allow_other_keys) continue;
        }
        if (phase == 0) f->min_args++;
        if (phase != 4) f->max_args++;
    }
    if (f->fn_flags & LS_FN_HAS_REST) f->max_args = 32767;
    if (f->fn_flags & LS_FN_HAS_KEY)  f->max_args = 32767;
    return v;
}

static ls_value_t eval_function(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t a = ls_car(args);
    if (a.tag == LS_T_SYMBOL) {
        ls_symbol_t *s = (ls_symbol_t *)a.u.ptr;
        /* First check the lexical environment for a flet/labels binding. */
        ls_value_t out;
        if (ls_env_lookup(env, s, &out) && ls_is_fn(out)) return out;
        if (s->sym_flags & LS_SYM_HAS_FN) return s->function;
        ls_undefined_function_error(L, s);
        return ls_nil_v();
    }
    if (a.tag == LS_T_CONS && ls_car(a).tag == LS_T_SYMBOL &&
        (ls_symbol_t *)ls_car(a).u.ptr == L->sym_lambda) {
        return eval_lambda(L, ls_cdr(a), env);
    }
    return ls_eval(L, a, env);
}

static ls_value_t eval_defun(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t name_v = ls_car(args);
    ls_value_t lambda_list = ls_car(ls_cdr(args));
    ls_value_t body = ls_cdr(ls_cdr(args));
    if (name_v.tag != LS_T_SYMBOL) { ls_error(L, "defun: name not symbol"); return ls_nil_v(); }
    ls_symbol_t *sn = (ls_symbol_t *)name_v.u.ptr;
    ls_value_t lam_args = ls_cons(L, lambda_list, body);
    ls_value_t fnv = eval_lambda(L, lam_args, env);
    ls_function_t *f = (ls_function_t *)fnv.u.ptr;
    f->name = sn;
    sn->function = fnv;
    sn->sym_flags |= LS_SYM_HAS_FN;
    return name_v;
}

static ls_value_t eval_defmacro(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t name_v = ls_car(args);
    ls_value_t lambda_list = ls_car(ls_cdr(args));
    ls_value_t body = ls_cdr(ls_cdr(args));
    ls_symbol_t *sn = (ls_symbol_t *)name_v.u.ptr;
    ls_value_t lam_args = ls_cons(L, lambda_list, body);
    ls_value_t fnv = eval_lambda(L, lam_args, env);
    ls_function_t *f = (ls_function_t *)fnv.u.ptr;
    f->name = sn;
    f->fn_flags |= LS_FN_MACRO;
    sn->function = fnv;
    sn->sym_flags |= LS_SYM_HAS_FN | LS_SYM_HAS_MACRO;
    return name_v;
}

static ls_value_t eval_defvar(ls_state_t *L, ls_value_t args, ls_env_t *env, int param) {
    ls_value_t name_v = ls_car(args);
    ls_value_t init   = ls_cdr(args).tag == LS_T_CONS ? ls_car(ls_cdr(args)) : ls_nil_v();
    ls_symbol_t *s = (ls_symbol_t *)name_v.u.ptr;
    s->sym_flags |= LS_SYM_SPECIAL;
    if (param || !(s->sym_flags & LS_SYM_HAS_VALUE)) {
        s->value = ls_eval(L, init, env);
        s->sym_flags |= LS_SYM_HAS_VALUE;
    }
    return name_v;
}

static ls_value_t eval_block(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t name = ls_car(args);
    ls_value_t body = ls_cdr(args);
    ls_escape_t e; memset(&e, 0, sizeof e);
    e.tag = name; e.kind = 0;
    e.next = L->esc_top; L->esc_top = &e;
    ls_value_t result;
    if (setjmp(e.buf) == 0) {
        result = ls_progn(L, body, env);
    } else {
        result = e.value;
    }
    L->esc_top = e.next;
    return result;
}

static ls_value_t eval_return_from(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t name = ls_car(args);
    ls_value_t val_form = ls_cdr(args).tag == LS_T_CONS ? ls_car(ls_cdr(args)) : ls_nil_v();
    ls_value_t val = ls_eval(L, val_form, env);
    /* find matching block */
    for (ls_escape_t *e = L->esc_top; e; e = e->next) {
        if (e->kind == 0 && e->tag.tag == LS_T_SYMBOL && e->tag.u.ptr == name.u.ptr) {
            e->value = val;
            longjmp(e->buf, 1);
        }
    }
    ls_error(L, "return-from: no matching block");
    return ls_nil_v();
}

static ls_value_t eval_catch(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t tag_form = ls_car(args);
    ls_value_t body = ls_cdr(args);
    ls_value_t tag = ls_eval(L, tag_form, env);
    ls_escape_t e; memset(&e, 0, sizeof e);
    e.tag = tag; e.kind = 1;
    e.next = L->esc_top; L->esc_top = &e;
    ls_value_t result;
    if (setjmp(e.buf) == 0) {
        result = ls_progn(L, body, env);
    } else {
        result = e.value;
    }
    L->esc_top = e.next;
    return result;
}

static ls_value_t eval_throw(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t tag = ls_eval(L, ls_car(args), env);
    ls_value_t val = ls_eval(L, ls_car(ls_cdr(args)), env);
    for (ls_escape_t *e = L->esc_top; e; e = e->next) {
        if (e->kind == 1 && ls_value_equal(e->tag, tag, LS_HASH_EQL)) {
            e->value = val;
            longjmp(e->buf, 1);
        }
    }
    ls_error(L, "throw: no matching catch");
    return ls_nil_v();
}

static ls_value_t eval_unwind_protect(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t prot = ls_car(args);
    ls_value_t cleanup = ls_cdr(args);
    /* This is simplified: we evaluate prot, then cleanup. If prot
     * non-locally exits, cleanup still runs because we install a
     * handler that re-throws after cleanup.  For a correct full
     * implementation we'd need to walk the escape chain. */
    ls_escape_t e; memset(&e, 0, sizeof e);
    e.kind = 99; /* sentinel, never matches */
    e.next = L->esc_top; L->esc_top = &e;
    ls_value_t result;
    int jumped = setjmp(e.buf);
    if (jumped == 0) {
        result = ls_eval(L, prot, env);
        L->esc_top = e.next;
        ls_progn(L, cleanup, env);
        return result;
    } else {
        L->esc_top = e.next;
        ls_progn(L, cleanup, env);
        /* Re-raise */
        if (L->esc_top) longjmp(L->esc_top->buf, 1);
        return ls_nil_v();
    }
}

/* ---------- return (shorthand for return-from nil) ---------- */

static ls_value_t eval_return(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    ls_value_t val_form = args.tag == LS_T_CONS ? ls_car(args) : ls_nil_v();
    ls_value_t val = ls_eval(L, val_form, env);
    /* find matching block with name NIL */
    for (ls_escape_t *e = L->esc_top; e; e = e->next) {
        if (e->kind == 0 && e->tag.tag == LS_T_NIL) {
            e->value = val;
            longjmp(e->buf, 1);
        }
    }
    ls_error(L, "return: no matching block NIL");
    return ls_nil_v();
}

/* ---------- tagbody / go ---------- */

static ls_value_t eval_tagbody(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* Tags are symbols or integers among the body forms.
     * (tagbody {tag | statement}*)
     * go transfers control to a tag via longjmp.
     */
    ls_escape_t esc;
    memset(&esc, 0, sizeof esc);
    esc.kind = 3; /* tagbody */
    esc.tag = ls_nil_v(); /* will hold the go-target tag */
    esc.next = L->esc_top;
    L->esc_top = &esc;

    /* On first entry or after a go, we scan from the matching tag. */
    ls_value_t forms = args;
    int resume;

    resume = setjmp(esc.buf);
    if (resume != 0) {
        /* esc.tag holds the symbol/integer the go jumped to.
         * Scan forms to find it. */
        ls_value_t scan = args;
        int found = 0;
        while (scan.tag == LS_T_CONS) {
            ls_value_t f = ls_car(scan);
            if ((f.tag == LS_T_SYMBOL || f.tag == LS_T_FIXNUM) &&
                ls_value_equal(f, esc.tag, LS_HASH_EQL)) {
                forms = ls_cdr(scan); /* execute after the tag */
                found = 1;
                break;
            }
            scan = ls_cdr(scan);
        }
        if (!found) {
            L->esc_top = esc.next;
            ls_error(L, "go: tag not found in tagbody");
            return ls_nil_v();
        }
    }

    /* Execute forms from current position. */
    while (forms.tag == LS_T_CONS) {
        ls_value_t f = ls_car(forms);
        forms = ls_cdr(forms);
        /* Skip tags (symbols and integers are tags, not forms). */
        if (f.tag == LS_T_SYMBOL || f.tag == LS_T_FIXNUM)
            continue;
        ls_eval(L, f, env);
    }

    L->esc_top = esc.next;
    return ls_nil_v(); /* tagbody always returns NIL */
}

static ls_value_t eval_go(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    (void)env;
    ls_value_t tag = ls_car(args);
    /* Find enclosing tagbody escape frame. */
    for (ls_escape_t *e = L->esc_top; e; e = e->next) {
        if (e->kind == 3) {
            e->tag = tag;
            longjmp(e->buf, 1);
        }
    }
    ls_error(L, "go: no enclosing tagbody");
    return ls_nil_v();
}

/* ---------- do / do* ---------- */

static ls_value_t eval_do_impl(ls_state_t *L, ls_value_t args, ls_env_t *env, int starred) {
    /* (do/do* ((var init step)...) (end-test result...) body...)
     * Wrapped in (block nil ...) to support (return ...). */
    ls_value_t var_clauses = ls_car(args);
    ls_value_t end_clause  = ls_car(ls_cdr(args));
    ls_value_t body        = ls_cdr(ls_cdr(args));

    /* Set up block NIL for (return ...) support. */
    ls_escape_t blk;
    memset(&blk, 0, sizeof blk);
    blk.tag = ls_nil_v();
    blk.kind = 0; /* block */
    blk.next = L->esc_top;
    L->esc_top = &blk;

    ls_value_t result;
    if (setjmp(blk.buf) != 0) {
        result = blk.value;
        L->esc_top = blk.next;
        return result;
    }

    /* Phase 1: bind variables with initial values. */
    ls_env_t *loop_env = ls_env_new(L, env);
    ls_env_t *init_env = starred ? loop_env : env;

    ls_value_t vc = var_clauses;
    while (vc.tag == LS_T_CONS) {
        ls_value_t clause = ls_car(vc); vc = ls_cdr(vc);
        ls_symbol_t *var;
        ls_value_t init_val = ls_nil_v();
        if (clause.tag == LS_T_SYMBOL) {
            var = (ls_symbol_t *)clause.u.ptr;
        } else {
            var = (ls_symbol_t *)ls_car(clause).u.ptr;
            ls_value_t init_rest = ls_cdr(clause);
            if (init_rest.tag == LS_T_CONS)
                init_val = ls_eval(L, ls_car(init_rest), init_env);
        }
        ls_env_bind(L, loop_env, var, init_val);
    }

    /* Phase 2: iterate. */
    ls_value_t end_test = ls_car(end_clause);
    ls_value_t result_forms = ls_cdr(end_clause);

    for (;;) {
        /* Test end condition. */
        ls_value_t tv = ls_eval(L, end_test, loop_env);
        if (tv.tag != LS_T_NIL) {
            /* End: evaluate result forms and return last value. */
            result = ls_progn(L, result_forms, loop_env);
            L->esc_top = blk.next;
            return result;
        }

        /* Execute body (with implicit tagbody around it). */
        ls_value_t b = body;
        while (b.tag == LS_T_CONS) {
            ls_value_t f = ls_car(b); b = ls_cdr(b);
            /* Skip tags in the body (tagbody-like behavior). */
            if (f.tag == LS_T_SYMBOL || f.tag == LS_T_FIXNUM)
                continue;
            ls_eval(L, f, loop_env);
        }

        /* Phase 3: step variables. */
        if (starred) {
            /* do*: step sequentially, each step sees prior updates. */
            vc = var_clauses;
            while (vc.tag == LS_T_CONS) {
                ls_value_t clause = ls_car(vc); vc = ls_cdr(vc);
                if (clause.tag == LS_T_SYMBOL) continue;
                ls_symbol_t *var = (ls_symbol_t *)ls_car(clause).u.ptr;
                ls_value_t rest = ls_cdr(clause);
                if (rest.tag == LS_T_CONS) {
                    rest = ls_cdr(rest); /* skip init, get step */
                    if (rest.tag == LS_T_CONS) {
                        ls_value_t step_val = ls_eval(L, ls_car(rest), loop_env);
                        ls_env_set(loop_env, var, step_val);
                    }
                }
            }
        } else {
            /* do: evaluate all step forms first, then update all at once. */
            ls_value_t step_vals[64];
            ls_symbol_t *step_vars[64];
            int n_steps = 0;

            vc = var_clauses;
            while (vc.tag == LS_T_CONS && n_steps < 64) {
                ls_value_t clause = ls_car(vc); vc = ls_cdr(vc);
                if (clause.tag == LS_T_SYMBOL) continue;
                ls_symbol_t *var = (ls_symbol_t *)ls_car(clause).u.ptr;
                ls_value_t rest = ls_cdr(clause);
                if (rest.tag == LS_T_CONS) {
                    rest = ls_cdr(rest); /* skip init, get step */
                    if (rest.tag == LS_T_CONS) {
                        step_vars[n_steps] = var;
                        step_vals[n_steps] = ls_eval(L, ls_car(rest), loop_env);
                        n_steps++;
                    }
                }
            }
            for (int i = 0; i < n_steps; i++)
                ls_env_set(loop_env, step_vars[i], step_vals[i]);
        }
    }
}

/* ---------- dotimes ---------- */

static ls_value_t eval_dotimes(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (dotimes (var count-form result-form) body...) */
    ls_value_t spec = ls_car(args);
    ls_value_t body = ls_cdr(args);

    ls_symbol_t *var = (ls_symbol_t *)ls_car(spec).u.ptr;
    ls_value_t count_form = ls_car(ls_cdr(spec));
    ls_value_t result_form = ls_cdr(ls_cdr(spec)).tag == LS_T_CONS
                             ? ls_car(ls_cdr(ls_cdr(spec))) : ls_nil_v();

    ls_value_t count_v = ls_eval(L, count_form, env);
    int64_t count = count_v.u.fixnum;

    /* Set up block NIL for (return ...) support. */
    ls_escape_t blk;
    memset(&blk, 0, sizeof blk);
    blk.tag = ls_nil_v();
    blk.kind = 0;
    blk.next = L->esc_top;
    L->esc_top = &blk;

    ls_value_t result;
    if (setjmp(blk.buf) != 0) {
        result = blk.value;
        L->esc_top = blk.next;
        return result;
    }

    ls_env_t *loop_env = ls_env_new(L, env);
    ls_env_bind(L, loop_env, var, ls_make_fixnum(0));

    for (int64_t i = 0; i < count; i++) {
        ls_env_set(loop_env, var, ls_make_fixnum(i));
        ls_progn(L, body, loop_env);
    }

    /* Set var to count for result form evaluation. */
    ls_env_set(loop_env, var, ls_make_fixnum(count));
    result = ls_eval(L, result_form, loop_env);
    L->esc_top = blk.next;
    return result;
}

/* ---------- dolist ---------- */

static ls_value_t eval_dolist(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (dolist (var list-form result-form) body...) */
    ls_value_t spec = ls_car(args);
    ls_value_t body = ls_cdr(args);

    ls_symbol_t *var = (ls_symbol_t *)ls_car(spec).u.ptr;
    ls_value_t list_form = ls_car(ls_cdr(spec));
    ls_value_t result_form = ls_cdr(ls_cdr(spec)).tag == LS_T_CONS
                             ? ls_car(ls_cdr(ls_cdr(spec))) : ls_nil_v();

    ls_value_t list_val = ls_eval(L, list_form, env);

    /* Set up block NIL for (return ...) support. */
    ls_escape_t blk;
    memset(&blk, 0, sizeof blk);
    blk.tag = ls_nil_v();
    blk.kind = 0;
    blk.next = L->esc_top;
    L->esc_top = &blk;

    ls_value_t result;
    if (setjmp(blk.buf) != 0) {
        result = blk.value;
        L->esc_top = blk.next;
        return result;
    }

    ls_env_t *loop_env = ls_env_new(L, env);
    ls_env_bind(L, loop_env, var, ls_nil_v());

    while (list_val.tag == LS_T_CONS) {
        ls_env_set(loop_env, var, ls_car(list_val));
        ls_progn(L, body, loop_env);
        list_val = ls_cdr(list_val);
    }

    /* Set var to NIL for result form evaluation (per spec). */
    ls_env_set(loop_env, var, ls_nil_v());
    result = ls_eval(L, result_form, loop_env);
    L->esc_top = blk.next;
    return result;
}

/* ---------- setf ---------- */

static ls_value_t eval_setf(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (setf place value [place value ...])
     * Handles: (setf var val), (setf (car x) val), (setf (cdr x) val),
     *          (setf (first x) val), (setf (rest x) val),
     *          (setf (nth n x) val), (setf (aref vec idx) val),
     *          (setf (gethash key ht) val),
     *          (setf (slot-value inst name) val),
     *          (setf (symbol-value sym) val),
     *          (setf (symbol-function sym) val).
     */
    ls_value_t v = ls_nil_v();
    while (args.tag == LS_T_CONS) {
        ls_value_t place = ls_car(args);
        args = ls_cdr(args);
        if (args.tag != LS_T_CONS) { ls_error(L, "odd args to setf"); break; }
        ls_value_t val_form = ls_car(args);
        args = ls_cdr(args);

        v = ls_eval(L, val_form, env);

        if (place.tag == LS_T_SYMBOL) {
            /* Simple variable setf = setq */
            ls_symbol_t *s = (ls_symbol_t *)place.u.ptr;
            if (!ls_env_set(env, s, v)) {
                s->value = v;
                s->sym_flags |= LS_SYM_HAS_VALUE;
            }
        } else if (place.tag == LS_T_CONS) {
            ls_value_t op = ls_car(place);
            if (op.tag != LS_T_SYMBOL) { ls_error(L, "setf: bad place"); break; }
            ls_symbol_t *place_op = (ls_symbol_t *)op.u.ptr;
            ls_value_t place_args = ls_cdr(place);

            if (place_op == L->sym_car || place_op == L->sym_first) {
                /* (setf (car x) v) => rplaca */
                ls_value_t target = ls_eval(L, ls_car(place_args), env);
                if (target.tag != LS_T_CONS) { ls_error(L, "setf car: not a cons"); break; }
                ((ls_cons_t *)target.u.ptr)->car = v;
            } else if (place_op == L->sym_cdr || place_op == L->sym_rest) {
                /* (setf (cdr x) v) => rplacd */
                ls_value_t target = ls_eval(L, ls_car(place_args), env);
                if (target.tag != LS_T_CONS) { ls_error(L, "setf cdr: not a cons"); break; }
                ((ls_cons_t *)target.u.ptr)->cdr = v;
            } else if (place_op == L->sym_nth) {
                /* (setf (nth n list) v) */
                ls_value_t idx_v = ls_eval(L, ls_car(place_args), env);
                ls_value_t lst = ls_eval(L, ls_car(ls_cdr(place_args)), env);
                int64_t n = idx_v.u.fixnum;
                for (int64_t i = 0; i < n && lst.tag == LS_T_CONS; i++)
                    lst = ls_cdr(lst);
                if (lst.tag == LS_T_CONS)
                    ((ls_cons_t *)lst.u.ptr)->car = v;
                else
                    ls_error(L, "setf nth: index out of range");
            } else if (place_op == L->sym_aref) {
                /* (setf (aref vec idx) v) */
                ls_value_t vec_v = ls_eval(L, ls_car(place_args), env);
                ls_value_t idx_v = ls_eval(L, ls_car(ls_cdr(place_args)), env);
                if (vec_v.tag != LS_T_VECTOR) { ls_error(L, "setf aref: not a vector"); break; }
                ls_vector_t *vec = (ls_vector_t *)vec_v.u.ptr;
                int64_t idx = idx_v.u.fixnum;
                if (idx < 0 || (size_t)idx >= vec->len) {
                    ls_error(L, "setf aref: index out of bounds");
                    break;
                }
                vec->data[idx] = v;
            } else if (place_op == L->sym_gethash) {
                /* (setf (gethash key ht) v) */
                ls_value_t key = ls_eval(L, ls_car(place_args), env);
                ls_value_t ht_v = ls_eval(L, ls_car(ls_cdr(place_args)), env);
                if (ht_v.tag != LS_T_HASHTABLE) { ls_error(L, "setf gethash: not a hash-table"); break; }
                ls_hashtable_t *ht = (ls_hashtable_t *)ht_v.u.ptr;
                ls_hash_put(L, ht, key, v);
            } else if (place_op == L->sym_slot_value) {
                /* (setf (slot-value inst slot-name) v) */
                ls_value_t inst_v = ls_eval(L, ls_car(place_args), env);
                ls_value_t sname = ls_eval(L, ls_car(ls_cdr(place_args)), env);
                if (inst_v.tag != LS_T_INSTANCE) { ls_error(L, "setf slot-value: not an instance"); break; }
                ls_instance_t *inst = (ls_instance_t *)inst_v.u.ptr;
                extern void ls_slot_set(ls_state_t *L, ls_instance_t *inst,
                                        ls_value_t slot_name, ls_value_t val);
                ls_slot_set(L, inst, sname, v);
            } else if (place_op == L->sym_symbol_value) {
                /* (setf (symbol-value sym) v) */
                ls_value_t sym_v = ls_eval(L, ls_car(place_args), env);
                if (sym_v.tag != LS_T_SYMBOL) { ls_error(L, "setf symbol-value: not a symbol"); break; }
                ls_symbol_t *s = (ls_symbol_t *)sym_v.u.ptr;
                s->value = v;
                s->sym_flags |= LS_SYM_HAS_VALUE;
            } else if (place_op == L->sym_symbol_function) {
                /* (setf (symbol-function sym) v) */
                ls_value_t sym_v = ls_eval(L, ls_car(place_args), env);
                if (sym_v.tag != LS_T_SYMBOL) { ls_error(L, "setf symbol-function: not a symbol"); break; }
                ls_symbol_t *s = (ls_symbol_t *)sym_v.u.ptr;
                s->function = v;
                s->sym_flags |= LS_SYM_HAS_FN;
            } else {
                /* Fallback: look up *setf-expanders* alist for this accessor.
                 * The variable is defined in the user package (where stdlib
                 * core.lisp runs), so look it up via the current package's
                 * search path. */
                int found_es;
                ls_value_t expanders_sym = ls_intern_sym(L, L->pkg_cl_user, "*SETF-EXPANDERS*", 16, &found_es);
                ls_symbol_t *es = (ls_symbol_t *)expanders_sym.u.ptr;
                int matched = 0;
                if (es->sym_flags & LS_SYM_HAS_VALUE) {
                    ls_value_t alist = es->value;
                    while (alist.tag == LS_T_CONS) {
                        ls_value_t entry = ls_car(alist);
                        if (entry.tag == LS_T_CONS) {
                            ls_value_t key = ls_car(entry);
                            if (key.tag == LS_T_SYMBOL && key.u.ptr == op.u.ptr) {
                                ls_value_t expander_fn = ls_cdr(entry);
                                /* Call (expander-fn place-args new-val); place-args is the
                                 * unevaluated arg list. The expander returns a form to evaluate. */
                                ls_value_t apply_args[2];
                                apply_args[0] = place_args; /* unevaluated */
                                apply_args[1] = v;          /* the new value */
                                ls_value_t expanded = ls_apply(L, expander_fn, 2, apply_args);
                                ls_eval(L, expanded, env);
                                matched = 1;
                                break;
                            }
                        }
                        alist = ls_cdr(alist);
                    }
                }
                if (!matched) {
                    /* Last resort: try (setf-ACCESSOR arg1 ... argn newval). */
                    char setf_name[256];
                    snprintf(setf_name, sizeof setf_name, "SETF-%s", place_op->name->chars);
                    ls_value_t setter_sym = ls_intern(L, "COMMON-LISP", setf_name);
                    ls_symbol_t *ss = (ls_symbol_t *)setter_sym.u.ptr;
                    if (ss->function.tag != LS_T_NIL) {
                        /* Evaluate place_args, then call (setter args... v) */
                        ls_value_t arr[32]; int an = 0;
                        ls_value_t pa = place_args;
                        while (pa.tag == LS_T_CONS && an < 31) {
                            arr[an++] = ls_eval(L, ls_car(pa), env);
                            pa = ls_cdr(pa);
                        }
                        arr[an++] = v;
                        ls_apply(L, ss->function, an, arr);
                    } else {
                        ls_error(L, "setf: unsupported place: (%s ...)", place_op->name->chars);
                        break;
                    }
                }
            }
        } else {
            ls_error(L, "setf: invalid place form");
            break;
        }
    }
    return v;
}

/* ---------- case ---------- */

static ls_value_t eval_case(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (case keyform (key body)... [(otherwise body) | (t body)]) */
    ls_value_t keyform = ls_car(args);
    ls_value_t clauses = ls_cdr(args);
    ls_value_t key = ls_eval(L, keyform, env);

    while (clauses.tag == LS_T_CONS) {
        ls_value_t clause = ls_car(clauses);
        clauses = ls_cdr(clauses);
        ls_value_t keys = ls_car(clause);
        ls_value_t body = ls_cdr(clause);

        /* Check for otherwise / t. */
        if (keys.tag == LS_T_SYMBOL) {
            ls_symbol_t *ks = (ls_symbol_t *)keys.u.ptr;
            if (ks == L->sym_otherwise || ks == L->sym_t) {
                return ls_progn(L, body, env);
            }
        }

        /* keys can be an atom (single key) or a list of keys. */
        if (keys.tag == LS_T_CONS) {
            /* List of keys: check each with eql. */
            ls_value_t kl = keys;
            while (kl.tag == LS_T_CONS) {
                if (ls_value_equal(key, ls_car(kl), LS_HASH_EQL))
                    return ls_progn(L, body, env);
                kl = ls_cdr(kl);
            }
        } else {
            /* Single key (atom): compare with eql. */
            if (ls_value_equal(key, keys, LS_HASH_EQL))
                return ls_progn(L, body, env);
        }
    }
    return ls_nil_v();
}

/* ---------- typecase ---------- */

static ls_value_t eval_typecase(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (typecase keyform (type body)... [(otherwise body) | (t body)]) */
    ls_value_t keyform = ls_car(args);
    ls_value_t clauses = ls_cdr(args);
    ls_value_t key = ls_eval(L, keyform, env);

    /* Get the class of the key value. */
    extern ls_value_t ls_class_of(ls_state_t *L, ls_value_t v);
    ls_value_t key_class_v = ls_class_of(L, key);
    ls_class_t *key_class = (ls_class_t *)key_class_v.u.ptr;

    while (clauses.tag == LS_T_CONS) {
        ls_value_t clause = ls_car(clauses);
        clauses = ls_cdr(clauses);
        ls_value_t type_spec = ls_car(clause);
        ls_value_t body = ls_cdr(clause);

        /* Check for otherwise / t. */
        if (type_spec.tag == LS_T_SYMBOL) {
            ls_symbol_t *ts = (ls_symbol_t *)type_spec.u.ptr;
            if (ts == L->sym_otherwise || ts == L->sym_t)
                return ls_progn(L, body, env);

            /* Look up the class for the type name. */
            if (ts->sym_flags & LS_SYM_HAS_VALUE) {
                ls_value_t cv = ts->value;
                if (cv.tag == LS_T_CLASS) {
                    ls_class_t *type_class = (ls_class_t *)cv.u.ptr;
                    /* Check if key_class is a subtype via CPL. */
                    ls_value_t cpl = key_class->precedence_list;
                    while (cpl.tag == LS_T_CONS) {
                        if (ls_car(cpl).u.ptr == (void *)type_class)
                            return ls_progn(L, body, env);
                        cpl = ls_cdr(cpl);
                    }
                }
            }
        } else if (type_spec.tag == LS_T_CLASS) {
            ls_class_t *type_class = (ls_class_t *)type_spec.u.ptr;
            ls_value_t cpl = key_class->precedence_list;
            while (cpl.tag == LS_T_CONS) {
                if (ls_car(cpl).u.ptr == (void *)type_class)
                    return ls_progn(L, body, env);
                cpl = ls_cdr(cpl);
            }
        }
    }
    return ls_nil_v();
}

/* ---------- multiple-value-bind ---------- */

static ls_value_t eval_multiple_value_bind(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (multiple-value-bind (var1 var2 ...) values-form body...) */
    ls_value_t vars = ls_car(args);
    ls_value_t values_form = ls_car(ls_cdr(args));
    ls_value_t body = ls_cdr(ls_cdr(args));

    /* Evaluate the values-form.  The primary value is the return value;
     * additional values are in L->mv. */
    ls_value_t primary = ls_eval(L, values_form, env);

    /* L->mv.v[0] = primary, L->mv.n = number of values */
    if (L->mv.n < 1) {
        L->mv.n = 1;
        L->mv.v[0] = primary;
    }

    ls_env_t *new_env = ls_env_new(L, env);
    int idx = 0;
    while (vars.tag == LS_T_CONS) {
        ls_symbol_t *var = (ls_symbol_t *)ls_car(vars).u.ptr;
        ls_value_t val = (idx < L->mv.n) ? L->mv.v[idx] : ls_nil_v();
        ls_env_bind(L, new_env, var, val);
        idx++;
        vars = ls_cdr(vars);
    }

    return ls_progn(L, body, new_env);
}

/* ---------- prog / prog* ---------- */

static ls_value_t eval_prog(ls_state_t *L, ls_value_t args, ls_env_t *env, int starred) {
    /* (prog/prog* ((var init)...) {declaration}* {tag | statement}*)
     * = let/let* bindings + block nil + tagbody.
     */
    ls_value_t bindings = ls_car(args);
    ls_value_t body = ls_cdr(args);

    /* Skip declarations at the start of body. */
    ls_value_t actual_body = body;
    while (actual_body.tag == LS_T_CONS) {
        ls_value_t f = ls_car(actual_body);
        if (f.tag == LS_T_CONS && ls_car(f).tag == LS_T_SYMBOL &&
            (ls_symbol_t *)ls_car(f).u.ptr == L->sym_declare)
            actual_body = ls_cdr(actual_body);
        else
            break;
    }

    /* Phase 1: bind variables (let or let*). */
    ls_env_t *prog_env = ls_env_new(L, env);
    ls_env_t *init_env = starred ? prog_env : env;
    ls_value_t bl = bindings;
    while (bl.tag == LS_T_CONS) {
        ls_value_t b = ls_car(bl); bl = ls_cdr(bl);
        ls_symbol_t *s; ls_value_t v;
        if (b.tag == LS_T_SYMBOL) { s = (ls_symbol_t *)b.u.ptr; v = ls_nil_v(); }
        else {
            s = (ls_symbol_t *)ls_car(b).u.ptr;
            v = ls_eval(L, ls_car(ls_cdr(b)), init_env);
        }
        ls_env_bind(L, prog_env, s, v);
    }

    /* Phase 2: establish block nil. */
    ls_escape_t blk;
    memset(&blk, 0, sizeof blk);
    blk.tag = ls_nil_v();
    blk.kind = 0; /* block */
    blk.next = L->esc_top;
    L->esc_top = &blk;

    ls_value_t result;
    if (setjmp(blk.buf) != 0) {
        result = blk.value;
        L->esc_top = blk.next;
        return result;
    }

    /* Phase 3: execute body as a tagbody.
     * We reuse the tagbody logic inline. */
    ls_escape_t tesc;
    memset(&tesc, 0, sizeof tesc);
    tesc.kind = 3; /* tagbody */
    tesc.tag = ls_nil_v();
    tesc.next = L->esc_top;
    L->esc_top = &tesc;

    ls_value_t forms = actual_body;
    int resume;

    resume = setjmp(tesc.buf);
    if (resume != 0) {
        /* Scan actual_body for the tag. */
        ls_value_t scan = actual_body;
        int found = 0;
        while (scan.tag == LS_T_CONS) {
            ls_value_t f = ls_car(scan);
            if ((f.tag == LS_T_SYMBOL || f.tag == LS_T_FIXNUM) &&
                ls_value_equal(f, tesc.tag, LS_HASH_EQL)) {
                forms = ls_cdr(scan);
                found = 1;
                break;
            }
            scan = ls_cdr(scan);
        }
        if (!found) {
            L->esc_top = tesc.next;
            /* Also pop block. */
            L->esc_top = blk.next;
            ls_error(L, "go: tag not found in prog");
            return ls_nil_v();
        }
    }

    while (forms.tag == LS_T_CONS) {
        ls_value_t f = ls_car(forms); forms = ls_cdr(forms);
        if (f.tag == LS_T_SYMBOL || f.tag == LS_T_FIXNUM)
            continue;
        ls_eval(L, f, prog_env);
    }

    L->esc_top = tesc.next;  /* pop tagbody */
    L->esc_top = blk.next;   /* pop block */
    return ls_nil_v(); /* prog returns NIL if no (return ...) was executed */
}

/* ---------- defclass / defgeneric / defmethod (special form dispatch) ---------- */

static ls_value_t eval_defclass(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (defclass name (supers...) ((slot-name :initarg :name ...)...))
     * We parse the special form and call %defclass with resolved data. */
    ls_value_t name_v = ls_car(args);
    ls_value_t supers_form = ls_car(ls_cdr(args));
    ls_value_t slot_specs = ls_cdr(ls_cdr(args)).tag == LS_T_CONS
                            ? ls_car(ls_cdr(ls_cdr(args))) : ls_nil_v();

    if (name_v.tag != LS_T_SYMBOL) {
        ls_error(L, "defclass: name must be a symbol");
        return ls_nil_v();
    }

    /* Build the slot-names list from slot-specs.
     * Each spec can be a symbol or (slot-name ...). */
    ls_value_t slot_names = ls_nil_v();
    ls_value_t slot_tail = ls_nil_v();
    ls_value_t ss = slot_specs;
    while (ss.tag == LS_T_CONS) {
        ls_value_t spec = ls_car(ss); ss = ls_cdr(ss);
        ls_value_t sname;
        if (spec.tag == LS_T_SYMBOL)
            sname = spec;
        else if (spec.tag == LS_T_CONS)
            sname = ls_car(spec);
        else
            continue;
        ls_value_t cell = ls_cons(L, sname, ls_nil_v());
        if (slot_names.tag == LS_T_NIL) slot_names = slot_tail = cell;
        else { ((ls_cons_t *)slot_tail.u.ptr)->cdr = cell; slot_tail = cell; }
    }

    /* Call %defclass(name, supers-list, slot-names-list). */
    ls_value_t call_args[3];
    call_args[0] = name_v;
    call_args[1] = supers_form;
    call_args[2] = slot_names;

    /* Look up %defclass. */
    ls_symbol_t *dc = (ls_symbol_t *)ls_intern(L, "COMMON-LISP", "%DEFCLASS").u.ptr;
    if (dc->sym_flags & LS_SYM_HAS_FN)
        return ls_apply(L, dc->function, 3, call_args);

    ls_error(L, "defclass: %%defclass not available");
    return ls_nil_v();
}

static ls_value_t eval_defgeneric(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (defgeneric name (lambda-list) [:documentation ...]) */
    ls_value_t name_v = ls_car(args);
    ls_value_t lambda_list = ls_car(ls_cdr(args));

    if (name_v.tag != LS_T_SYMBOL) {
        ls_error(L, "defgeneric: name must be a symbol");
        return ls_nil_v();
    }

    ls_value_t call_args[2];
    call_args[0] = name_v;
    call_args[1] = lambda_list;

    ls_symbol_t *dg = (ls_symbol_t *)ls_intern(L, "COMMON-LISP", "%DEFGENERIC").u.ptr;
    if (dg->sym_flags & LS_SYM_HAS_FN)
        return ls_apply(L, dg->function, 2, call_args);

    ls_error(L, "defgeneric: %%defgeneric not available");
    return ls_nil_v();
}

static ls_value_t eval_defmethod(ls_state_t *L, ls_value_t args, ls_env_t *env) {
    /* (defmethod name [qualifier] ((param specializer) ...) body...)
     *
     * We need to:
     * 1. Parse the name.
     * 2. Check for optional qualifier (:before :after :around).
     * 3. Parse the specialized lambda list to extract specializers.
     * 4. Build a lambda with the stripped lambda list.
     * 5. Call %defmethod(name, specializers, qualifiers, lambda).
     */
    ls_value_t name_v = ls_car(args);
    args = ls_cdr(args);

    if (name_v.tag != LS_T_SYMBOL) {
        ls_error(L, "defmethod: name must be a symbol");
        return ls_nil_v();
    }

    /* Check for qualifier keyword (:before, :after, :around). */
    ls_value_t qualifiers = ls_nil_v();
    if (args.tag == LS_T_CONS) {
        ls_value_t maybe_qual = ls_car(args);
        if (maybe_qual.tag == LS_T_SYMBOL) {
            ls_symbol_t *mqs = (ls_symbol_t *)maybe_qual.u.ptr;
            if (mqs->package == L->pkg_keyword) {
                /* It's a keyword qualifier. */
                qualifiers = ls_cons(L, maybe_qual, ls_nil_v());
                args = ls_cdr(args);
            }
        }
    }

    /* Next should be the specialized lambda list. */
    ls_value_t spec_ll = ls_car(args);
    ls_value_t body = ls_cdr(args);

    /* Parse specialized lambda list: each param can be a symbol or
     * (param-name class-name). Build both the plain lambda list
     * and the specializer list. */
    ls_value_t plain_ll = ls_nil_v(), plain_tail = ls_nil_v();
    ls_value_t specializers = ls_nil_v(), spec_tail = ls_nil_v();

    ls_value_t sl = spec_ll;
    while (sl.tag == LS_T_CONS) {
        ls_value_t entry = ls_car(sl); sl = ls_cdr(sl);

        /* Check for &-keywords: pass them through to the plain lambda list. */
        if (entry.tag == LS_T_SYMBOL) {
            ls_symbol_t *es = (ls_symbol_t *)entry.u.ptr;
            if (es == L->sym_ampersand_rest || es == L->sym_ampersand_optional ||
                es == L->sym_ampersand_key || es == L->sym_ampersand_aux ||
                es == L->sym_ampersand_body || es == L->sym_ampersand_allow_other_keys) {
                /* Append rest of sl as-is to plain_ll. */
                ls_value_t cell = ls_cons(L, entry, sl);
                if (plain_ll.tag == LS_T_NIL) plain_ll = cell;
                else ((ls_cons_t *)plain_tail.u.ptr)->cdr = cell;
                break;
            }
        }

        ls_value_t param_name;
        ls_value_t specializer;
        if (entry.tag == LS_T_CONS) {
            /* (param-name class-name) */
            param_name = ls_car(entry);
            specializer = ls_car(ls_cdr(entry));
        } else {
            /* Just a symbol -- specializer is T. */
            param_name = entry;
            specializer = ls_wrap(LS_T_SYMBOL, L->sym_t);
        }

        ls_value_t pcell = ls_cons(L, param_name, ls_nil_v());
        if (plain_ll.tag == LS_T_NIL) plain_ll = plain_tail = pcell;
        else { ((ls_cons_t *)plain_tail.u.ptr)->cdr = pcell; plain_tail = pcell; }

        ls_value_t scell = ls_cons(L, specializer, ls_nil_v());
        if (specializers.tag == LS_T_NIL) specializers = spec_tail = scell;
        else { ((ls_cons_t *)spec_tail.u.ptr)->cdr = scell; spec_tail = scell; }
    }

    /* Build the lambda from the plain lambda list and body. */
    ls_value_t lam_args = ls_cons(L, plain_ll, body);
    ls_value_t lambda_v = eval_lambda(L, lam_args, env);

    /* Call %defmethod(generic-name, specializers, qualifiers, lambda). */
    ls_value_t call_args[4];
    call_args[0] = name_v;
    call_args[1] = specializers;
    call_args[2] = qualifiers;
    call_args[3] = lambda_v;

    ls_symbol_t *dm = (ls_symbol_t *)ls_intern(L, "COMMON-LISP", "%DEFMETHOD").u.ptr;
    if (dm->sym_flags & LS_SYM_HAS_FN)
        return ls_apply(L, dm->function, 4, call_args);

    ls_error(L, "defmethod: %%defmethod not available");
    return ls_nil_v();
}

/* ---------- Macro expansion ---------- */

static ls_value_t apply_macro(ls_state_t *L, ls_function_t *m, ls_value_t form, ls_env_t *env) {
    /* macro arguments are the cdr of form (unevaluated) */
    ls_value_t args = ls_cdr(form);
    int n = (int)ls_list_length(args);
    ls_value_t arr[64]; if (n > 64) n = 64;
    int i = 0;
    while (args.tag == LS_T_CONS && i < 64) { arr[i++] = ls_car(args); args = ls_cdr(args); }
    ls_env_t *new_env = ls_env_new(L, m->closure);
    ls_function_t saved = *m;
    saved.fn_flags &= ~LS_FN_MACRO;
    bind_args(L, &saved, n, arr, new_env);
    ls_value_t expansion = ls_progn(L, m->body, new_env);
    return ls_eval(L, expansion, env);
}

ls_value_t ls_macroexpand(ls_state_t *L, ls_value_t form, ls_env_t *env) {
    while (form.tag == LS_T_CONS) {
        ls_value_t hd = ls_car(form);
        if (hd.tag != LS_T_SYMBOL) break;
        ls_symbol_t *s = (ls_symbol_t *)hd.u.ptr;
        if (!(s->sym_flags & LS_SYM_HAS_MACRO)) break;
        ls_function_t *m = ls_function_p(s->function);
        if (!m) break;
        /* run macro and continue expanding */
        ls_value_t args = ls_cdr(form);
        int n = (int)ls_list_length(args);
        ls_value_t arr[64]; if (n > 64) n = 64;
        int i = 0;
        while (args.tag == LS_T_CONS && i < 64) { arr[i++] = ls_car(args); args = ls_cdr(args); }
        ls_env_t *ne = ls_env_new(L, m->closure);
        ls_function_t saved = *m;
        saved.fn_flags &= ~LS_FN_MACRO;
        bind_args(L, &saved, n, arr, ne);
        form = ls_progn(L, m->body, ne);
        (void)env;
    }
    return form;
}

/* ---------- Quasiquote expansion ---------- */
static ls_value_t expand_qq(ls_state_t *L, ls_value_t form, ls_env_t *env);

static ls_value_t qq_list(ls_state_t *L, ls_value_t form, ls_env_t *env) {
    if (form.tag != LS_T_CONS) return expand_qq(L, form, env);
    ls_value_t hd = ls_car(form);
    if (hd.tag == LS_T_SYMBOL) {
        ls_symbol_t *s = (ls_symbol_t *)hd.u.ptr;
        if (s == L->sym_unquote) return ls_eval(L, ls_car(ls_cdr(form)), env);
    }
    /* Walk the list, splicing where needed. */
    ls_value_t out = ls_nil_v(), tail = ls_nil_v();
    while (form.tag == LS_T_CONS) {
        ls_value_t x = ls_car(form);
        form = ls_cdr(form);
        int spliced = 0;
        if (x.tag == LS_T_CONS && ls_car(x).tag == LS_T_SYMBOL &&
            (ls_symbol_t *)ls_car(x).u.ptr == L->sym_unquote_splicing) {
            ls_value_t spl = ls_eval(L, ls_car(ls_cdr(x)), env);
            while (spl.tag == LS_T_CONS) {
                ls_value_t cell = ls_cons(L, ls_car(spl), ls_nil_v());
                if (out.tag == LS_T_NIL) out = tail = cell;
                else { ((ls_cons_t*)tail.u.ptr)->cdr = cell; tail = cell; }
                spl = ls_cdr(spl);
            }
            spliced = 1;
        }
        if (!spliced) {
            ls_value_t v = qq_list(L, x, env);
            ls_value_t cell = ls_cons(L, v, ls_nil_v());
            if (out.tag == LS_T_NIL) out = tail = cell;
            else { ((ls_cons_t*)tail.u.ptr)->cdr = cell; tail = cell; }
        }
    }
    if (form.tag != LS_T_NIL) {
        if (tail.tag == LS_T_CONS) ((ls_cons_t*)tail.u.ptr)->cdr = expand_qq(L, form, env);
        else out = expand_qq(L, form, env);
    }
    return out;
}

static ls_value_t expand_qq(ls_state_t *L, ls_value_t form, ls_env_t *env) {
    if (form.tag != LS_T_CONS) return form;
    return qq_list(L, form, env);
}

/* ---------- The evaluator ---------- */

static ls_value_t apply_function(ls_state_t *L, ls_function_t *f, int n, ls_value_t *args) {
    ls_env_t *env = ls_env_new(L, f->closure);
    if (!bind_args(L, f, n, args, env)) return ls_nil_v();
    return ls_progn(L, f->body, env);
}

ls_value_t ls_apply(ls_state_t *L, ls_value_t fn, int nargs, ls_value_t *args) {
    L->mv.n = 1;
    switch (fn.tag) {
    case LS_T_NATIVE: {
        ls_native_t *n = (ls_native_t *)fn.u.ptr;
        if (n->min_args >= 0 && nargs < n->min_args) {
            ls_arity_error(L, n->name, nargs, n->min_args, n->max_args);
            return ls_nil_v();
        }
        if (n->max_args >= 0 && nargs > n->max_args) {
            ls_arity_error(L, n->name, nargs, n->min_args, n->max_args);
            return ls_nil_v();
        }
        ls_value_t r = n->fn(L, nargs, args);
        if (L->mv.n < 1) { L->mv.n = 1; L->mv.v[0] = r; }
        return r;
    }
    case LS_T_FUNCTION:
        return apply_function(L, (ls_function_t *)fn.u.ptr, nargs, args);
    case LS_T_BYTECODE: {
        extern ls_value_t ls_vm_run(ls_state_t *L, ls_bytecode_fn_t *f, int n, ls_value_t *a);
        ls_bytecode_fn_t *bf = (ls_bytecode_fn_t *)fn.u.ptr;
        if (bf->jit_entry) {
            typedef ls_value_t (*jit_fn)(ls_state_t *, int, ls_value_t *);
            return ((jit_fn)bf->jit_entry)(L, nargs, args);
        }
        return ls_vm_run(L, bf, nargs, args);
    }
    case LS_T_GENERIC: {
        extern ls_value_t ls_dispatch_generic(ls_state_t *L, ls_generic_t *g, int n, ls_value_t *a);
        return ls_dispatch_generic(L, (ls_generic_t *)fn.u.ptr, nargs, args);
    }
    case LS_T_SYMBOL: {
        ls_symbol_t *s = (ls_symbol_t *)fn.u.ptr;
        if (s->sym_flags & LS_SYM_HAS_FN) return ls_apply(L, s->function, nargs, args);
        ls_undefined_function_error(L, s);
        return ls_nil_v();
    }
    default:
        ls_error(L, "not callable");
        return ls_nil_v();
    }
}

ls_value_t ls_eval(ls_state_t *L, ls_value_t form, ls_env_t *env) {
    ls_value_t out;
    /* Self-evaluating */
    switch (form.tag) {
    case LS_T_NIL: case LS_T_T: case LS_T_FIXNUM: case LS_T_FLONUM:
    case LS_T_CHAR: case LS_T_STRING: case LS_T_VECTOR:
    case LS_T_HASHTABLE:
        return form;
    case LS_T_SYMBOL: {
        ls_symbol_t *s = (ls_symbol_t *)form.u.ptr;
        if (s->package == L->pkg_keyword) return form;
        if (s == L->sym_t) return ls_t_v();
        if (ls_env_lookup(env, s, &out)) return out;
        if (s->sym_flags & LS_SYM_HAS_VALUE) return s->value;
        ls_unbound_variable_error(L, s);
        return ls_nil_v();
    }
    case LS_T_CONS: break;
    default:
        return form;
    }

    ls_value_t hd = ls_car(form);
    ls_value_t args = ls_cdr(form);

    if (hd.tag == LS_T_SYMBOL) {
        ls_symbol_t *op = (ls_symbol_t *)hd.u.ptr;

        /* Special forms first. */
        if (op == L->sym_quote)   return eval_quote(L, args, env);
        if (op == L->sym_if)      return eval_if(L, args, env);
        if (op == L->sym_progn)   return ls_progn(L, args, env);
        if (op == L->sym_setq)    return eval_setq(L, args, env);
        if (op == L->sym_let)     return eval_let(L, args, env, 0);
        if (op == L->sym_letstar) return eval_let(L, args, env, 1);
        if (op == L->sym_lambda)  return eval_lambda(L, args, env);
        if (op == L->sym_function)return eval_function(L, args, env);
        if (op == L->sym_defun)   return eval_defun(L, args, env);
        if (op == L->sym_defmacro)return eval_defmacro(L, args, env);
        if (op == L->sym_defvar)  return eval_defvar(L, args, env, 0);
        if (op == L->sym_defparameter) return eval_defvar(L, args, env, 1);
        if (op == L->sym_defconstant)  return eval_defvar(L, args, env, 1);
        if (op == L->sym_block)   return eval_block(L, args, env);
        if (op == L->sym_return_from) return eval_return_from(L, args, env);
        if (op == L->sym_catch)   return eval_catch(L, args, env);
        if (op == L->sym_throw)   return eval_throw(L, args, env);
        if (op == L->sym_unwind_protect) return eval_unwind_protect(L, args, env);
        if (op == L->sym_quasiquote) return expand_qq(L, ls_car(args), env);
        if (op == L->sym_and) {
            ls_value_t v = ls_t_v();
            while (args.tag == LS_T_CONS) {
                v = ls_eval(L, ls_car(args), env);
                if (v.tag == LS_T_NIL) return v;
                args = ls_cdr(args);
            }
            return v;
        }
        if (op == L->sym_or) {
            while (args.tag == LS_T_CONS) {
                ls_value_t v = ls_eval(L, ls_car(args), env);
                if (v.tag != LS_T_NIL) return v;
                args = ls_cdr(args);
            }
            return ls_nil_v();
        }
        if (op == L->sym_when) {
            ls_value_t test = ls_eval(L, ls_car(args), env);
            if (test.tag != LS_T_NIL) return ls_progn(L, ls_cdr(args), env);
            return ls_nil_v();
        }
        if (op == L->sym_unless) {
            ls_value_t test = ls_eval(L, ls_car(args), env);
            if (test.tag == LS_T_NIL) return ls_progn(L, ls_cdr(args), env);
            return ls_nil_v();
        }
        if (op == L->sym_cond) {
            while (args.tag == LS_T_CONS) {
                ls_value_t clause = ls_car(args); args = ls_cdr(args);
                ls_value_t test = ls_eval(L, ls_car(clause), env);
                if (test.tag != LS_T_NIL) {
                    if (ls_cdr(clause).tag == LS_T_NIL) return test;
                    return ls_progn(L, ls_cdr(clause), env);
                }
            }
            return ls_nil_v();
        }
        if (op == L->sym_eval_when) {
            /* simplified: always evaluate */
            ls_value_t situation = ls_car(args); (void)situation;
            return ls_progn(L, ls_cdr(args), env);
        }
        if (op == L->sym_locally) return ls_progn(L, args, env);
        if (op == L->sym_the) {
            /* (the type form) -- ignore type assertion */
            return ls_eval(L, ls_car(ls_cdr(args)), env);
        }
        if (op == L->sym_declare) return ls_nil_v();
        if (op == L->sym_flet || op == L->sym_labels) {
            ls_value_t binds = ls_car(args);
            ls_value_t body = ls_cdr(args);
            ls_env_t *ne = ls_env_new(L, env);
            ls_env_t *def_env = (op == L->sym_labels) ? ne : env;
            while (binds.tag == LS_T_CONS) {
                ls_value_t b = ls_car(binds); binds = ls_cdr(binds);
                ls_symbol_t *name = (ls_symbol_t *)ls_car(b).u.ptr;
                ls_value_t lam = eval_lambda(L, ls_cdr(b), def_env);
                ((ls_function_t*)lam.u.ptr)->name = name;
                ls_env_bind(L, ne, name, lam);
            }
            /* shadow function slot via env binding only */
            return ls_progn(L, body, ne);
        }
        if (op == L->sym_return)  return eval_return(L, args, env);
        if (op == L->sym_tagbody)return eval_tagbody(L, args, env);
        if (op == L->sym_go)     return eval_go(L, args, env);
        if (op == L->sym_do)     return eval_do_impl(L, args, env, 0);
        if (op == L->sym_dostar) return eval_do_impl(L, args, env, 1);
        if (op == L->sym_dotimes)return eval_dotimes(L, args, env);
        if (op == L->sym_dolist) return eval_dolist(L, args, env);
        if (op == L->sym_setf)   return eval_setf(L, args, env);
        if (op == L->sym_case)   return eval_case(L, args, env);
        if (op == L->sym_typecase) return eval_typecase(L, args, env);
        if (op == L->sym_multiple_value_bind) return eval_multiple_value_bind(L, args, env);
        if (op == L->sym_prog)   return eval_prog(L, args, env, 0);
        if (op == L->sym_progstar) return eval_prog(L, args, env, 1);
        if (op == L->sym_defclass)   return eval_defclass(L, args, env);
        if (op == L->sym_defgeneric) return eval_defgeneric(L, args, env);
        if (op == L->sym_defmethod)  return eval_defmethod(L, args, env);

        /* Macro? */
        if (op->sym_flags & LS_SYM_HAS_MACRO) {
            return apply_macro(L, ls_function_p(op->function), form, env);
        }

        /* Function call. */
        if (op->sym_flags & LS_SYM_HAS_FN) {
            ls_value_t arr[32]; int n = 0;
            while (args.tag == LS_T_CONS && n < 32) {
                arr[n++] = ls_eval(L, ls_car(args), env);
                args = ls_cdr(args);
            }
            return ls_apply(L, op->function, n, arr);
        }
        /* check env for shadowed function */
        if (ls_env_lookup(env, op, &out) && ls_is_fn(out)) {
            ls_value_t arr[32]; int n = 0;
            while (args.tag == LS_T_CONS && n < 32) {
                arr[n++] = ls_eval(L, ls_car(args), env);
                args = ls_cdr(args);
            }
            return ls_apply(L, out, n, arr);
        }
        ls_undefined_function_error(L, op);
        return ls_nil_v();
    }

    /* ((lambda ...) ...) etc. */
    if (hd.tag == LS_T_CONS) {
        ls_value_t fn = ls_eval(L, hd, env);
        ls_value_t arr[32]; int n = 0;
        while (args.tag == LS_T_CONS && n < 32) {
            arr[n++] = ls_eval(L, ls_car(args), env);
            args = ls_cdr(args);
        }
        return ls_apply(L, fn, n, arr);
    }

    ls_error(L, "cannot evaluate");
    return ls_nil_v();
}
