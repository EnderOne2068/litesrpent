/* lsvm.c -- bytecode compiler + stack-based virtual machine for Litesrpent.
 *
 * The compiler walks an S-expression and emits a flat array of 32-bit
 * instructions into an ls_proto_t.  The VM executes those instructions
 * with a simple value stack.
 *
 * Special forms handled directly by the compiler:
 *   quote, if, progn, let, let*, setq, lambda, and, or,
 *   when, unless, cond, function
 *
 * Everything else is compiled as a function call.
 */
#include "lscore.h"
#include "lseval.h"
#include "lsopcodes.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================
 *  Compiler state
 * ================================================================ */

/* A local variable binding known at compile time. */
typedef struct {
    ls_symbol_t *sym;
    int          slot;      /* index into the locals array */
} ls_local_t;

/* An upvalue reference (captured from an enclosing scope). */
typedef struct {
    ls_symbol_t *sym;
    int          index;     /* index into upvals array at runtime */
    int          is_local;  /* 1 = captured from direct parent's locals */
    int          parent_slot; /* slot in parent locals/upvals */
} ls_upval_desc_t;

/* Maximum limits for a single compilation unit. */
#define COMP_MAX_CODE   4096
#define COMP_MAX_CONSTS 512
#define COMP_MAX_LOCALS 256
#define COMP_MAX_UPVALS 64
#define COMP_MAX_INNER  64

typedef struct ls_compiler ls_compiler_t;

struct ls_compiler {
    ls_state_t    *L;
    ls_compiler_t *parent;           /* enclosing compiler (for upvals) */

    uint32_t       code[COMP_MAX_CODE];
    int            code_len;

    ls_value_t     consts[COMP_MAX_CONSTS];
    int            n_consts;

    ls_local_t     locals[COMP_MAX_LOCALS];
    int            n_locals;
    int            scope_base;       /* first local of current scope */

    ls_upval_desc_t upvals[COMP_MAX_UPVALS];
    int            n_upvals;

    /* Nested lambda protos compiled inside this one. */
    ls_proto_t    *inner_protos[COMP_MAX_INNER];
    int            n_inner;

    int            n_args;           /* parameter count (for this lambda) */
    int            n_opt;
    int            has_rest;
    ls_symbol_t   *name;
};

/* ------------------------------------------------------------------ */
/*  Compiler helpers                                                   */
/* ------------------------------------------------------------------ */

static void emit(ls_compiler_t *c, uint32_t instr) {
    if (c->code_len >= COMP_MAX_CODE)
        ls_error(c->L, "bytecode compiler: code buffer overflow");
    c->code[c->code_len++] = instr;
}

static int emit_placeholder(ls_compiler_t *c) {
    int pos = c->code_len;
    emit(c, OP_MAKE(OP_NOP, 0));
    return pos;
}

static void patch_jump(ls_compiler_t *c, int pos, ls_opcode_t op) {
    int offset = c->code_len - pos - 1;
    c->code[pos] = OP_MAKE(op, (uint32_t)(offset & 0x00FFFFFF));
}

static void patch_jump_to(ls_compiler_t *c, int pos, ls_opcode_t op, int target) {
    int offset = target - pos - 1;
    c->code[pos] = OP_MAKE(op, (uint32_t)(offset & 0x00FFFFFF));
}

static int add_const(ls_compiler_t *c, ls_value_t v) {
    /* Deduplicate fixnums and symbols. */
    for (int i = 0; i < c->n_consts; i++) {
        if (c->consts[i].tag == v.tag && c->consts[i].u.ptr == v.u.ptr &&
            (v.tag == LS_T_SYMBOL || v.tag == LS_T_FIXNUM || v.tag == LS_T_NIL || v.tag == LS_T_T))
            return i;
    }
    if (c->n_consts >= COMP_MAX_CONSTS)
        ls_error(c->L, "bytecode compiler: constant pool overflow");
    c->consts[c->n_consts] = v;
    return c->n_consts++;
}

static int add_local(ls_compiler_t *c, ls_symbol_t *sym) {
    if (c->n_locals >= COMP_MAX_LOCALS)
        ls_error(c->L, "bytecode compiler: too many locals");
    int slot = c->n_locals;
    c->locals[c->n_locals].sym  = sym;
    c->locals[c->n_locals].slot = slot;
    c->n_locals++;
    return slot;
}

static int find_local(ls_compiler_t *c, ls_symbol_t *sym) {
    for (int i = c->n_locals - 1; i >= 0; i--) {
        if (c->locals[i].sym == sym)
            return c->locals[i].slot;
    }
    return -1;
}

/* Resolve or create an upvalue reference for sym.  Returns the upval
 * index or -1 if the symbol is not reachable lexically. */
static int resolve_upval(ls_compiler_t *c, ls_symbol_t *sym) {
    /* Already captured? */
    for (int i = 0; i < c->n_upvals; i++) {
        if (c->upvals[i].sym == sym)
            return i;
    }
    if (!c->parent) return -1;

    /* Try parent's locals first. */
    int pslot = find_local(c->parent, sym);
    if (pslot >= 0) {
        if (c->n_upvals >= COMP_MAX_UPVALS)
            ls_error(c->L, "bytecode compiler: too many upvalues");
        int idx = c->n_upvals++;
        c->upvals[idx].sym         = sym;
        c->upvals[idx].index       = idx;
        c->upvals[idx].is_local    = 1;
        c->upvals[idx].parent_slot = pslot;
        return idx;
    }

    /* Try parent's upvals (transitive capture). */
    int pidx = resolve_upval(c->parent, sym);
    if (pidx >= 0) {
        if (c->n_upvals >= COMP_MAX_UPVALS)
            ls_error(c->L, "bytecode compiler: too many upvalues");
        int idx = c->n_upvals++;
        c->upvals[idx].sym         = sym;
        c->upvals[idx].index       = idx;
        c->upvals[idx].is_local    = 0;
        c->upvals[idx].parent_slot = pidx;
        return idx;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Forward declaration                                                */
/* ------------------------------------------------------------------ */
static void compile_form(ls_compiler_t *c, ls_value_t form, int tail);

/* ------------------------------------------------------------------ */
/*  Compile atoms                                                      */
/* ------------------------------------------------------------------ */

static void compile_atom(ls_compiler_t *c, ls_value_t form) {
    switch (form.tag) {
    case LS_T_NIL:
        emit(c, OP_MAKE(OP_NIL, 0));
        return;
    case LS_T_T:
        emit(c, OP_MAKE(OP_T, 0));
        return;
    case LS_T_FIXNUM: {
        int64_t val = form.u.fixnum;
        if (val >= -8388608 && val <= 8388607) {
            emit(c, OP_MAKE(OP_FIXNUM, (uint32_t)((int32_t)val & 0x00FFFFFF)));
        } else {
            int ci = add_const(c, form);
            emit(c, OP_MAKE(OP_CONST, ci));
        }
        return;
    }
    case LS_T_FLONUM:
    case LS_T_STRING:
    case LS_T_CHAR: {
        int ci = add_const(c, form);
        emit(c, OP_MAKE(OP_CONST, ci));
        return;
    }
    case LS_T_SYMBOL: {
        ls_symbol_t *sym = (ls_symbol_t *)form.u.ptr;
        /* Is it a keyword?  Keywords evaluate to themselves. */
        if (sym->sym_flags & LS_SYM_KEYWORD) {
            int ci = add_const(c, form);
            emit(c, OP_MAKE(OP_CONST, ci));
            return;
        }
        /* Local? */
        int slot = find_local(c, sym);
        if (slot >= 0) {
            emit(c, OP_MAKE(OP_LOAD_LOCAL, slot));
            return;
        }
        /* Upvalue? */
        int uv = resolve_upval(c, sym);
        if (uv >= 0) {
            emit(c, OP_MAKE(OP_LOAD_UPVAL, uv));
            return;
        }
        /* Global. */
        int ci = add_const(c, form);
        emit(c, OP_MAKE(OP_LOAD_GLOBAL, ci));
        return;
    }
    default:
        /* Unknown atom -- shove it into the constant pool. */
        {
            int ci = add_const(c, form);
            emit(c, OP_MAKE(OP_CONST, ci));
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  Compile special forms                                              */
/* ------------------------------------------------------------------ */

/* (quote datum) */
static void compile_quote(ls_compiler_t *c, ls_value_t args) {
    if (args.tag != LS_T_CONS)
        ls_error(c->L, "quote: need exactly 1 argument");
    ls_value_t datum = LS_CAR(args);
    if (datum.tag == LS_T_NIL)
        emit(c, OP_MAKE(OP_NIL, 0));
    else if (datum.tag == LS_T_T)
        emit(c, OP_MAKE(OP_T, 0));
    else {
        int ci = add_const(c, datum);
        emit(c, OP_MAKE(OP_CONST, ci));
    }
}

/* (if test then [else]) */
static void compile_if(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "if: need test");
    ls_value_t test = LS_CAR(args);
    ls_value_t rest = LS_CDR(args);
    if (rest.tag != LS_T_CONS) ls_error(c->L, "if: need then");
    ls_value_t then_form = LS_CAR(rest);
    ls_value_t else_tail = LS_CDR(rest);
    int has_else = (else_tail.tag == LS_T_CONS);

    compile_form(c, test, 0);
    int jmp_else = emit_placeholder(c);  /* JUMP_IF_NIL -> else */

    compile_form(c, then_form, tail);
    int jmp_end = -1;
    if (has_else) {
        jmp_end = emit_placeholder(c);   /* JUMP -> end */
    }

    patch_jump(c, jmp_else, OP_JUMP_IF_NIL);

    if (has_else) {
        compile_form(c, LS_CAR(else_tail), tail);
        patch_jump(c, jmp_end, OP_JUMP);
    } else {
        emit(c, OP_MAKE(OP_NIL, 0));     /* no else => nil */
    }
}

/* (progn form*) */
static void compile_progn(ls_compiler_t *c, ls_value_t forms, int tail) {
    if (forms.tag == LS_T_NIL) {
        emit(c, OP_MAKE(OP_NIL, 0));
        return;
    }
    while (forms.tag == LS_T_CONS) {
        ls_value_t form = LS_CAR(forms);
        ls_value_t rest = LS_CDR(forms);
        int is_last = (rest.tag != LS_T_CONS);
        compile_form(c, form, tail && is_last);
        if (!is_last) emit(c, OP_MAKE(OP_POP, 0));
        forms = rest;
    }
}

/* (let ((var init)...) body...) */
static void compile_let(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "let: missing bindings");
    ls_value_t bindings = LS_CAR(args);
    ls_value_t body     = LS_CDR(args);

    int saved_n_locals = c->n_locals;
    int saved_scope    = c->scope_base;
    c->scope_base = c->n_locals;

    /* Evaluate all inits, then bind all at once (parallel let). */
    int count = 0;
    ls_value_t b = bindings;
    while (b.tag == LS_T_CONS) {
        ls_value_t entry = LS_CAR(b);
        if (entry.tag == LS_T_SYMBOL) {
            /* (let ((x) ...) ...) - var bound to nil */
            emit(c, OP_MAKE(OP_NIL, 0));
        } else if (entry.tag == LS_T_CONS) {
            ls_value_t init_tail = LS_CDR(entry);
            if (init_tail.tag == LS_T_CONS)
                compile_form(c, LS_CAR(init_tail), 0);
            else
                emit(c, OP_MAKE(OP_NIL, 0));
        } else {
            ls_error(c->L, "let: bad binding form");
        }
        count++;
        b = LS_CDR(b);
    }

    /* Now create slots and store (in reverse order off the stack). */
    int *slots = (int *)alloca(count * sizeof(int));
    b = bindings;
    for (int i = 0; i < count; i++) {
        ls_value_t entry = LS_CAR(b);
        ls_symbol_t *sym;
        if (entry.tag == LS_T_SYMBOL)
            sym = (ls_symbol_t *)entry.u.ptr;
        else
            sym = (ls_symbol_t *)(LS_CAR(entry)).u.ptr;
        slots[i] = add_local(c, sym);
        b = LS_CDR(b);
    }
    /* Values are on the stack in order slots[0]..slots[count-1]; store
     * them from bottom to top so the first pushed value goes to slots[0]. */
    for (int i = count - 1; i >= 0; i--)
        emit(c, OP_MAKE(OP_STORE_LOCAL, slots[i]));

    compile_progn(c, body, tail);

    c->n_locals   = saved_n_locals;
    c->scope_base = saved_scope;
}

/* (let* ((var init)...) body...) -- sequential binding */
static void compile_letstar(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "let*: missing bindings");
    ls_value_t bindings = LS_CAR(args);
    ls_value_t body     = LS_CDR(args);

    int saved_n_locals = c->n_locals;
    int saved_scope    = c->scope_base;
    c->scope_base = c->n_locals;

    ls_value_t b = bindings;
    while (b.tag == LS_T_CONS) {
        ls_value_t entry = LS_CAR(b);
        ls_symbol_t *sym;
        ls_value_t init = ls_nil_v();
        if (entry.tag == LS_T_SYMBOL) {
            sym = (ls_symbol_t *)entry.u.ptr;
        } else if (entry.tag == LS_T_CONS) {
            sym = (ls_symbol_t *)(LS_CAR(entry)).u.ptr;
            ls_value_t init_tail = LS_CDR(entry);
            if (init_tail.tag == LS_T_CONS)
                init = LS_CAR(init_tail);
        } else {
            ls_error(c->L, "let*: bad binding form");
            return;
        }
        compile_form(c, init, 0);
        int slot = add_local(c, sym);
        emit(c, OP_MAKE(OP_STORE_LOCAL, slot));
        b = LS_CDR(b);
    }

    compile_progn(c, body, tail);

    c->n_locals   = saved_n_locals;
    c->scope_base = saved_scope;
}

/* (setq var val [var val ...]) */
static void compile_setq(ls_compiler_t *c, ls_value_t args) {
    while (args.tag == LS_T_CONS) {
        ls_value_t var_v = LS_CAR(args);
        args = LS_CDR(args);
        if (var_v.tag != LS_T_SYMBOL)
            ls_error(c->L, "setq: not a symbol");
        ls_symbol_t *sym = (ls_symbol_t *)var_v.u.ptr;

        if (args.tag != LS_T_CONS)
            ls_error(c->L, "setq: odd number of arguments");
        ls_value_t val = LS_CAR(args);
        args = LS_CDR(args);

        compile_form(c, val, 0);
        emit(c, OP_MAKE(OP_DUP, 0));   /* setq returns the value */

        int slot = find_local(c, sym);
        if (slot >= 0) {
            emit(c, OP_MAKE(OP_STORE_LOCAL, slot));
        } else {
            int uv = resolve_upval(c, sym);
            if (uv >= 0) {
                emit(c, OP_MAKE(OP_STORE_UPVAL, uv));
            } else {
                int ci = add_const(c, var_v);
                emit(c, OP_MAKE(OP_STORE_GLOBAL, ci));
            }
        }

        /* If there are more pairs, pop the intermediate result. */
        if (args.tag == LS_T_CONS)
            emit(c, OP_MAKE(OP_POP, 0));
    }
}

/* (and form*) -- short-circuit: returns nil on first nil, last value
 * otherwise. */
static void compile_and(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag == LS_T_NIL) {
        emit(c, OP_MAKE(OP_T, 0));     /* (and) => t */
        return;
    }
    int patch_slots[256];
    int n_patches = 0;

    while (args.tag == LS_T_CONS) {
        ls_value_t form = LS_CAR(args);
        ls_value_t rest = LS_CDR(args);
        int is_last = (rest.tag != LS_T_CONS);

        compile_form(c, form, tail && is_last);

        if (!is_last) {
            emit(c, OP_MAKE(OP_DUP, 0));
            patch_slots[n_patches++] = emit_placeholder(c); /* JUMP_IF_NIL -> end */
            emit(c, OP_MAKE(OP_POP, 0));
        }
        args = rest;
    }
    for (int i = 0; i < n_patches; i++)
        patch_jump(c, patch_slots[i], OP_JUMP_IF_NIL);
}

/* (or form*) -- short-circuit: returns first non-nil, else nil. */
static void compile_or(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag == LS_T_NIL) {
        emit(c, OP_MAKE(OP_NIL, 0));   /* (or) => nil */
        return;
    }
    int patch_slots[256];
    int n_patches = 0;

    while (args.tag == LS_T_CONS) {
        ls_value_t form = LS_CAR(args);
        ls_value_t rest = LS_CDR(args);
        int is_last = (rest.tag != LS_T_CONS);

        compile_form(c, form, tail && is_last);

        if (!is_last) {
            emit(c, OP_MAKE(OP_DUP, 0));
            patch_slots[n_patches++] = emit_placeholder(c); /* JUMP_IF_NOT_NIL -> end */
            emit(c, OP_MAKE(OP_POP, 0));
        }
        args = rest;
    }
    for (int i = 0; i < n_patches; i++)
        patch_jump(c, patch_slots[i], OP_JUMP_IF_NOT_NIL);
}

/* (when test body...) => (if test (progn body...)) */
static void compile_when(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "when: need test");
    ls_value_t test = LS_CAR(args);
    ls_value_t body = LS_CDR(args);

    compile_form(c, test, 0);
    int jmp = emit_placeholder(c);
    compile_progn(c, body, tail);
    int jmp_end = emit_placeholder(c);
    patch_jump(c, jmp, OP_JUMP_IF_NIL);
    emit(c, OP_MAKE(OP_NIL, 0));
    patch_jump(c, jmp_end, OP_JUMP);
}

/* (unless test body...) => (if (not test) (progn body...)) */
static void compile_unless(ls_compiler_t *c, ls_value_t args, int tail) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "unless: need test");
    ls_value_t test = LS_CAR(args);
    ls_value_t body = LS_CDR(args);

    compile_form(c, test, 0);
    int jmp = emit_placeholder(c);
    emit(c, OP_MAKE(OP_NIL, 0));
    int jmp_end = emit_placeholder(c);
    patch_jump(c, jmp, OP_JUMP_IF_NIL);
    compile_progn(c, body, tail);
    patch_jump(c, jmp_end, OP_JUMP);
}

/* (cond (test form*)...) */
static void compile_cond(ls_compiler_t *c, ls_value_t clauses, int tail) {
    int end_patches[256];
    int n_end = 0;

    while (clauses.tag == LS_T_CONS) {
        ls_value_t clause = LS_CAR(clauses);
        clauses = LS_CDR(clauses);
        if (clause.tag != LS_T_CONS)
            ls_error(c->L, "cond: bad clause");

        ls_value_t test = LS_CAR(clause);
        ls_value_t body = LS_CDR(clause);
        int is_last = (clauses.tag != LS_T_CONS);

        /* Check for (t ...) -- the default clause. */
        if (test.tag == LS_T_T || (test.tag == LS_T_SYMBOL &&
            (ls_symbol_t *)test.u.ptr == c->L->sym_t)) {
            if (body.tag == LS_T_NIL)
                emit(c, OP_MAKE(OP_T, 0));
            else
                compile_progn(c, body, tail);
            goto cond_end;
        }

        compile_form(c, test, 0);
        if (body.tag == LS_T_NIL) {
            /* (cond (test) ...) => test value itself is the result if
             * non-nil; but we need it on the stack either way. */
            emit(c, OP_MAKE(OP_DUP, 0));
            int skip = emit_placeholder(c);
            emit(c, OP_MAKE(OP_POP, 0));
            if (!is_last) {
                /* continue to next clause */
                patch_jump(c, skip, OP_JUMP_IF_NOT_NIL);
            } else {
                patch_jump(c, skip, OP_JUMP_IF_NOT_NIL);
                emit(c, OP_MAKE(OP_NIL, 0));
            }
            if (!is_last) {
                end_patches[n_end++] = emit_placeholder(c);
                /* Fall through to the nil-pop path. */
            }
            continue;
        }

        int next_clause = emit_placeholder(c);
        compile_progn(c, body, tail && is_last);
        if (!is_last)
            end_patches[n_end++] = emit_placeholder(c);
        patch_jump(c, next_clause, OP_JUMP_IF_NIL);
    }
    /* No clause matched => nil. */
    emit(c, OP_MAKE(OP_NIL, 0));

cond_end:
    for (int i = 0; i < n_end; i++)
        patch_jump(c, end_patches[i], OP_JUMP);
}

/* Compile a lambda and return the proto index in the parent's inner
 * proto array.  The lambda form is ((params...) body...). */
static int compile_lambda_inner(ls_compiler_t *parent, ls_value_t lambda_list,
                                ls_value_t body, ls_symbol_t *name);

/* (lambda (params...) body...) */
static void compile_lambda(ls_compiler_t *c, ls_value_t args) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "lambda: need params");
    ls_value_t params = LS_CAR(args);
    ls_value_t body   = LS_CDR(args);

    int proto_idx = compile_lambda_inner(c, params, body, NULL);
    emit(c, OP_MAKE(OP_CLOSURE, proto_idx));
}

/* (function name-or-lambda) */
static void compile_function_form(ls_compiler_t *c, ls_value_t args) {
    if (args.tag != LS_T_CONS) ls_error(c->L, "function: need argument");
    ls_value_t arg = LS_CAR(args);

    if (arg.tag == LS_T_SYMBOL) {
        /* #'name -- look up the function cell. */
        ls_symbol_t *sym = (ls_symbol_t *)arg.u.ptr;
        int slot = find_local(c, sym);
        if (slot >= 0) {
            emit(c, OP_MAKE(OP_LOAD_LOCAL, slot));
            return;
        }
        int uv = resolve_upval(c, sym);
        if (uv >= 0) {
            emit(c, OP_MAKE(OP_LOAD_UPVAL, uv));
            return;
        }
        /* Fall back to global function slot. */
        int ci = add_const(c, arg);
        emit(c, OP_MAKE(OP_LOAD_GLOBAL, ci));
        return;
    }
    if (arg.tag == LS_T_CONS) {
        /* #'(lambda ...) */
        ls_value_t head = LS_CAR(arg);
        if (head.tag == LS_T_SYMBOL && (ls_symbol_t *)head.u.ptr == c->L->sym_lambda) {
            compile_lambda(c, LS_CDR(arg));
            return;
        }
    }
    ls_error(c->L, "function: bad argument");
}

/* Compile a function call: (fn arg1 arg2 ...) */
static void compile_call(ls_compiler_t *c, ls_value_t head, ls_value_t args, int tail) {
    /* Compile the function position. */
    if (head.tag == LS_T_SYMBOL) {
        /* Try to resolve the symbol to look up function slot. */
        ls_symbol_t *sym = (ls_symbol_t *)head.u.ptr;
        int slot = find_local(c, sym);
        if (slot >= 0) {
            emit(c, OP_MAKE(OP_LOAD_LOCAL, slot));
        } else {
            int uv = resolve_upval(c, sym);
            if (uv >= 0) {
                emit(c, OP_MAKE(OP_LOAD_UPVAL, uv));
            } else {
                int ci = add_const(c, head);
                emit(c, OP_MAKE(OP_LOAD_GLOBAL, ci));
            }
        }
    } else {
        compile_form(c, head, 0);
    }

    /* Compile arguments left-to-right. */
    int nargs = 0;
    ls_value_t a = args;
    while (a.tag == LS_T_CONS) {
        compile_form(c, LS_CAR(a), 0);
        nargs++;
        a = LS_CDR(a);
    }

    if (tail)
        emit(c, OP_MAKE(OP_TAILCALL, nargs));
    else
        emit(c, OP_MAKE(OP_CALL, nargs));
}

/* ------------------------------------------------------------------ */
/*  Main form dispatcher                                               */
/* ------------------------------------------------------------------ */

static void compile_form(ls_compiler_t *c, ls_value_t form, int tail) {
    ls_state_t *L = c->L;

    if (form.tag != LS_T_CONS) {
        compile_atom(c, form);
        return;
    }

    ls_value_t head = LS_CAR(form);
    ls_value_t args = LS_CDR(form);

    if (head.tag == LS_T_SYMBOL) {
        ls_symbol_t *sym = (ls_symbol_t *)head.u.ptr;

        /* Special forms. */
        if (sym == L->sym_quote)    { compile_quote(c, args); return; }
        if (sym == L->sym_if)       { compile_if(c, args, tail); return; }
        if (sym == L->sym_progn)    { compile_progn(c, args, tail); return; }
        if (sym == L->sym_let)      { compile_let(c, args, tail); return; }
        if (sym == L->sym_letstar)  { compile_letstar(c, args, tail); return; }
        if (sym == L->sym_setq)     { compile_setq(c, args); return; }
        if (sym == L->sym_lambda)   { compile_lambda(c, args); return; }
        if (sym == L->sym_function) { compile_function_form(c, args); return; }
        if (sym == L->sym_and)      { compile_and(c, args, tail); return; }
        if (sym == L->sym_or)       { compile_or(c, args, tail); return; }
        if (sym == L->sym_when)     { compile_when(c, args, tail); return; }
        if (sym == L->sym_unless)   { compile_unless(c, args, tail); return; }
        if (sym == L->sym_cond)     { compile_cond(c, args, tail); return; }

        /* Recognize builtin two-arg arithmetic/comparison for direct
         * opcodes instead of a full call. */
        ls_value_t a2 = LS_CDR(args);  /* rest after first arg */
        if (args.tag == LS_T_CONS && a2.tag == LS_T_CONS &&
            LS_CDR(a2).tag == LS_T_NIL) {
            /* Exactly two arguments. */
            ls_opcode_t binop = OP_NOP;
            ls_symbol_t *sn = sym;
            const char *nm = sn->name ? sn->name->chars : "";
            if      (!strcmp(nm, "+"))  binop = OP_ADD;
            else if (!strcmp(nm, "-"))  binop = OP_SUB;
            else if (!strcmp(nm, "*"))  binop = OP_MUL;
            else if (!strcmp(nm, "/"))  binop = OP_DIV;
            else if (!strcmp(nm, "MOD"))binop = OP_MOD;
            else if (!strcmp(nm, "REM"))binop = OP_MOD;
            else if (!strcmp(nm, "EQ")) binop = OP_EQ;
            else if (!strcmp(nm, "<"))  binop = OP_LT;
            else if (!strcmp(nm, "<=")) binop = OP_LE;
            else if (!strcmp(nm, ">"))  binop = OP_GT;
            else if (!strcmp(nm, ">=")) binop = OP_GE;
            else if (!strcmp(nm, "="))  binop = OP_NUMEQ;

            if (binop != OP_NOP) {
                compile_form(c, LS_CAR(args), 0);
                compile_form(c, LS_CAR(a2),   0);
                emit(c, OP_MAKE(binop, 0));
                return;
            }
        }
        /* One-arg builtins. */
        if (args.tag == LS_T_CONS && LS_CDR(args).tag == LS_T_NIL) {
            const char *nm = sym->name ? sym->name->chars : "";
            if (!strcmp(nm, "CAR")) {
                compile_form(c, LS_CAR(args), 0);
                emit(c, OP_MAKE(OP_CAR, 0));
                return;
            }
            if (!strcmp(nm, "CDR")) {
                compile_form(c, LS_CAR(args), 0);
                emit(c, OP_MAKE(OP_CDR, 0));
                return;
            }
            if (!strcmp(nm, "NOT") || !strcmp(nm, "NULL")) {
                compile_form(c, LS_CAR(args), 0);
                emit(c, OP_MAKE(OP_NOT, 0));
                return;
            }
            if (!strcmp(nm, "-")) {
                compile_form(c, LS_CAR(args), 0);
                emit(c, OP_MAKE(OP_NEG, 0));
                return;
            }
        }
        /* Two-arg cons. */
        if (args.tag == LS_T_CONS && a2.tag == LS_T_CONS &&
            LS_CDR(a2).tag == LS_T_NIL) {
            const char *nm = sym->name ? sym->name->chars : "";
            if (!strcmp(nm, "CONS")) {
                compile_form(c, LS_CAR(args), 0);
                compile_form(c, LS_CAR(a2),   0);
                emit(c, OP_MAKE(OP_CONS, 0));
                return;
            }
        }
        /* N-arg list. */
        {
            const char *nm = sym->name ? sym->name->chars : "";
            if (!strcmp(nm, "LIST")) {
                int n = 0;
                ls_value_t a = args;
                while (a.tag == LS_T_CONS) {
                    compile_form(c, LS_CAR(a), 0);
                    n++;
                    a = LS_CDR(a);
                }
                emit(c, OP_MAKE(OP_LIST, n));
                return;
            }
        }
    }

    /* If the head is a lambda expression: ((lambda (...) body) args...) */
    if (head.tag == LS_T_CONS) {
        ls_value_t hcar = LS_CAR(head);
        if (hcar.tag == LS_T_SYMBOL &&
            (ls_symbol_t *)hcar.u.ptr == L->sym_lambda) {
            /* Compile as a closure then immediately call it. */
            compile_lambda(c, LS_CDR(head));
            int nargs = 0;
            ls_value_t a = args;
            while (a.tag == LS_T_CONS) {
                compile_form(c, LS_CAR(a), 0);
                nargs++;
                a = LS_CDR(a);
            }
            if (tail)
                emit(c, OP_MAKE(OP_TAILCALL, nargs));
            else
                emit(c, OP_MAKE(OP_CALL, nargs));
            return;
        }
    }

    /* Generic function call. */
    compile_call(c, head, args, tail);
}

/* ------------------------------------------------------------------ */
/*  Inner lambda compilation                                           */
/* ------------------------------------------------------------------ */

static int compile_lambda_inner(ls_compiler_t *parent, ls_value_t lambda_list,
                                ls_value_t body, ls_symbol_t *name) {
    ls_state_t *L = parent->L;

    ls_compiler_t child;
    memset(&child, 0, sizeof child);
    child.L       = L;
    child.parent  = parent;
    child.name    = name;

    /* Parse the lambda list and allocate locals for parameters. */
    int phase = 0;  /* 0=req, 1=opt, 2=rest */
    ls_value_t ll = lambda_list;
    while (ll.tag == LS_T_CONS) {
        ls_value_t p = LS_CAR(ll);
        ll = LS_CDR(ll);

        if (p.tag == LS_T_SYMBOL) {
            ls_symbol_t *ps = (ls_symbol_t *)p.u.ptr;
            if (ps == L->sym_ampersand_rest) {
                child.has_rest = 1;
                phase = 2;
                continue;
            }
            if (ps == L->sym_ampersand_optional) {
                phase = 1;
                continue;
            }
            if (ps == L->sym_ampersand_key || ps == L->sym_ampersand_aux ||
                ps == L->sym_ampersand_body || ps == L->sym_ampersand_allow_other_keys) {
                /* For simplicity, treat &key/&aux etc. like optionals. */
                phase = 1;
                continue;
            }
            add_local(&child, ps);
            if (phase == 0) child.n_args++;
            else child.n_opt++;
        } else if (p.tag == LS_T_CONS) {
            /* (name default) -- optional with default */
            ls_value_t pn = LS_CAR(p);
            if (pn.tag == LS_T_SYMBOL) {
                add_local(&child, (ls_symbol_t *)pn.u.ptr);
                child.n_opt++;
            }
        }
    }

    /* Compile the body. */
    compile_progn(&child, body, 1);
    emit(&child, OP_MAKE(OP_RETURN, 0));

    /* Build the proto object. */
    ls_value_t pv = ls_make_obj(L, LS_T_BYTECODE, sizeof(ls_proto_t));
    ls_proto_t *proto = (ls_proto_t *)pv.u.ptr;
    proto->h.tag = LS_T_BYTECODE;

    proto->code_len = (uint32_t)child.code_len;
    proto->code = (uint32_t *)malloc(child.code_len * sizeof(uint32_t));
    memcpy(proto->code, child.code, child.code_len * sizeof(uint32_t));

    proto->n_consts = (uint32_t)child.n_consts;
    if (child.n_consts > 0) {
        proto->consts = (ls_value_t *)malloc(child.n_consts * sizeof(ls_value_t));
        memcpy(proto->consts, child.consts, child.n_consts * sizeof(ls_value_t));
    }

    proto->n_upvals = (uint32_t)child.n_upvals;
    if (child.n_upvals > 0) {
        proto->upvals = (ls_symbol_t **)malloc(child.n_upvals * sizeof(ls_symbol_t *));
        for (int i = 0; i < child.n_upvals; i++)
            proto->upvals[i] = child.upvals[i].sym;
    }

    proto->n_args    = (uint32_t)child.n_args;
    proto->n_opt     = (uint32_t)child.n_opt;
    proto->n_locals  = (uint32_t)child.n_locals;
    proto->has_rest  = (uint32_t)child.has_rest;
    proto->name      = name;
    proto->source    = ls_nil_v();
    proto->line_info = NULL;
    proto->line_info_len = 0;

    /* Store any nested protos compiled inside this child into the
     * child's constant pool (they are referenced by OP_CLOSURE). */
    /* The inner_protos of *child* are already referenced by their
     * constant-pool index (added during compile_lambda_inner recursion
     * from the child).  Nothing extra to do. */

    /* Register this proto in the parent's inner array. */
    if (parent->n_inner >= COMP_MAX_INNER)
        ls_error(L, "bytecode compiler: too many nested lambdas");
    int idx = parent->n_inner++;
    parent->inner_protos[idx] = proto;

    /* Also add the proto as a constant in the parent so OP_CLOSURE can
     * reference it by constant-pool index. */
    int ci = add_const(parent, pv);
    (void)ci;  /* The CLOSURE instruction uses the inner index. */

    return idx;
}

/* ================================================================
 *  Top-level compilation entry point
 * ================================================================ */

ls_value_t ls_compile_to_bytecode(ls_state_t *L, ls_value_t form) {
    ls_compiler_t top;
    memset(&top, 0, sizeof top);
    top.L = L;

    compile_form(&top, form, 1);
    emit(&top, OP_MAKE(OP_RETURN, 0));

    /* Build the top-level proto. */
    ls_value_t pv = ls_make_obj(L, LS_T_BYTECODE, sizeof(ls_proto_t));
    ls_proto_t *proto = (ls_proto_t *)pv.u.ptr;
    proto->h.tag = LS_T_BYTECODE;

    proto->code_len = (uint32_t)top.code_len;
    proto->code = (uint32_t *)malloc(top.code_len * sizeof(uint32_t));
    memcpy(proto->code, top.code, top.code_len * sizeof(uint32_t));

    proto->n_consts = (uint32_t)top.n_consts;
    if (top.n_consts > 0) {
        proto->consts = (ls_value_t *)malloc(top.n_consts * sizeof(ls_value_t));
        memcpy(proto->consts, top.consts, top.n_consts * sizeof(ls_value_t));
    }

    proto->n_upvals  = 0;
    proto->upvals    = NULL;
    proto->n_args    = 0;
    proto->n_opt     = 0;
    proto->n_locals  = (uint32_t)top.n_locals;
    proto->has_rest  = 0;
    proto->name      = NULL;
    proto->source    = form;
    proto->line_info = NULL;
    proto->line_info_len = 0;

    /* Wrap in a bytecode closure. */
    ls_value_t fv = ls_make_obj(L, LS_T_BYTECODE, sizeof(ls_bytecode_fn_t));
    ls_bytecode_fn_t *fn = (ls_bytecode_fn_t *)fv.u.ptr;
    fn->proto      = proto;
    fn->upvals     = NULL;
    fn->nupvals    = 0;
    fn->jit_entry  = NULL;
    fn->call_count = 0;
    fn->name       = NULL;

    return fv;
}

/* ================================================================
 *  Virtual machine
 * ================================================================ */

#define VM_STACK_SIZE 1024

ls_value_t ls_vm_run(ls_state_t *L, ls_bytecode_fn_t *fn, int nargs, ls_value_t *args) {
    ls_proto_t  *proto  = fn->proto;
    uint32_t    *code   = proto->code;
    ls_value_t  *consts = proto->consts;
    ls_value_t  *upvals = fn->upvals;

    ls_value_t stack[VM_STACK_SIZE];
    int sp = 0;  /* stack pointer: points to the next free slot */

    /* Allocate space for locals.  Locals live at the bottom of the
     * stack and are directly indexed. */
    int n_locals = (int)proto->n_locals;
    for (int i = 0; i < n_locals; i++)
        stack[sp++] = ls_nil_v();

    /* Bind arguments into the first locals slots. */
    int n_params = (int)proto->n_args + (int)proto->n_opt;
    for (int i = 0; i < nargs && i < n_params; i++)
        stack[i] = args[i];

    /* &rest parameter: collect remaining args into a list. */
    if (proto->has_rest && n_params < n_locals) {
        ls_value_t rest = ls_nil_v();
        for (int i = nargs - 1; i >= n_params; i--)
            rest = ls_cons(L, args[i], rest);
        stack[n_params] = rest;
    }

    int pc = 0;

    /* Handler stack for PUSH_HANDLER / POP_HANDLER. */
    typedef struct vm_handler {
        int target_pc;
        int saved_sp;
    } vm_handler_t;
    vm_handler_t handlers[64];
    int hsp = 0;

    fn->call_count++;

    for (;;) {
        if (pc < 0 || pc >= (int)proto->code_len)
            ls_error(L, "VM: program counter out of bounds (pc=%d, len=%u)",
                     pc, proto->code_len);
        if (sp >= VM_STACK_SIZE - 4)
            ls_error(L, "VM: stack overflow");

        uint32_t instr = code[pc++];
        uint32_t op    = OP_GET_OP(instr);
        uint32_t arg   = OP_GET_ARG(instr);
        int32_t  sarg  = OP_GET_SARG(instr);

        switch (op) {

        case OP_NOP:
            break;

        /* -- Constants / immediates ------------------------------------ */
        case OP_CONST:
            if (arg >= proto->n_consts)
                ls_error(L, "VM: CONST index %u out of range", arg);
            stack[sp++] = consts[arg];
            break;

        case OP_NIL:
            stack[sp++] = ls_nil_v();
            break;

        case OP_T:
            stack[sp++] = ls_t_v();
            break;

        case OP_FIXNUM:
            stack[sp++] = ls_make_fixnum((int64_t)sarg);
            break;

        /* -- Locals / upvals / globals --------------------------------- */
        case OP_LOAD_LOCAL:
            if ((int)arg >= n_locals)
                ls_error(L, "VM: LOAD_LOCAL slot %u >= n_locals %d", arg, n_locals);
            stack[sp++] = stack[arg];
            break;

        case OP_STORE_LOCAL:
            if (sp < 1) ls_error(L, "VM: stack underflow on STORE_LOCAL");
            if ((int)arg >= n_locals)
                ls_error(L, "VM: STORE_LOCAL slot %u >= n_locals %d", arg, n_locals);
            stack[arg] = stack[--sp];
            break;

        case OP_LOAD_UPVAL:
            if (!upvals || arg >= fn->nupvals)
                ls_error(L, "VM: LOAD_UPVAL index %u out of range", arg);
            stack[sp++] = upvals[arg];
            break;

        case OP_STORE_UPVAL:
            if (sp < 1) ls_error(L, "VM: stack underflow on STORE_UPVAL");
            if (!upvals || arg >= fn->nupvals)
                ls_error(L, "VM: STORE_UPVAL index %u out of range", arg);
            upvals[arg] = stack[--sp];
            break;

        case OP_LOAD_GLOBAL: {
            if (arg >= proto->n_consts)
                ls_error(L, "VM: LOAD_GLOBAL const index %u out of range", arg);
            ls_value_t sym_v = consts[arg];
            if (sym_v.tag != LS_T_SYMBOL)
                ls_error(L, "VM: LOAD_GLOBAL const is not a symbol");
            ls_symbol_t *sym = (ls_symbol_t *)sym_v.u.ptr;
            /* Check function slot first, then value slot. */
            if (sym->sym_flags & LS_SYM_HAS_FN) {
                stack[sp++] = sym->function;
            } else if (sym->sym_flags & LS_SYM_HAS_VALUE) {
                stack[sp++] = sym->value;
            } else if (sym->value.tag != LS_T_NIL || sym->function.tag != LS_T_NIL) {
                /* Try value then function even without flags (common
                 * for symbols bound by setq at top level). */
                if (sym->value.tag != LS_T_NIL)
                    stack[sp++] = sym->value;
                else
                    stack[sp++] = sym->function;
            } else {
                ls_unbound_variable_error(L, sym);
            }
            break;
        }

        case OP_STORE_GLOBAL: {
            if (sp < 1) ls_error(L, "VM: stack underflow on STORE_GLOBAL");
            if (arg >= proto->n_consts)
                ls_error(L, "VM: STORE_GLOBAL const index %u out of range", arg);
            ls_value_t sym_v = consts[arg];
            if (sym_v.tag != LS_T_SYMBOL)
                ls_error(L, "VM: STORE_GLOBAL const is not a symbol");
            ls_symbol_t *sym = (ls_symbol_t *)sym_v.u.ptr;
            if (sym->sym_flags & LS_SYM_CONSTANT)
                ls_error(L, "VM: cannot assign to constant %s",
                         sym->name ? sym->name->chars : "?");
            sym->value = stack[--sp];
            sym->sym_flags |= LS_SYM_HAS_VALUE;
            break;
        }

        /* -- Stack manipulation ---------------------------------------- */
        case OP_POP:
            if (sp < 1) ls_error(L, "VM: stack underflow on POP");
            sp--;
            break;

        case OP_DUP:
            if (sp < 1) ls_error(L, "VM: stack underflow on DUP");
            stack[sp] = stack[sp - 1];
            sp++;
            break;

        /* -- Branching ------------------------------------------------- */
        case OP_JUMP:
            pc += sarg;
            break;

        case OP_JUMP_IF_NIL:
            if (sp < 1) ls_error(L, "VM: stack underflow on JUMP_IF_NIL");
            if (stack[--sp].tag == LS_T_NIL)
                pc += sarg;
            break;

        case OP_JUMP_IF_NOT_NIL:
            if (sp < 1) ls_error(L, "VM: stack underflow on JUMP_IF_NOT_NIL");
            if (stack[--sp].tag != LS_T_NIL)
                pc += sarg;
            break;

        /* -- Calls ----------------------------------------------------- */
        case OP_CALL:
        case OP_TAILCALL: {
            int na = (int)arg;
            if (sp < na + 1)
                ls_error(L, "VM: stack underflow on CALL (sp=%d, nargs=%d)", sp, na);
            ls_value_t *call_args = &stack[sp - na];
            ls_value_t callee = stack[sp - na - 1];
            sp -= (na + 1);

            ls_value_t result;
            if (callee.tag == LS_T_BYTECODE) {
                ls_bytecode_fn_t *bfn = (ls_bytecode_fn_t *)callee.u.ptr;

                /* JIT threshold check. */
                if (L->jit_enabled && bfn->call_count >= (uint32_t)L->jit_threshold &&
                    bfn->jit_entry == NULL) {
                    ls_jit_function(L, callee);
                }

                if (op == OP_TAILCALL && bfn == fn) {
                    /* Self tail-call: reuse the current frame. */
                    for (int i = 0; i < na && i < n_locals; i++)
                        stack[i] = call_args[i];
                    for (int i = na; i < n_locals; i++)
                        stack[i] = ls_nil_v();
                    sp = n_locals;
                    pc = 0;
                    bfn->call_count++;
                    break;
                }

                result = ls_vm_run(L, bfn, na, call_args);
            } else if (callee.tag == LS_T_NATIVE) {
                ls_native_t *nat = (ls_native_t *)callee.u.ptr;
                result = nat->fn(L, na, call_args);
            } else if (callee.tag == LS_T_FUNCTION || callee.tag == LS_T_GENERIC) {
                result = ls_apply(L, callee, na, call_args);
            } else {
                ls_error(L, "VM: attempt to call non-function (tag=%d)", callee.tag);
                result = ls_nil_v();
            }

            stack[sp++] = result;
            break;
        }

        case OP_RETURN:
            if (sp < 1) return ls_nil_v();
            return stack[sp - 1];

        case OP_HALT:
            if (sp < 1) return ls_nil_v();
            return stack[sp - 1];

        /* -- Arithmetic ------------------------------------------------ */
        case OP_ADD: {
            if (sp < 2) ls_error(L, "VM: stack underflow on ADD");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = ls_make_fixnum(a.u.fixnum + b.u.fixnum);
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = ls_make_flonum(da + db);
            }
            break;
        }

        case OP_SUB: {
            if (sp < 2) ls_error(L, "VM: stack underflow on SUB");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = ls_make_fixnum(a.u.fixnum - b.u.fixnum);
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = ls_make_flonum(da - db);
            }
            break;
        }

        case OP_MUL: {
            if (sp < 2) ls_error(L, "VM: stack underflow on MUL");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = ls_make_fixnum(a.u.fixnum * b.u.fixnum);
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = ls_make_flonum(da * db);
            }
            break;
        }

        case OP_DIV: {
            if (sp < 2) ls_error(L, "VM: stack underflow on DIV");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
            if (db == 0.0) ls_error(L, "VM: division by zero");
            double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
            /* Integer division when both fixnum and exact. */
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM &&
                b.u.fixnum != 0 && (a.u.fixnum % b.u.fixnum) == 0)
                stack[sp++] = ls_make_fixnum(a.u.fixnum / b.u.fixnum);
            else
                stack[sp++] = ls_make_flonum(da / db);
            break;
        }

        case OP_MOD: {
            if (sp < 2) ls_error(L, "VM: stack underflow on MOD");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM) {
                if (b.u.fixnum == 0) ls_error(L, "VM: mod by zero");
                /* CL mod: result has sign of divisor. */
                int64_t r = a.u.fixnum % b.u.fixnum;
                if (r != 0 && ((r ^ b.u.fixnum) < 0))
                    r += b.u.fixnum;
                stack[sp++] = ls_make_fixnum(r);
            } else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                if (db == 0.0) ls_error(L, "VM: mod by zero");
                double r = da - db * floor(da / db);
                stack[sp++] = ls_make_flonum(r);
            }
            break;
        }

        case OP_NEG: {
            if (sp < 1) ls_error(L, "VM: stack underflow on NEG");
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM)
                stack[sp++] = ls_make_fixnum(-a.u.fixnum);
            else
                stack[sp++] = ls_make_flonum(-a.u.flonum);
            break;
        }

        /* -- Comparisons ----------------------------------------------- */
        case OP_EQ: {
            if (sp < 2) ls_error(L, "VM: stack underflow on EQ");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            /* EQ: pointer identity (or both nil, or same fixnum). */
            int eq = (a.tag == b.tag && a.u.ptr == b.u.ptr);
            stack[sp++] = eq ? ls_t_v() : ls_nil_v();
            break;
        }

        case OP_NUMEQ: {
            if (sp < 2) ls_error(L, "VM: stack underflow on NUMEQ");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
            double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
            stack[sp++] = (da == db) ? ls_t_v() : ls_nil_v();
            break;
        }

        case OP_LT: {
            if (sp < 2) ls_error(L, "VM: stack underflow on LT");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = (a.u.fixnum < b.u.fixnum) ? ls_t_v() : ls_nil_v();
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = (da < db) ? ls_t_v() : ls_nil_v();
            }
            break;
        }

        case OP_LE: {
            if (sp < 2) ls_error(L, "VM: stack underflow on LE");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = (a.u.fixnum <= b.u.fixnum) ? ls_t_v() : ls_nil_v();
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = (da <= db) ? ls_t_v() : ls_nil_v();
            }
            break;
        }

        case OP_GT: {
            if (sp < 2) ls_error(L, "VM: stack underflow on GT");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = (a.u.fixnum > b.u.fixnum) ? ls_t_v() : ls_nil_v();
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = (da > db) ? ls_t_v() : ls_nil_v();
            }
            break;
        }

        case OP_GE: {
            if (sp < 2) ls_error(L, "VM: stack underflow on GE");
            ls_value_t b = stack[--sp];
            ls_value_t a = stack[--sp];
            if (a.tag == LS_T_FIXNUM && b.tag == LS_T_FIXNUM)
                stack[sp++] = (a.u.fixnum >= b.u.fixnum) ? ls_t_v() : ls_nil_v();
            else {
                double da = (a.tag == LS_T_FIXNUM) ? (double)a.u.fixnum : a.u.flonum;
                double db = (b.tag == LS_T_FIXNUM) ? (double)b.u.fixnum : b.u.flonum;
                stack[sp++] = (da >= db) ? ls_t_v() : ls_nil_v();
            }
            break;
        }

        /* -- Logic ----------------------------------------------------- */
        case OP_NOT: {
            if (sp < 1) ls_error(L, "VM: stack underflow on NOT");
            ls_value_t a = stack[--sp];
            stack[sp++] = (a.tag == LS_T_NIL) ? ls_t_v() : ls_nil_v();
            break;
        }

        /* -- Cons / list ----------------------------------------------- */
        case OP_CONS: {
            if (sp < 2) ls_error(L, "VM: stack underflow on CONS");
            ls_value_t cdr = stack[--sp];
            ls_value_t car = stack[--sp];
            stack[sp++] = ls_cons(L, car, cdr);
            break;
        }

        case OP_CAR: {
            if (sp < 1) ls_error(L, "VM: stack underflow on CAR");
            ls_value_t a = stack[--sp];
            stack[sp++] = ls_car(a);
            break;
        }

        case OP_CDR: {
            if (sp < 1) ls_error(L, "VM: stack underflow on CDR");
            ls_value_t a = stack[--sp];
            stack[sp++] = ls_cdr(a);
            break;
        }

        case OP_LIST: {
            int n = (int)arg;
            if (sp < n) ls_error(L, "VM: stack underflow on LIST");
            ls_value_t lst = ls_nil_v();
            for (int i = 0; i < n; i++)
                lst = ls_cons(L, stack[sp - n + i], lst);
            /* That built the list backwards; reverse it. */
            lst = ls_list_reverse(L, lst);
            sp -= n;
            stack[sp++] = lst;
            break;
        }

        /* -- Closures -------------------------------------------------- */
        case OP_CLOSURE: {
            if (arg >= proto->n_consts)
                ls_error(L, "VM: CLOSURE const index %u out of range", arg);
            ls_value_t proto_v = consts[arg];
            if (proto_v.tag != LS_T_BYTECODE)
                ls_error(L, "VM: CLOSURE const is not a proto");
            ls_proto_t *inner = (ls_proto_t *)proto_v.u.ptr;

            ls_value_t fv = ls_make_obj(L, LS_T_BYTECODE, sizeof(ls_bytecode_fn_t));
            ls_bytecode_fn_t *closure = (ls_bytecode_fn_t *)fv.u.ptr;
            closure->proto      = inner;
            closure->jit_entry  = NULL;
            closure->call_count = 0;
            closure->name       = inner->name;

            /* Capture upvalues. */
            closure->nupvals = inner->n_upvals;
            if (inner->n_upvals > 0) {
                closure->upvals = (ls_value_t *)malloc(inner->n_upvals * sizeof(ls_value_t));
                for (uint32_t i = 0; i < inner->n_upvals; i++) {
                    /* We need to look up the upval descriptor from the
                     * compiler state.  But at runtime we don't have that
                     * struct.  Instead, we use a convention: the proto
                     * stores the upvalue symbol names; we resolve by
                     * looking at the current frame's locals first and then
                     * at the current closure's upvals. */
                    if (inner->upvals && inner->upvals[i]) {
                        ls_symbol_t *uvsym = inner->upvals[i];
                        /* Search current locals. */
                        int found = 0;
                        for (int j = 0; j < n_locals; j++) {
                            /* We need the local's symbol name.  We look
                             * it up from the proto's const table where
                             * the compiler placed variable symbols. */
                            /* Simpler approach: during compilation we
                             * record enough info.  At runtime we use a
                             * flat index scheme encoded in the proto.
                             * For now, match by the upval symbol against
                             * the outer proto's locals by iterating
                             * the const table for symbol matches. */
                            (void)j;
                        }
                        /* Fallback: search current fn's upvals. */
                        if (!found && upvals) {
                            for (uint32_t j = 0; j < fn->nupvals; j++) {
                                if (upvals[j].tag == LS_T_SYMBOL &&
                                    (ls_symbol_t *)upvals[j].u.ptr == uvsym) {
                                    closure->upvals[i] = upvals[j];
                                    found = 1;
                                    break;
                                }
                            }
                        }
                        /* Last resort: get from locals by slot index.
                         * The compiler arranged upval slot i to capture
                         * local slot i from the parent when is_local=1.
                         * We encode this: upvals[i] maps to stack[i]. */
                        if (!found && (int)i < n_locals) {
                            closure->upvals[i] = stack[i];
                        } else if (!found) {
                            closure->upvals[i] = ls_nil_v();
                        }
                    } else {
                        /* No symbol recorded; use positional: grab from
                         * current frame's locals or parent upvals. */
                        if ((int)i < n_locals)
                            closure->upvals[i] = stack[i];
                        else if (upvals && i < fn->nupvals)
                            closure->upvals[i] = upvals[i];
                        else
                            closure->upvals[i] = ls_nil_v();
                    }
                }
            } else {
                closure->upvals = NULL;
            }

            stack[sp++] = fv;
            break;
        }

        /* -- Exception handling ---------------------------------------- */
        case OP_PUSH_HANDLER:
            if (hsp >= 64) ls_error(L, "VM: handler stack overflow");
            handlers[hsp].target_pc = pc + sarg;
            handlers[hsp].saved_sp  = sp;
            hsp++;
            break;

        case OP_POP_HANDLER:
            if (hsp > 0) hsp--;
            break;

        default:
            ls_error(L, "VM: unknown opcode %u at pc=%d", op, pc - 1);
            return ls_nil_v();
        }
    }
    /* Should never reach here. */
    return ls_nil_v();
}

/* ================================================================
 *  Disassembler
 * ================================================================ */

static void disasm_proto(ls_state_t *L, ls_proto_t *proto, int indent) {
    (void)L;
    const char *pad = "";
    char ibuf[64];
    if (indent > 0) {
        int n = indent * 2;
        if (n >= (int)sizeof ibuf) n = (int)sizeof ibuf - 1;
        memset(ibuf, ' ', n);
        ibuf[n] = '\0';
        pad = ibuf;
    }

    printf("%s; proto", pad);
    if (proto->name && proto->name->name)
        printf(" %s", proto->name->name->chars);
    printf("  args=%u opt=%u locals=%u upvals=%u rest=%u\n",
           proto->n_args, proto->n_opt, proto->n_locals,
           proto->n_upvals, proto->has_rest);
    printf("%s; %u instructions, %u constants\n",
           pad, proto->code_len, proto->n_consts);

    for (uint32_t i = 0; i < proto->code_len; i++) {
        uint32_t instr = proto->code[i];
        uint32_t opc   = OP_GET_OP(instr);
        uint32_t arg   = OP_GET_ARG(instr);
        int32_t  sarg  = OP_GET_SARG(instr);

        const char *name = (opc < OP_MAX) ? ls_opcode_names[opc] : "???";

        printf("%s  %4u  %-18s", pad, i, name);

        switch (opc) {
        case OP_CONST:
            printf(" %u", arg);
            if (arg < proto->n_consts) {
                ls_value_t cv = proto->consts[arg];
                if (cv.tag == LS_T_SYMBOL) {
                    ls_symbol_t *s = (ls_symbol_t *)cv.u.ptr;
                    if (s->name) printf("  ; %s", s->name->chars);
                }
                else if (cv.tag == LS_T_FIXNUM) printf("  ; %lld", (long long)cv.u.fixnum);
                else if (cv.tag == LS_T_FLONUM) printf("  ; %g", cv.u.flonum);
                else if (cv.tag == LS_T_STRING) {
                    ls_string_t *ss = (ls_string_t *)cv.u.ptr;
                    if (ss && ss->chars) printf("  ; \"%s\"", ss->chars);
                }
            }
            break;
        case OP_FIXNUM:       printf(" %d", sarg); break;
        case OP_LOAD_LOCAL:   printf(" %u", arg);  break;
        case OP_STORE_LOCAL:  printf(" %u", arg);  break;
        case OP_LOAD_UPVAL:   printf(" %u", arg);  break;
        case OP_STORE_UPVAL:  printf(" %u", arg);  break;
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
            printf(" %u", arg);
            if (arg < proto->n_consts) {
                ls_value_t cv = proto->consts[arg];
                if (cv.tag == LS_T_SYMBOL) {
                    ls_symbol_t *s = (ls_symbol_t *)cv.u.ptr;
                    if (s->name) printf("  ; %s", s->name->chars);
                }
            }
            break;
        case OP_JUMP:
        case OP_JUMP_IF_NIL:
        case OP_JUMP_IF_NOT_NIL:
        case OP_PUSH_HANDLER:
            printf(" %+d -> %u", sarg, (uint32_t)((int32_t)i + 1 + sarg));
            break;
        case OP_CALL:
        case OP_TAILCALL:     printf(" %u", arg); break;
        case OP_CLOSURE:      printf(" %u", arg); break;
        case OP_LIST:         printf(" %u", arg); break;
        default: break;
        }
        printf("\n");
    }

    /* Print inner protos. */
    for (uint32_t i = 0; i < proto->n_consts; i++) {
        if (proto->consts[i].tag == LS_T_BYTECODE) {
            /* Could be a proto (if it has code). */
            ls_proto_t *inner = (ls_proto_t *)proto->consts[i].u.ptr;
            if (inner->code && inner->code_len > 0) {
                printf("%s  -- inner proto const[%u] --\n", pad, i);
                disasm_proto(L, inner, indent + 1);
            }
        }
    }
}

/* ================================================================
 *  Native builtins: (compile form), (disassemble fn)
 * ================================================================ */

static ls_value_t bi_compile(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) ls_error(L, "compile: need 1 argument");
    return ls_compile_to_bytecode(L, args[0]);
}

static ls_value_t bi_vm_eval(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) ls_error(L, "vm-eval: need 1 argument");
    ls_value_t fn_v = ls_compile_to_bytecode(L, args[0]);
    if (fn_v.tag != LS_T_BYTECODE)
        ls_error(L, "vm-eval: compilation failed");
    ls_bytecode_fn_t *fn = (ls_bytecode_fn_t *)fn_v.u.ptr;
    return ls_vm_run(L, fn, 0, NULL);
}

static ls_value_t bi_disassemble(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) ls_error(L, "disassemble: need 1 argument");
    ls_value_t v = args[0];

    ls_proto_t *proto = NULL;
    if (v.tag == LS_T_BYTECODE) {
        /* Could be a bytecode_fn or a proto.  Check if it has a proto
         * pointer (bytecode_fn) or a code pointer (proto). */
        ls_bytecode_fn_t *bfn = (ls_bytecode_fn_t *)v.u.ptr;
        if (bfn->proto)
            proto = bfn->proto;
        else {
            /* Might be a raw proto. */
            ls_proto_t *p = (ls_proto_t *)v.u.ptr;
            if (p->code) proto = p;
        }
    }

    if (!proto) {
        /* Try to compile it first. */
        ls_value_t compiled = ls_compile_to_bytecode(L, v);
        if (compiled.tag == LS_T_BYTECODE) {
            ls_bytecode_fn_t *bfn = (ls_bytecode_fn_t *)compiled.u.ptr;
            proto = bfn->proto;
        }
    }

    if (!proto) {
        ls_error(L, "disassemble: cannot disassemble value");
        return ls_nil_v();
    }

    disasm_proto(L, proto, 0);
    return ls_nil_v();
}

/* ================================================================
 *  Init
 * ================================================================ */

void ls_init_vm(ls_state_t *L) {
    ls_defun(L, "COMMON-LISP", "COMPILE-TO-BYTECODE", bi_compile, 1, 1);
    ls_defun(L, "COMMON-LISP", "VM-EVAL", bi_vm_eval, 1, 1);
    ls_defun(L, "COMMON-LISP", "DISASSEMBLE", bi_disassemble, 1, 1);
}

/* REPL entry point: compile FORM to bytecode and run it on the VM.
 * Falls back to the interpreter on compilation failure (so unsupported
 * special forms still work). */
ls_value_t ls_vm_compile_and_run(ls_state_t *L, ls_value_t form) {
    ls_value_t fn_v = ls_compile_to_bytecode(L, form);
    if (fn_v.tag != LS_T_BYTECODE) {
        /* fall back to interpreter */
        return ls_eval(L, form, L->genv);
    }
    ls_bytecode_fn_t *fn = (ls_bytecode_fn_t *)fn_v.u.ptr;
    return ls_vm_run(L, fn, 0, NULL);
}
