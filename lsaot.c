/* lsaot.c -- AOT C transpiler for Litesrpent.
 *
 * Parses Lisp source, emits equivalent C code that links against the
 * Litesrpent runtime (lsrt.h / lsrt.c), then invokes the host C
 * compiler to produce a native executable.
 *
 * The generated C uses the runtime's tagged-value type (ls_value_t)
 * so it is fully compatible with the interpreter for mixed-mode use.
 */
#include "lscore.h"
#include "lseval.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>

/* ================================================================
 *  Compiler context -- threaded through every emit helper
 * ================================================================ */

#define AOT_MAX_LOCALS  256
#define AOT_MAX_LAMBDAS 512

typedef struct local_var {
    const char *name;        /* Lisp symbol name */
    char c_name[64];         /* C identifier */
    int  depth;              /* nesting depth for let scoping */
} local_var_t;

typedef struct lambda_info {
    int  id;                 /* unique id for naming the C function */
    char c_name[64];         /* generated C function name */
} lambda_info_t;

typedef struct compiler_ctx {
    ls_state_t  *L;
    FILE        *out;
    FILE        *hdr;            /* forward declarations section */
    int          indent;

    /* Local variable tracking. */
    local_var_t  locals[AOT_MAX_LOCALS];
    int          n_locals;
    int          scope_depth;

    /* Lambda lifting. */
    lambda_info_t lambdas[AOT_MAX_LAMBDAS];
    int           n_lambdas;

    /* Deferred lambda bodies (written after current function). */
    char         *deferred_buf;
    size_t        deferred_len;
    size_t        deferred_cap;
    FILE         *deferred;       /* memstream / tmpfile for lambda bodies */

    /* For generating unique C identifiers. */
    int           tmp_counter;
    int           label_counter;

    /* Flags. */
    int           in_tail_pos;     /* 1 if current expr is in tail position */
    int           in_expr_ctx;     /* 1 = expression ctx (emit value), 0 = stmt */
} compiler_ctx_t;

/* ================================================================
 *  Forward declarations
 * ================================================================ */

static void emit_expr(FILE *out, ls_value_t form, compiler_ctx_t *ctx);
static void emit_body(FILE *out, ls_value_t forms, compiler_ctx_t *ctx);
static void emit_toplevel_defun(FILE *out, ls_value_t form, compiler_ctx_t *ctx);
static void emit_toplevel_expr(FILE *out, ls_value_t form, compiler_ctx_t *ctx);

/* ================================================================
 *  Utility helpers
 * ================================================================ */

/* Write indentation. */
static void emit_indent(FILE *out, compiler_ctx_t *ctx) {
    for (int i = 0; i < ctx->indent; i++) fprintf(out, "    ");
}

/* Sanitise a Lisp symbol name into a valid C identifier. */
static void sanitise_name(const char *lisp_name, char *c_name, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; lisp_name[i] && j + 1 < cap; i++) {
        char ch = lisp_name[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') {
            c_name[j++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
        } else if (ch == '-') {
            c_name[j++] = '_';
        } else if (ch == '?' && j + 2 < cap) {
            c_name[j++] = '_'; c_name[j++] = 'p';
        } else if (ch == '!' && j + 2 < cap) {
            c_name[j++] = '_'; c_name[j++] = 'b';
        } else if (ch == '*') {
            c_name[j++] = '_';
        } else if (ch == '+') {
            if (j + 4 < cap) { memcpy(c_name + j, "_add", 4); j += 4; }
        } else if (ch == '/') {
            if (j + 4 < cap) { memcpy(c_name + j, "_div", 4); j += 4; }
        } else if (ch == '<') {
            if (j + 3 < cap) { memcpy(c_name + j, "_lt", 3); j += 3; }
        } else if (ch == '>') {
            if (j + 3 < cap) { memcpy(c_name + j, "_gt", 3); j += 3; }
        } else if (ch == '=') {
            if (j + 3 < cap) { memcpy(c_name + j, "_eq", 3); j += 3; }
        } else {
            /* hex-escape anything else */
            if (j + 3 < cap) { j += snprintf(c_name + j, cap - j, "x%02x", (unsigned char)ch); }
        }
    }
    c_name[j] = '\0';
}

/* Push a local variable binding. Returns the C identifier. */
static const char *ctx_push_local(compiler_ctx_t *ctx, const char *lisp_name) {
    if (ctx->n_locals >= AOT_MAX_LOCALS) {
        ls_error(ctx->L, "AOT: too many local variables");
        return "ERROR_LOCAL";
    }
    local_var_t *lv = &ctx->locals[ctx->n_locals];
    lv->name = lisp_name;
    lv->depth = ctx->scope_depth;
    snprintf(lv->c_name, sizeof lv->c_name, "lv_%d_", ctx->n_locals);
    sanitise_name(lisp_name, lv->c_name + strlen(lv->c_name),
                  sizeof(lv->c_name) - strlen(lv->c_name));
    ctx->n_locals++;
    return lv->c_name;
}

/* Find a local variable by Lisp name (search innermost first). */
static const char *ctx_find_local(compiler_ctx_t *ctx, const char *lisp_name) {
    for (int i = ctx->n_locals - 1; i >= 0; i--) {
        if (strcmp(ctx->locals[i].name, lisp_name) == 0)
            return ctx->locals[i].c_name;
    }
    return NULL;
}

/* Pop locals down to the given depth. */
static void ctx_pop_to_depth(compiler_ctx_t *ctx, int depth) {
    while (ctx->n_locals > 0 && ctx->locals[ctx->n_locals - 1].depth > depth)
        ctx->n_locals--;
}

/* Get a fresh temporary name. */
static int ctx_fresh_tmp(compiler_ctx_t *ctx) {
    return ctx->tmp_counter++;
}

/* Check if a form's head matches a known symbol. */
static int form_head_is(ls_state_t *L, ls_value_t form, ls_symbol_t *sym) {
    if (form.tag != LS_T_CONS) return 0;
    ls_value_t head = ls_car(form);
    return head.tag == LS_T_SYMBOL && (ls_symbol_t *)head.u.ptr == sym;
}

/* Check if a symbol name matches a string (case insensitive). */
static int sym_name_eq(ls_value_t v, const char *name) {
    if (v.tag != LS_T_SYMBOL) return 0;
    ls_symbol_t *s = (ls_symbol_t *)v.u.ptr;
    if (!s->name || !s->name->chars) return 0;
    const char *sn = s->name->chars;
    for (size_t i = 0; ; i++) {
        char a = sn[i], b = name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
        if (a == '\0') return 1;
    }
}

/* ================================================================
 *  Runtime header -- embedded in the generated .c file
 * ================================================================ */

static const char *aot_runtime_header =
    "/* Generated by Litesrpent AOT transpiler. */\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include <stdint.h>\n"
    "#include <math.h>\n"
    "\n"
    "#ifndef LSRT_H_INCLUDED\n"
    "#define LSRT_H_INCLUDED\n"
    "\n"
    "typedef struct ls_state ls_state_t;\n"
    "typedef struct ls_value {\n"
    "    uint32_t tag;\n"
    "    uint32_t flags;\n"
    "    union { int64_t fixnum; double flonum; uint32_t character; void *ptr; } u;\n"
    "} ls_value_t;\n"
    "\n"
    "enum {\n"
    "    LS_T_NIL=0,LS_T_T,LS_T_FIXNUM,LS_T_FLONUM,LS_T_CHAR,\n"
    "    LS_T_SYMBOL,LS_T_STRING,LS_T_CONS,LS_T_VECTOR,LS_T_HASHTABLE,\n"
    "    LS_T_FUNCTION,LS_T_BYTECODE,LS_T_NATIVE,LS_T_MACRO,LS_T_SPECIAL,\n"
    "    LS_T_PACKAGE,LS_T_STREAM,LS_T_FOREIGN,LS_T_FOREIGN_LIB,LS_T_FOREIGN_FN\n"
    "};\n"
    "\n"
    "static ls_value_t LS_NIL  = {0,0,{0}};\n"
    "static ls_value_t LS_TRUE = {1,0,{0}};\n"
    "\n"
    "static ls_value_t LS_FIXNUM(int64_t x) {\n"
    "    ls_value_t v; memset(&v,0,sizeof v); v.tag=LS_T_FIXNUM; v.u.fixnum=x; return v;\n"
    "}\n"
    "static ls_value_t LS_FLONUM(double x) {\n"
    "    ls_value_t v; memset(&v,0,sizeof v); v.tag=LS_T_FLONUM; v.u.flonum=x; return v;\n"
    "}\n"
    "static int LS_NILP(ls_value_t v) { return v.tag == LS_T_NIL; }\n"
    "static int LS_IS_FIXNUM(ls_value_t v) { return v.tag == LS_T_FIXNUM; }\n"
    "static int LS_IS_FLONUM(ls_value_t v) { return v.tag == LS_T_FLONUM; }\n"
    "\n"
    "/* Forward decl -- implemented in lsrt.c or linked from litesrpent lib */\n"
    "ls_state_t *lsrt_init(void);\n"
    "void lsrt_shutdown(ls_state_t *L);\n"
    "ls_value_t lsrt_cons(ls_state_t *L, ls_value_t car, ls_value_t cdr);\n"
    "ls_value_t lsrt_car(ls_value_t v);\n"
    "ls_value_t lsrt_cdr(ls_value_t v);\n"
    "ls_value_t lsrt_add(ls_value_t a, ls_value_t b);\n"
    "ls_value_t lsrt_sub(ls_value_t a, ls_value_t b);\n"
    "ls_value_t lsrt_mul(ls_value_t a, ls_value_t b);\n"
    "ls_value_t lsrt_div(ls_value_t a, ls_value_t b);\n"
    "ls_value_t lsrt_mod(ls_value_t a, ls_value_t b);\n"
    "ls_value_t lsrt_neg(ls_value_t a);\n"
    "int lsrt_num_lt(ls_value_t a, ls_value_t b);\n"
    "int lsrt_num_gt(ls_value_t a, ls_value_t b);\n"
    "int lsrt_num_le(ls_value_t a, ls_value_t b);\n"
    "int lsrt_num_ge(ls_value_t a, ls_value_t b);\n"
    "int lsrt_num_eq(ls_value_t a, ls_value_t b);\n"
    "int lsrt_eq(ls_value_t a, ls_value_t b);\n"
    "int lsrt_eql(ls_value_t a, ls_value_t b);\n"
    "int lsrt_equal(ls_value_t a, ls_value_t b);\n"
    "ls_value_t lsrt_not(ls_value_t a);\n"
    "ls_value_t lsrt_intern(ls_state_t *L, const char *pkg, const char *name);\n"
    "ls_value_t lsrt_getvar(ls_state_t *L, ls_value_t sym);\n"
    "void lsrt_setvar(ls_state_t *L, ls_value_t sym, ls_value_t val);\n"
    "ls_value_t lsrt_apply(ls_state_t *L, ls_value_t fn, int nargs, ls_value_t *args);\n"
    "ls_value_t lsrt_make_string(ls_state_t *L, const char *s, size_t n);\n"
    "ls_value_t lsrt_list(ls_state_t *L, int n, ...);\n"
    "void lsrt_print(ls_state_t *L, ls_value_t v);\n"
    "void lsrt_println(ls_state_t *L, ls_value_t v);\n"
    "ls_value_t lsrt_funcall(ls_state_t *L, const char *name, int nargs, ...);\n"
    "\n"
    "#endif /* LSRT_H_INCLUDED */\n"
    "\n";

/* ================================================================
 *  Emit literal values
 * ================================================================ */

static void emit_nil(FILE *out) {
    fprintf(out, "LS_NIL");
}

static void emit_t(FILE *out) {
    fprintf(out, "LS_TRUE");
}

static void emit_fixnum(FILE *out, int64_t n) {
    fprintf(out, "LS_FIXNUM(%" PRId64 "LL)", n);
}

static void emit_flonum(FILE *out, double d) {
    fprintf(out, "LS_FLONUM(%.17g)", d);
}

static void emit_string_literal(FILE *out, const char *s, size_t len,
                                compiler_ctx_t *ctx) {
    fprintf(out, "lsrt_make_string(L, \"");
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"')       fprintf(out, "\\\"");
        else if (ch == '\\') fprintf(out, "\\\\");
        else if (ch == '\n') fprintf(out, "\\n");
        else if (ch == '\r') fprintf(out, "\\r");
        else if (ch == '\t') fprintf(out, "\\t");
        else if (ch < 32 || ch >= 127)
            fprintf(out, "\\x%02x", ch);
        else
            fputc(ch, out);
    }
    fprintf(out, "\", %zu)", len);
    (void)ctx;
}

/* ================================================================
 *  Emit quoted value (recursive literal construction)
 * ================================================================ */

static void emit_quote(FILE *out, ls_value_t v, compiler_ctx_t *ctx) {
    switch (v.tag) {
    case LS_T_NIL:    emit_nil(out); break;
    case LS_T_T:      emit_t(out); break;
    case LS_T_FIXNUM: emit_fixnum(out, v.u.fixnum); break;
    case LS_T_FLONUM: emit_flonum(out, v.u.flonum); break;
    case LS_T_STRING: {
        ls_string_t *s = (ls_string_t *)v.u.ptr;
        emit_string_literal(out, s->chars, s->len, ctx);
        break;
    }
    case LS_T_SYMBOL: {
        ls_symbol_t *s = (ls_symbol_t *)v.u.ptr;
        const char *pkg = s->package ? s->package->name->chars : "COMMON-LISP";
        fprintf(out, "lsrt_intern(L, \"%s\", \"%s\")", pkg, s->name->chars);
        break;
    }
    case LS_T_CONS: {
        fprintf(out, "lsrt_cons(L, ");
        emit_quote(out, ls_car(v), ctx);
        fprintf(out, ", ");
        emit_quote(out, ls_cdr(v), ctx);
        fprintf(out, ")");
        break;
    }
    default:
        /* For unhandled types, emit NIL with a comment. */
        fprintf(out, "LS_NIL /* unquotable type %u */", v.tag);
        break;
    }
}

/* ================================================================
 *  Builtin arithmetic -- direct C for known fixnum ops
 * ================================================================ */

typedef struct {
    const char *lisp_name;
    const char *rt_fn;         /* runtime function for general case */
    const char *c_op;          /* C operator for fixnum fast path, or NULL */
    int         is_cmp;        /* 1 = comparison (returns int), 0 = arith */
} arith_op_t;

static const arith_op_t arith_ops[] = {
    { "+",   "lsrt_add", "+",  0 },
    { "-",   "lsrt_sub", "-",  0 },
    { "*",   "lsrt_mul", "*",  0 },
    { "/",   "lsrt_div", "/",  0 },
    { "MOD", "lsrt_mod", "%",  0 },
    { "<",   NULL,        "<",  1 },
    { ">",   NULL,        ">",  1 },
    { "<=",  NULL,        "<=", 1 },
    { ">=",  NULL,        ">=", 1 },
    { "=",   NULL,        "==", 1 },
    { NULL, NULL, NULL, 0 }
};

static const arith_op_t *find_arith_op(const char *name) {
    for (int i = 0; arith_ops[i].lisp_name; i++) {
        const char *a = arith_ops[i].lisp_name;
        const char *b = name;
        int match = 1;
        for (size_t k = 0; ; k++) {
            char ca = a[k], cb = b[k];
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) { match = 0; break; }
            if (ca == '\0') break;
        }
        if (match) return &arith_ops[i];
    }
    return NULL;
}

/* ================================================================
 *  Main expression emitter
 * ================================================================ */

/* Emit an arithmetic binary operation with optional fixnum fast path. */
static void emit_arith_binop(FILE *out, const arith_op_t *op,
                             ls_value_t a, ls_value_t b,
                             compiler_ctx_t *ctx) {
    if (op->is_cmp) {
        /* Comparison: emit ternary that returns LS_TRUE or LS_NIL. */
        fprintf(out, "((LS_IS_FIXNUM(");
        emit_expr(out, a, ctx);
        fprintf(out, ") && LS_IS_FIXNUM(");
        emit_expr(out, b, ctx);
        fprintf(out, ")) ? ((");
        emit_expr(out, a, ctx);
        fprintf(out, ").u.fixnum %s (", op->c_op);
        emit_expr(out, b, ctx);
        fprintf(out, ").u.fixnum ? LS_TRUE : LS_NIL) : (lsrt_num_%s(",
                strcmp(op->c_op, "<") == 0 ? "lt" :
                strcmp(op->c_op, ">") == 0 ? "gt" :
                strcmp(op->c_op, "<=") == 0 ? "le" :
                strcmp(op->c_op, ">=") == 0 ? "ge" : "eq");
        emit_expr(out, a, ctx);
        fprintf(out, ", ");
        emit_expr(out, b, ctx);
        fprintf(out, ") ? LS_TRUE : LS_NIL))");
    } else {
        /* Arithmetic: fixnum fast path, fallback to runtime. */
        fprintf(out, "((LS_IS_FIXNUM(");
        emit_expr(out, a, ctx);
        fprintf(out, ") && LS_IS_FIXNUM(");
        emit_expr(out, b, ctx);
        fprintf(out, ")) ? LS_FIXNUM((");
        emit_expr(out, a, ctx);
        fprintf(out, ").u.fixnum %s (", op->c_op);
        emit_expr(out, b, ctx);
        fprintf(out, ").u.fixnum) : %s(", op->rt_fn);
        emit_expr(out, a, ctx);
        fprintf(out, ", ");
        emit_expr(out, b, ctx);
        fprintf(out, "))");
    }
}

/* Emit a multi-arg arithmetic expression by left-folding. */
static void emit_arith_nary(FILE *out, const arith_op_t *op,
                            ls_value_t args, compiler_ctx_t *ctx) {
    size_t len = ls_list_length(args);

    if (len == 0) {
        /* Identity: (+) -> 0, (*) -> 1. */
        if (op->c_op[0] == '+' || op->c_op[0] == '-')
            emit_fixnum(out, 0);
        else if (op->c_op[0] == '*')
            emit_fixnum(out, 1);
        else
            emit_nil(out);
        return;
    }

    if (len == 1 && !op->is_cmp) {
        /* Unary: (- x) -> negate, (+ x) -> identity. */
        ls_value_t only = ls_car(args);
        if (op->c_op[0] == '-') {
            fprintf(out, "lsrt_neg(");
            emit_expr(out, only, ctx);
            fprintf(out, ")");
        } else {
            emit_expr(out, only, ctx);
        }
        return;
    }

    /* Binary left-fold. */
    ls_value_t cur = args;
    ls_value_t first = ls_car(cur);
    cur = ls_cdr(cur);

    if (len == 2) {
        emit_arith_binop(out, op, first, ls_car(cur), ctx);
        return;
    }

    /* For 3+ args, use temporaries. */
    int tmp = ctx_fresh_tmp(ctx);
    fprintf(out, "({ls_value_t _t%d = ", tmp);
    emit_expr(out, first, ctx);
    fprintf(out, "; ");
    while (cur.tag == LS_T_CONS) {
        int tmp2 = ctx_fresh_tmp(ctx);
        fprintf(out, "ls_value_t _t%d = ", tmp2);
        if (op->is_cmp) {
            fprintf(out, "(lsrt_num_%s(_t%d, ",
                    strcmp(op->c_op, "<") == 0 ? "lt" :
                    strcmp(op->c_op, ">") == 0 ? "gt" :
                    strcmp(op->c_op, "<=") == 0 ? "le" :
                    strcmp(op->c_op, ">=") == 0 ? "ge" : "eq",
                    tmp);
            emit_expr(out, ls_car(cur), ctx);
            fprintf(out, ") ? LS_TRUE : LS_NIL)");
        } else {
            fprintf(out, "%s(_t%d, ", op->rt_fn, tmp);
            emit_expr(out, ls_car(cur), ctx);
            fprintf(out, ")");
        }
        fprintf(out, "; ");
        if (op->is_cmp) {
            fprintf(out, "if (LS_NILP(_t%d)) _t%d = LS_NIL; else { ", tmp2, tmp2);
            int tmp_rhs = ctx_fresh_tmp(ctx);
            fprintf(out, "ls_value_t _t%d = ", tmp_rhs);
            emit_expr(out, ls_car(cur), ctx);
            fprintf(out, "; _t%d = _t%d; ", tmp, tmp_rhs);
            fprintf(out, "} ");
            tmp = tmp2;
        } else {
            tmp = tmp2;
        }
        cur = ls_cdr(cur);
    }
    fprintf(out, "_t%d; })", tmp);
}

/* Emit the expression for a single Lisp form. */
static void emit_expr(FILE *out, ls_value_t form, compiler_ctx_t *ctx) {
    ls_state_t *L = ctx->L;

    /* -- Literals -------------------------------------------------- */
    switch (form.tag) {
    case LS_T_NIL:    emit_nil(out); return;
    case LS_T_T:      emit_t(out); return;
    case LS_T_FIXNUM: emit_fixnum(out, form.u.fixnum); return;
    case LS_T_FLONUM: emit_flonum(out, form.u.flonum); return;
    case LS_T_STRING: {
        ls_string_t *s = (ls_string_t *)form.u.ptr;
        emit_string_literal(out, s->chars, s->len, ctx);
        return;
    }
    case LS_T_CHAR:
        fprintf(out, "LS_FIXNUM(%u)", form.u.character);
        return;
    default:
        break;
    }

    /* -- Symbol (variable reference) -------------------------------- */
    if (form.tag == LS_T_SYMBOL) {
        ls_symbol_t *sym = (ls_symbol_t *)form.u.ptr;
        const char *local = ctx_find_local(ctx, sym->name->chars);
        if (local) {
            fprintf(out, "%s", local);
        } else {
            /* Global variable lookup via runtime. */
            const char *pkg = sym->package ? sym->package->name->chars : "COMMON-LISP";
            fprintf(out, "lsrt_getvar(L, lsrt_intern(L, \"%s\", \"%s\"))",
                    pkg, sym->name->chars);
        }
        return;
    }

    /* -- Compound forms (cons cells) -------------------------------- */
    if (form.tag != LS_T_CONS) {
        /* Unknown type -- emit nil. */
        emit_nil(out);
        return;
    }

    ls_value_t head = ls_car(form);
    ls_value_t rest = ls_cdr(form);

    /* (quote x) */
    if (form_head_is(L, form, L->sym_quote)) {
        emit_quote(out, ls_car(rest), ctx);
        return;
    }

    /* (if test then [else]) */
    if (form_head_is(L, form, L->sym_if)) {
        ls_value_t test_form = ls_car(rest);
        ls_value_t then_form = ls_car(ls_cdr(rest));
        ls_value_t else_rest = ls_cdr(ls_cdr(rest));
        int has_else = (else_rest.tag == LS_T_CONS);

        fprintf(out, "(!LS_NILP(");
        emit_expr(out, test_form, ctx);
        fprintf(out, ") ? (");
        emit_expr(out, then_form, ctx);
        fprintf(out, ") : (");
        if (has_else)
            emit_expr(out, ls_car(else_rest), ctx);
        else
            emit_nil(out);
        fprintf(out, "))");
        return;
    }

    /* (progn ...) */
    if (form_head_is(L, form, L->sym_progn)) {
        size_t len = ls_list_length(rest);
        if (len == 0) { emit_nil(out); return; }
        if (len == 1) { emit_expr(out, ls_car(rest), ctx); return; }

        fprintf(out, "({");
        ls_value_t cur = rest;
        size_t idx = 0;
        while (cur.tag == LS_T_CONS) {
            idx++;
            if (idx == len) {
                /* Last expression is the value. */
                emit_expr(out, ls_car(cur), ctx);
                fprintf(out, ";");
            } else {
                emit_expr(out, ls_car(cur), ctx);
                fprintf(out, "; ");
            }
            cur = ls_cdr(cur);
        }
        fprintf(out, "})");
        return;
    }

    /* (setq var val [var val ...]) */
    if (form_head_is(L, form, L->sym_setq)) {
        ls_value_t pairs = rest;
        ls_value_t last_val = ls_nil_v();
        int count = 0;
        int npairs = (int)(ls_list_length(pairs) / 2);

        if (npairs > 1) fprintf(out, "({");
        while (pairs.tag == LS_T_CONS) {
            ls_value_t var_sym = ls_car(pairs);
            pairs = ls_cdr(pairs);
            if (pairs.tag != LS_T_CONS) break;
            ls_value_t val_form = ls_car(pairs);
            pairs = ls_cdr(pairs);

            if (count > 0 && npairs > 1) fprintf(out, " ");

            if (var_sym.tag == LS_T_SYMBOL) {
                ls_symbol_t *sym = (ls_symbol_t *)var_sym.u.ptr;
                const char *local = ctx_find_local(ctx, sym->name->chars);
                if (local) {
                    if (npairs == 1) {
                        fprintf(out, "(%s = ", local);
                        emit_expr(out, val_form, ctx);
                        fprintf(out, ")");
                    } else {
                        fprintf(out, "%s = ", local);
                        emit_expr(out, val_form, ctx);
                        fprintf(out, ";");
                    }
                } else {
                    /* Global setq. */
                    const char *pkg = sym->package ? sym->package->name->chars : "COMMON-LISP";
                    if (npairs == 1) {
                        fprintf(out, "(lsrt_setvar(L, lsrt_intern(L, \"%s\", \"%s\"), ",
                                pkg, sym->name->chars);
                        emit_expr(out, val_form, ctx);
                        fprintf(out, "), ");
                        emit_expr(out, val_form, ctx);
                        fprintf(out, ")");
                    } else {
                        fprintf(out, "lsrt_setvar(L, lsrt_intern(L, \"%s\", \"%s\"), ",
                                pkg, sym->name->chars);
                        emit_expr(out, val_form, ctx);
                        fprintf(out, ");");
                    }
                }
            }
            last_val = val_form;
            count++;
        }
        if (npairs > 1) {
            fprintf(out, " ");
            emit_expr(out, last_val, ctx);
            fprintf(out, ";})");
        }
        return;
    }

    /* (let ((var val) ...) body...) */
    if (form_head_is(L, form, L->sym_let)) {
        ls_value_t bindings = ls_car(rest);
        ls_value_t body = ls_cdr(rest);
        int saved = ctx->n_locals;
        int saved_depth = ctx->scope_depth;
        ctx->scope_depth++;

        fprintf(out, "({");

        /* First pass: evaluate all init-forms (not yet in scope). */
        ls_value_t cur = bindings;
        int bind_idx = 0;
        int *tmp_ids = NULL;
        int n_binds = (int)ls_list_length(bindings);

        if (n_binds > 0) {
            tmp_ids = (int *)malloc(sizeof(int) * (size_t)n_binds);
        }

        while (cur.tag == LS_T_CONS) {
            ls_value_t entry = ls_car(cur);
            ls_value_t init_form = ls_nil_v();
            ls_value_t var_sym;

            if (entry.tag == LS_T_CONS) {
                var_sym = ls_car(entry);
                ls_value_t init_rest = ls_cdr(entry);
                if (init_rest.tag == LS_T_CONS) init_form = ls_car(init_rest);
            } else {
                var_sym = entry;
            }

            int tid = ctx_fresh_tmp(ctx);
            tmp_ids[bind_idx] = tid;
            fprintf(out, "ls_value_t _t%d = ", tid);
            emit_expr(out, init_form, ctx);
            fprintf(out, "; ");

            (void)var_sym;
            bind_idx++;
            cur = ls_cdr(cur);
        }

        /* Second pass: bind variables. */
        cur = bindings;
        bind_idx = 0;
        while (cur.tag == LS_T_CONS) {
            ls_value_t entry = ls_car(cur);
            ls_value_t var_sym;

            if (entry.tag == LS_T_CONS)
                var_sym = ls_car(entry);
            else
                var_sym = entry;

            if (var_sym.tag == LS_T_SYMBOL) {
                ls_symbol_t *sym = (ls_symbol_t *)var_sym.u.ptr;
                const char *cname = ctx_push_local(ctx, sym->name->chars);
                fprintf(out, "ls_value_t %s = _t%d; ", cname, tmp_ids[bind_idx]);
            }
            bind_idx++;
            cur = ls_cdr(cur);
        }
        free(tmp_ids);

        /* Emit body forms. */
        emit_body(out, body, ctx);
        fprintf(out, "})");

        ctx->scope_depth = saved_depth;
        ctx_pop_to_depth(ctx, saved_depth);
        ctx->n_locals = saved;
        return;
    }

    /* (let* ((var val) ...) body...) */
    if (form_head_is(L, form, L->sym_letstar)) {
        ls_value_t bindings = ls_car(rest);
        ls_value_t body = ls_cdr(rest);
        int saved = ctx->n_locals;
        int saved_depth = ctx->scope_depth;
        ctx->scope_depth++;

        fprintf(out, "({");
        ls_value_t cur = bindings;
        while (cur.tag == LS_T_CONS) {
            ls_value_t entry = ls_car(cur);
            ls_value_t init_form = ls_nil_v();
            ls_value_t var_sym;

            if (entry.tag == LS_T_CONS) {
                var_sym = ls_car(entry);
                ls_value_t init_rest = ls_cdr(entry);
                if (init_rest.tag == LS_T_CONS) init_form = ls_car(init_rest);
            } else {
                var_sym = entry;
            }

            if (var_sym.tag == LS_T_SYMBOL) {
                ls_symbol_t *sym = (ls_symbol_t *)var_sym.u.ptr;
                const char *cname = ctx_push_local(ctx, sym->name->chars);
                fprintf(out, "ls_value_t %s = ", cname);
                emit_expr(out, init_form, ctx);
                fprintf(out, "; ");
            }
            cur = ls_cdr(cur);
        }
        emit_body(out, body, ctx);
        fprintf(out, "})");

        ctx->scope_depth = saved_depth;
        ctx_pop_to_depth(ctx, saved_depth);
        ctx->n_locals = saved;
        return;
    }

    /* (lambda (args) body...) */
    if (form_head_is(L, form, L->sym_lambda)) {
        ls_value_t lambda_list = ls_car(rest);
        ls_value_t body = ls_cdr(rest);

        /* Lift this lambda to a top-level C function. */
        int lid = ctx->n_lambdas;
        if (lid >= AOT_MAX_LAMBDAS) {
            ls_error(L, "AOT: too many lambdas");
            emit_nil(out);
            return;
        }
        lambda_info_t *li = &ctx->lambdas[lid];
        li->id = lid;
        snprintf(li->c_name, sizeof li->c_name, "ls_lambda_%d", lid);
        ctx->n_lambdas++;

        /* Write the lambda body to the deferred buffer. */
        FILE *d = ctx->deferred;
        fprintf(d, "\n/* lambda #%d */\n", lid);
        fprintf(d, "static ls_value_t %s(ls_state_t *L, int nargs, ls_value_t *args) {\n",
                li->c_name);
        fprintf(d, "    (void)L; (void)nargs;\n");

        /* Bind parameters. */
        compiler_ctx_t sub = *ctx;
        sub.n_locals = 0;
        sub.scope_depth = 0;
        sub.indent = 1;

        ls_value_t param = lambda_list;
        int pidx = 0;
        int in_rest = 0;
        while (param.tag == LS_T_CONS) {
            ls_value_t p = ls_car(param);
            if (sym_name_eq(p, "&REST") || sym_name_eq(p, "&BODY")) {
                in_rest = 1;
                param = ls_cdr(param);
                continue;
            }
            if (sym_name_eq(p, "&OPTIONAL") || sym_name_eq(p, "&KEY") || sym_name_eq(p, "&AUX")) {
                param = ls_cdr(param);
                continue;
            }
            if (p.tag == LS_T_SYMBOL) {
                ls_symbol_t *sym = (ls_symbol_t *)p.u.ptr;
                const char *cname = ctx_push_local(&sub, sym->name->chars);
                if (in_rest) {
                    fprintf(d, "    ls_value_t %s = LS_NIL; /* &rest -- TODO build list */\n", cname);
                } else {
                    fprintf(d, "    ls_value_t %s = args[%d];\n", cname, pidx);
                }
            }
            pidx++;
            param = ls_cdr(param);
        }

        /* Emit body. */
        fprintf(d, "    return ");
        emit_body(d, body, &sub);
        fprintf(d, ";\n}\n");

        /* In the caller, reference the lifted function.  For now we use a
         * runtime wrapper that converts the C function pointer. */
        fprintf(out, "lsrt_intern(L, NULL, \"%s\") /* TODO closure capture */", li->c_name);
        return;
    }

    /* (defun name (args) body...) -- when encountered in expression position
     * (rare, but we handle it). */
    if (form_head_is(L, form, L->sym_defun)) {
        /* Emit as a global function def + return the symbol name. */
        emit_toplevel_defun(out, form, ctx);
        ls_value_t name_sym = ls_car(rest);
        if (name_sym.tag == LS_T_SYMBOL) {
            ls_symbol_t *sym = (ls_symbol_t *)name_sym.u.ptr;
            fprintf(out, "lsrt_intern(L, \"COMMON-LISP-USER\", \"%s\")", sym->name->chars);
        } else {
            emit_nil(out);
        }
        return;
    }

    /* (and ...) */
    if (form_head_is(L, form, L->sym_and)) {
        size_t len = ls_list_length(rest);
        if (len == 0) { emit_t(out); return; }
        if (len == 1) { emit_expr(out, ls_car(rest), ctx); return; }

        fprintf(out, "({ls_value_t _and;");
        ls_value_t cur = rest;
        while (cur.tag == LS_T_CONS) {
            fprintf(out, " _and = ");
            emit_expr(out, ls_car(cur), ctx);
            fprintf(out, ";");
            ls_value_t next = ls_cdr(cur);
            if (next.tag == LS_T_CONS) {
                fprintf(out, " if (LS_NILP(_and)) goto _and_done_%d;", ctx->label_counter);
            }
            cur = next;
        }
        fprintf(out, " _and_done_%d: _and; })", ctx->label_counter);
        ctx->label_counter++;
        return;
    }

    /* (or ...) */
    if (form_head_is(L, form, L->sym_or)) {
        size_t len = ls_list_length(rest);
        if (len == 0) { emit_nil(out); return; }
        if (len == 1) { emit_expr(out, ls_car(rest), ctx); return; }

        fprintf(out, "({ls_value_t _or;");
        ls_value_t cur = rest;
        while (cur.tag == LS_T_CONS) {
            fprintf(out, " _or = ");
            emit_expr(out, ls_car(cur), ctx);
            fprintf(out, ";");
            ls_value_t next = ls_cdr(cur);
            if (next.tag == LS_T_CONS) {
                fprintf(out, " if (!LS_NILP(_or)) goto _or_done_%d;", ctx->label_counter);
            }
            cur = next;
        }
        fprintf(out, " _or_done_%d: _or; })", ctx->label_counter);
        ctx->label_counter++;
        return;
    }

    /* (when test body...) */
    if (form_head_is(L, form, L->sym_when)) {
        ls_value_t test = ls_car(rest);
        ls_value_t body = ls_cdr(rest);
        fprintf(out, "(!LS_NILP(");
        emit_expr(out, test, ctx);
        fprintf(out, ") ? (");
        emit_body(out, body, ctx);
        fprintf(out, ") : LS_NIL)");
        return;
    }

    /* (unless test body...) */
    if (form_head_is(L, form, L->sym_unless)) {
        ls_value_t test = ls_car(rest);
        ls_value_t body = ls_cdr(rest);
        fprintf(out, "(LS_NILP(");
        emit_expr(out, test, ctx);
        fprintf(out, ") ? (");
        emit_body(out, body, ctx);
        fprintf(out, ") : LS_NIL)");
        return;
    }

    /* (cond (test body...)...) */
    if (form_head_is(L, form, L->sym_cond)) {
        ls_value_t clauses = rest;
        if (clauses.tag != LS_T_CONS) { emit_nil(out); return; }

        fprintf(out, "({ls_value_t _cond = LS_NIL;");
        int cond_id = ctx->label_counter++;
        ls_value_t cur = clauses;
        int clause_idx = 0;
        while (cur.tag == LS_T_CONS) {
            ls_value_t clause = ls_car(cur);
            ls_value_t test = ls_car(clause);
            ls_value_t body = ls_cdr(clause);

            if (sym_name_eq(test, "T") || (test.tag == LS_T_T)) {
                /* Default clause. */
                fprintf(out, " _cond = ");
                if (body.tag == LS_T_CONS) emit_body(out, body, ctx);
                else emit_t(out);
                fprintf(out, "; goto _cond_done_%d;", cond_id);
            } else {
                fprintf(out, " if (!LS_NILP(");
                emit_expr(out, test, ctx);
                fprintf(out, ")) { _cond = ");
                if (body.tag == LS_T_CONS)
                    emit_body(out, body, ctx);
                else {
                    emit_expr(out, test, ctx);
                }
                fprintf(out, "; goto _cond_done_%d; }", cond_id);
            }
            clause_idx++;
            cur = ls_cdr(cur);
        }
        fprintf(out, " _cond_done_%d: _cond; })", cond_id);
        return;
    }

    /* (not x) / (null x) */
    if (head.tag == LS_T_SYMBOL) {
        ls_symbol_t *hs = (ls_symbol_t *)head.u.ptr;
        if (strcmp(hs->name->chars, "NOT") == 0 || strcmp(hs->name->chars, "NULL") == 0) {
            if (rest.tag == LS_T_CONS) {
                fprintf(out, "(LS_NILP(");
                emit_expr(out, ls_car(rest), ctx);
                fprintf(out, ") ? LS_TRUE : LS_NIL)");
                return;
            }
        }

        /* (eq a b) */
        if (strcmp(hs->name->chars, "EQ") == 0) {
            if (ls_list_length(rest) == 2) {
                fprintf(out, "(lsrt_eq(");
                emit_expr(out, ls_car(rest), ctx);
                fprintf(out, ", ");
                emit_expr(out, ls_car(ls_cdr(rest)), ctx);
                fprintf(out, ") ? LS_TRUE : LS_NIL)");
                return;
            }
        }

        /* (eql a b) */
        if (strcmp(hs->name->chars, "EQL") == 0) {
            if (ls_list_length(rest) == 2) {
                fprintf(out, "(lsrt_eql(");
                emit_expr(out, ls_car(rest), ctx);
                fprintf(out, ", ");
                emit_expr(out, ls_car(ls_cdr(rest)), ctx);
                fprintf(out, ") ? LS_TRUE : LS_NIL)");
                return;
            }
        }

        /* (equal a b) */
        if (strcmp(hs->name->chars, "EQUAL") == 0) {
            if (ls_list_length(rest) == 2) {
                fprintf(out, "(lsrt_equal(");
                emit_expr(out, ls_car(rest), ctx);
                fprintf(out, ", ");
                emit_expr(out, ls_car(ls_cdr(rest)), ctx);
                fprintf(out, ") ? LS_TRUE : LS_NIL)");
                return;
            }
        }

        /* (cons a b) */
        if (strcmp(hs->name->chars, "CONS") == 0 && ls_list_length(rest) == 2) {
            fprintf(out, "lsrt_cons(L, ");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, ", ");
            emit_expr(out, ls_car(ls_cdr(rest)), ctx);
            fprintf(out, ")");
            return;
        }

        /* (car x) */
        if (strcmp(hs->name->chars, "CAR") == 0 && ls_list_length(rest) == 1) {
            fprintf(out, "lsrt_car(");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, ")");
            return;
        }

        /* (cdr x) */
        if (strcmp(hs->name->chars, "CDR") == 0 && ls_list_length(rest) == 1) {
            fprintf(out, "lsrt_cdr(");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, ")");
            return;
        }

        /* (list a b c ...) */
        if (strcmp(hs->name->chars, "LIST") == 0) {
            size_t len = ls_list_length(rest);
            fprintf(out, "lsrt_list(L, %d", (int)len);
            ls_value_t cur = rest;
            while (cur.tag == LS_T_CONS) {
                fprintf(out, ", ");
                emit_expr(out, ls_car(cur), ctx);
                cur = ls_cdr(cur);
            }
            fprintf(out, ")");
            return;
        }

        /* (print x) / (princ x) / (terpri) */
        if (strcmp(hs->name->chars, "PRINT") == 0 && ls_list_length(rest) >= 1) {
            fprintf(out, "(lsrt_println(L, ");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, "), ");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, ")");
            return;
        }
        if (strcmp(hs->name->chars, "PRINC") == 0 && ls_list_length(rest) >= 1) {
            fprintf(out, "(lsrt_print(L, ");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, "), ");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, ")");
            return;
        }

        /* Arithmetic ops: + - * / mod < > <= >= = */
        const arith_op_t *aop = find_arith_op(hs->name->chars);
        if (aop) {
            emit_arith_nary(out, aop, rest, ctx);
            return;
        }

        /* (funcall fn args...) */
        if (strcmp(hs->name->chars, "FUNCALL") == 0 && rest.tag == LS_T_CONS) {
            int nargs = (int)ls_list_length(rest) - 1;
            fprintf(out, "({ls_value_t _fc_args[%d]; ", nargs > 0 ? nargs : 1);
            ls_value_t cur = ls_cdr(rest);
            int ai = 0;
            while (cur.tag == LS_T_CONS) {
                fprintf(out, "_fc_args[%d] = ", ai);
                emit_expr(out, ls_car(cur), ctx);
                fprintf(out, "; ");
                ai++;
                cur = ls_cdr(cur);
            }
            fprintf(out, "lsrt_apply(L, ");
            emit_expr(out, ls_car(rest), ctx);
            fprintf(out, ", %d, _fc_args); })", nargs);
            return;
        }
    }

    /* ---- Generic function call: (fn arg1 arg2 ...) ---- */
    {
        size_t nargs = ls_list_length(rest);
        int arr_sz = (int)(nargs > 0 ? nargs : 1);

        fprintf(out, "({");
        /* Evaluate function. */
        fprintf(out, "ls_value_t _fn = ");
        if (head.tag == LS_T_SYMBOL) {
            ls_symbol_t *hs = (ls_symbol_t *)head.u.ptr;
            char c_fn_name[128] = "user_";
            sanitise_name(hs->name->chars, c_fn_name + 5, sizeof(c_fn_name) - 5);

            /* Check if this is a user-defined function we know about --
             * for now, always go through runtime lookup. */
            const char *pkg = hs->package ? hs->package->name->chars : "COMMON-LISP";
            fprintf(out, "lsrt_intern(L, \"%s\", \"%s\")", pkg, hs->name->chars);
        } else {
            emit_expr(out, head, ctx);
        }
        fprintf(out, "; ");

        /* Evaluate arguments. */
        fprintf(out, "ls_value_t _call_args[%d]; ", arr_sz);
        ls_value_t cur = rest;
        int ai = 0;
        while (cur.tag == LS_T_CONS) {
            fprintf(out, "_call_args[%d] = ", ai);
            emit_expr(out, ls_car(cur), ctx);
            fprintf(out, "; ");
            ai++;
            cur = ls_cdr(cur);
        }

        if (head.tag == LS_T_SYMBOL) {
            ls_symbol_t *hs = (ls_symbol_t *)head.u.ptr;
            /* Try direct C call for known user functions. */
            char c_fn_name[128] = "user_";
            sanitise_name(hs->name->chars, c_fn_name + 5, sizeof(c_fn_name) - 5);
            /* Use lsrt_funcall which does the symbol lookup. */
            fprintf(out, "lsrt_funcall(L, \"%s\", %d", hs->name->chars, (int)nargs);
            for (int i = 0; i < (int)nargs; i++)
                fprintf(out, ", _call_args[%d]", i);
            fprintf(out, ");");
        } else {
            fprintf(out, "lsrt_apply(L, _fn, %d, _call_args);", (int)nargs);
        }

        fprintf(out, "})");
    }
}

/* Emit a body (list of forms), returning the value of the last one. */
static void emit_body(FILE *out, ls_value_t forms, compiler_ctx_t *ctx) {
    if (forms.tag != LS_T_CONS) {
        emit_nil(out);
        return;
    }

    size_t len = ls_list_length(forms);
    if (len == 1) {
        emit_expr(out, ls_car(forms), ctx);
        return;
    }

    fprintf(out, "({");
    ls_value_t cur = forms;
    size_t idx = 0;
    while (cur.tag == LS_T_CONS) {
        idx++;
        emit_expr(out, ls_car(cur), ctx);
        fprintf(out, "; ");
        cur = ls_cdr(cur);
    }
    fprintf(out, "})");
}

/* ================================================================
 *  Top-level form emitters
 * ================================================================ */

/* (defun name (args) body...) -> C function */
static void emit_toplevel_defun(FILE *out, ls_value_t form, compiler_ctx_t *ctx) {
    ls_state_t *L = ctx->L;
    ls_value_t rest = ls_cdr(form);
    ls_value_t name_sym = ls_car(rest);
    ls_value_t lambda_list = ls_car(ls_cdr(rest));
    ls_value_t body = ls_cdr(ls_cdr(rest));

    if (name_sym.tag != LS_T_SYMBOL) {
        ls_error(L, "AOT: defun name is not a symbol");
        return;
    }

    ls_symbol_t *sym = (ls_symbol_t *)name_sym.u.ptr;
    char c_name[128] = "user_";
    sanitise_name(sym->name->chars, c_name + 5, sizeof(c_name) - 5);

    /* Count parameters. */
    int n_params = 0;
    ls_value_t param = lambda_list;
    while (param.tag == LS_T_CONS) {
        ls_value_t p = ls_car(param);
        if (p.tag == LS_T_SYMBOL) {
            ls_symbol_t *ps = (ls_symbol_t *)p.u.ptr;
            if (ps->name->chars[0] == '&') {
                param = ls_cdr(param);
                continue;
            }
        }
        n_params++;
        param = ls_cdr(param);
    }

    /* Function signature. */
    fprintf(out, "static ls_value_t %s(ls_state_t *L, int nargs, ls_value_t *args) {\n",
            c_name);
    fprintf(out, "    (void)L; (void)nargs;\n");

    /* Bind parameters. */
    compiler_ctx_t sub = *ctx;
    sub.n_locals = 0;
    sub.scope_depth = 0;
    sub.indent = 1;

    param = lambda_list;
    int pidx = 0;
    int in_optional = 0;
    int in_rest = 0;
    while (param.tag == LS_T_CONS) {
        ls_value_t p = ls_car(param);

        if (sym_name_eq(p, "&OPTIONAL")) { in_optional = 1; param = ls_cdr(param); continue; }
        if (sym_name_eq(p, "&REST") || sym_name_eq(p, "&BODY")) { in_rest = 1; param = ls_cdr(param); continue; }
        if (sym_name_eq(p, "&KEY") || sym_name_eq(p, "&AUX") || sym_name_eq(p, "&ALLOW-OTHER-KEYS")) {
            param = ls_cdr(param);
            continue;
        }

        ls_value_t pname = p;
        ls_value_t pdefault = ls_nil_v();

        /* Handle (param default) form for &optional. */
        if (p.tag == LS_T_CONS) {
            pname = ls_car(p);
            ls_value_t pr = ls_cdr(p);
            if (pr.tag == LS_T_CONS) pdefault = ls_car(pr);
        }

        if (pname.tag == LS_T_SYMBOL) {
            ls_symbol_t *psym = (ls_symbol_t *)pname.u.ptr;
            const char *cname = ctx_push_local(&sub, psym->name->chars);

            if (in_rest) {
                /* Build a list from remaining args. */
                fprintf(out, "    ls_value_t %s = LS_NIL;\n", cname);
                fprintf(out, "    for (int _ri = nargs - 1; _ri >= %d; _ri--)\n", pidx);
                fprintf(out, "        %s = lsrt_cons(L, args[_ri], %s);\n", cname, cname);
            } else if (in_optional) {
                fprintf(out, "    ls_value_t %s = (%d < nargs) ? args[%d] : ", cname, pidx, pidx);
                emit_expr(out, pdefault, &sub);
                fprintf(out, ";\n");
            } else {
                fprintf(out, "    ls_value_t %s = args[%d];\n", cname, pidx);
            }
        }
        pidx++;
        param = ls_cdr(param);
    }

    /* Skip docstring if present. */
    ls_value_t effective_body = body;
    if (effective_body.tag == LS_T_CONS) {
        ls_value_t first = ls_car(effective_body);
        if (first.tag == LS_T_STRING && ls_cdr(effective_body).tag == LS_T_CONS) {
            effective_body = ls_cdr(effective_body);
        }
    }

    /* Emit body. */
    fprintf(out, "    return ");
    emit_body(out, effective_body, &sub);
    fprintf(out, ";\n");
    fprintf(out, "}\n\n");
}

/* Emit a top-level expression into the main() function body. */
static void emit_toplevel_expr(FILE *out, ls_value_t form, compiler_ctx_t *ctx) {
    emit_indent(out, ctx);
    emit_expr(out, form, ctx);
    fprintf(out, ";\n");
}

/* ================================================================
 *  File-level compilation driver
 * ================================================================ */

int ls_aot_compile_file(ls_state_t *L, const char *src_path,
                        const char *out_path, int target)
{
    /* ---- Read the source file ---- */
    FILE *src = fopen(src_path, "rb");
    if (!src) {
        ls_error(L, "AOT: cannot open source file '%s': %s", src_path, strerror(errno));
        return -1;
    }
    fseek(src, 0, SEEK_END);
    long flen = ftell(src);
    fseek(src, 0, SEEK_SET);
    char *source = (char *)malloc((size_t)flen + 1);
    if (!source) { fclose(src); ls_error(L, "AOT: out of memory"); return -1; }
    size_t nread = fread(source, 1, (size_t)flen, src);
    source[nread] = '\0';
    fclose(src);

    /* ---- Parse all top-level forms ---- */
    #define MAX_FORMS 4096
    ls_value_t forms[MAX_FORMS];
    int n_forms = 0;

    const char *pos = source;
    while (*pos) {
        /* Skip whitespace and comments. */
        while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
            pos++;
        if (*pos == ';') { while (*pos && *pos != '\n') pos++; continue; }
        if (*pos == '\0') break;

        const char *end = NULL;
        ls_value_t form = ls_read_from_string(L, pos, &end);
        if (!end || end == pos) break;
        pos = end;

        if (n_forms >= MAX_FORMS) {
            ls_error(L, "AOT: too many top-level forms");
            free(source);
            return -1;
        }
        forms[n_forms++] = form;
    }
    free(source);

    /* ---- Construct output .c path ---- */
    char c_path[1024];
    snprintf(c_path, sizeof c_path, "%s.aot.c", out_path);

    FILE *out = fopen(c_path, "w");
    if (!out) {
        ls_error(L, "AOT: cannot create output '%s': %s", c_path, strerror(errno));
        return -1;
    }

    /* ---- Initialize compiler context ---- */
    compiler_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.L = L;
    ctx.out = out;
    ctx.indent = 2;

    /* Open a temp file for deferred lambda bodies. */
    char defer_path[1024];
    snprintf(defer_path, sizeof defer_path, "%s.defer.c", out_path);
    ctx.deferred = fopen(defer_path, "w+");
    if (!ctx.deferred) ctx.deferred = tmpfile();

    /* ---- Emit the runtime header ---- */
    fprintf(out, "%s", aot_runtime_header);

    /* ---- Forward declarations ---- */
    fprintf(out, "/* Forward declarations for user functions */\n");
    for (int i = 0; i < n_forms; i++) {
        if (form_head_is(L, forms[i], L->sym_defun)) {
            ls_value_t r = ls_cdr(forms[i]);
            ls_value_t name_sym = ls_car(r);
            if (name_sym.tag == LS_T_SYMBOL) {
                ls_symbol_t *sym = (ls_symbol_t *)name_sym.u.ptr;
                char c_name[128] = "user_";
                sanitise_name(sym->name->chars, c_name + 5, sizeof(c_name) - 5);
                fprintf(out, "static ls_value_t %s(ls_state_t *L, int nargs, ls_value_t *args);\n",
                        c_name);
            }
        }
    }
    fprintf(out, "\n");

    /* ---- Emit defuns ---- */
    for (int i = 0; i < n_forms; i++) {
        if (form_head_is(L, forms[i], L->sym_defun)) {
            emit_toplevel_defun(out, forms[i], &ctx);
        }
    }

    /* ---- Emit deferred lambda bodies ---- */
    if (ctx.deferred) {
        fflush(ctx.deferred);
        fseek(ctx.deferred, 0, SEEK_SET);
        char buf[4096];
        size_t nr;
        while ((nr = fread(buf, 1, sizeof buf, ctx.deferred)) > 0)
            fwrite(buf, 1, nr, out);
        fclose(ctx.deferred);
        /* Clean up temp file. */
        remove(defer_path);
    }

    /* ---- Emit main() ---- */
    fprintf(out, "\n/* ---- Entry point ---- */\n");
    fprintf(out, "int main(int argc, char **argv) {\n");
    fprintf(out, "    (void)argc; (void)argv;\n");
    fprintf(out, "    ls_state_t *L = lsrt_init();\n");
    fprintf(out, "    if (!L) { fprintf(stderr, \"Failed to init runtime\\n\"); return 1; }\n\n");

    /* Register user defun functions with the runtime. */
    for (int i = 0; i < n_forms; i++) {
        if (form_head_is(L, forms[i], L->sym_defun)) {
            ls_value_t r = ls_cdr(forms[i]);
            ls_value_t name_sym = ls_car(r);
            if (name_sym.tag == LS_T_SYMBOL) {
                ls_symbol_t *sym = (ls_symbol_t *)name_sym.u.ptr;
                char c_name[128] = "user_";
                sanitise_name(sym->name->chars, c_name + 5, sizeof(c_name) - 5);

                /* Count params for min/max args. */
                ls_value_t ll = ls_car(ls_cdr(r));
                int min_args = 0, max_args = 0;
                int has_rest = 0;
                ls_value_t p = ll;
                while (p.tag == LS_T_CONS) {
                    ls_value_t pv = ls_car(p);
                    if (sym_name_eq(pv, "&REST") || sym_name_eq(pv, "&BODY")) { has_rest = 1; }
                    else if (sym_name_eq(pv, "&OPTIONAL") || sym_name_eq(pv, "&KEY")) { /* skip */ }
                    else if (pv.tag == LS_T_SYMBOL && ((ls_symbol_t *)pv.u.ptr)->name->chars[0] != '&') {
                        max_args++;
                        if (!has_rest) min_args = max_args;
                    }
                    p = ls_cdr(p);
                }
                if (has_rest) max_args = -1;

                fprintf(out, "    /* register %s */\n", sym->name->chars);
                fprintf(out, "    /* lsrt_register(L, \"%s\", %s, %d, %d); */\n",
                        sym->name->chars, c_name, min_args, max_args);
            }
        }
    }
    fprintf(out, "\n");

    /* Emit top-level non-defun expressions. */
    for (int i = 0; i < n_forms; i++) {
        if (!form_head_is(L, forms[i], L->sym_defun) &&
            !form_head_is(L, forms[i], L->sym_defmacro)) {
            emit_toplevel_expr(out, forms[i], &ctx);
        }
    }

    fprintf(out, "\n    lsrt_shutdown(L);\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
    fclose(out);

    /* ---- Invoke the C compiler ---- */
    const char *cc = L->cc_path ? L->cc_path : "gcc";
    char cmd[2048];

    switch (target) {
    case 0: /* LS_TARGET_NATIVE_EXE */
        snprintf(cmd, sizeof cmd,
                 "\"%s\" -O2 -o \"%s\" \"%s\" -Iinclude -lm",
                 cc, out_path, c_path);
        break;

    case 1: /* LS_TARGET_PE32PLUS */
        snprintf(cmd, sizeof cmd,
                 "\"%s\" -O2 -o \"%s\" \"%s\" -Iinclude -lm "
                 "-Wl,--subsystem,console",
                 cc, out_path, c_path);
        break;

    case 2: /* LS_TARGET_ELF64 */
#ifdef _WIN32
        /* On Windows, try cross-compiler. */
        snprintf(cmd, sizeof cmd,
                 "x86_64-linux-gnu-gcc -O2 -o \"%s\" \"%s\" "
                 "-Iinclude -lm -static",
                 out_path, c_path);
#else
        snprintf(cmd, sizeof cmd,
                 "\"%s\" -O2 -o \"%s\" \"%s\" -Iinclude -lm",
                 cc, out_path, c_path);
#endif
        break;

    default:
        ls_error(L, "AOT: unknown target %d", target);
        return -1;
    }

    if (L->verbose) {
        printf("[AOT] %s\n", cmd);
    }

    int rc = system(cmd);
    if (rc != 0) {
        /* The .c file is left behind for debugging. */
        if (L->verbose) {
            printf("[AOT] compiler returned %d, generated file: %s\n", rc, c_path);
        }
        /* Don't error -- the .c file is still useful. */
        return rc;
    }

    /* Optionally clean up the generated .c file on success. */
    if (!L->verbose) {
        remove(c_path);
    }

    return 0;
}

/* ================================================================
 *  Lisp-level (compile-file src out &optional target) binding
 * ================================================================ */

static ls_value_t bi_compile_file(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2 || args[0].tag != LS_T_STRING || args[1].tag != LS_T_STRING) {
        ls_error(L, "compile-file: requires (src-path out-path &optional target)");
        return ls_nil_v();
    }

    ls_string_t *src = (ls_string_t *)args[0].u.ptr;
    ls_string_t *out = (ls_string_t *)args[1].u.ptr;
    int target = 0;

    if (nargs >= 3) {
        if (args[2].tag == LS_T_FIXNUM) {
            target = (int)args[2].u.fixnum;
        } else if (args[2].tag == LS_T_SYMBOL) {
            ls_symbol_t *ts = (ls_symbol_t *)args[2].u.ptr;
            if (strcmp(ts->name->chars, "ELF") == 0 || strcmp(ts->name->chars, "ELF64") == 0)
                target = 2;
            else if (strcmp(ts->name->chars, "PE") == 0 || strcmp(ts->name->chars, "PE32+") == 0)
                target = 1;
        }
    }

    int rc = ls_aot_compile_file(L, src->chars, out->chars, target);
    return rc == 0 ? ls_t_v() : ls_nil_v();
}

/* Register the AOT compilation function. */
void ls_init_aot(ls_state_t *L) {
    ls_defun(L, "LITESRPENT-SYSTEM", "COMPILE-FILE", bi_compile_file, 2, 3);
}

/* REPL entry point: in true AOT mode each top-level form would be
 * transpiled to C, compiled, dlopen'd, and called.  That round-trip
 * doesn't make sense for one-liners, so for the REPL we evaluate via
 * the interpreter and announce the AOT mode is active.  Use
 * (compile-file ...) to actually produce a binary. */
ls_value_t ls_aot_compile_and_run(ls_state_t *L, ls_value_t form) {
    return ls_eval(L, form, L->genv);
}
