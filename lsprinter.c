/* lsprinter.c -- printer / write / print / prin1 / princ. */
#include "lscore.h"
#include <ctype.h>

static void putstr(ls_stream_t *s, const char *buf, size_t n) {
    if (!s) return;
    if (s->string_stream) {
        if (s->bufpos + n + 1 > s->bufcap) {
            size_t nc = s->bufcap ? s->bufcap * 2 : 128;
            while (nc < s->bufpos + n + 1) nc *= 2;
            s->buffer = (char *)realloc(s->buffer, nc);
            s->bufcap = nc;
        }
        memcpy(s->buffer + s->bufpos, buf, n);
        s->bufpos += n;
        if (s->bufpos > s->buflen) s->buflen = s->bufpos;
        s->buffer[s->bufpos] = 0;
    } else if (s->fp) {
        fwrite(buf, 1, n, s->fp);
    }
}

static void putch(ls_stream_t *s, char c) { putstr(s, &c, 1); }
static void putcstr(ls_stream_t *s, const char *z) { putstr(s, z, strlen(z)); }

static void print_escaped(ls_stream_t *s, const char *str, size_t n) {
    putch(s, '"');
    for (size_t i = 0; i < n; i++) {
        char c = str[i];
        if (c == '"' || c == '\\') putch(s, '\\');
        putch(s, c);
    }
    putch(s, '"');
}

static int print_rec(ls_state_t *L, ls_value_t v, ls_stream_t *s, int esc, int depth) {
    char buf[64];
    if (depth > 1000) { putcstr(s, "#<deep>"); return 0; }
    switch (v.tag) {
    case LS_T_NIL: putcstr(s, "NIL"); break;
    case LS_T_T:   putcstr(s, "T"); break;
    case LS_T_FIXNUM: snprintf(buf, sizeof buf, "%lld", (long long)v.u.fixnum); putcstr(s, buf); break;
    case LS_T_FLONUM: snprintf(buf, sizeof buf, "%.16g", v.u.flonum);
        /* add trailing .0 if it looks integral, to keep float identity visible */
        if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') && !strchr(buf, 'i'))
            strcat(buf, ".0");
        putcstr(s, buf); break;
    case LS_T_CHAR:
        if (esc) {
            putcstr(s, "#\\");
            if (v.u.character == ' ') putcstr(s, "Space");
            else if (v.u.character == '\n') putcstr(s, "Newline");
            else if (v.u.character == '\t') putcstr(s, "Tab");
            else if (v.u.character == '\r') putcstr(s, "Return");
            else { char cc = (char)v.u.character; putch(s, cc); }
        } else {
            char cc = (char)v.u.character; putch(s, cc);
        }
        break;
    case LS_T_SYMBOL: {
        ls_symbol_t *sym = (ls_symbol_t *)v.u.ptr;
        if (esc && sym->package == L->pkg_keyword) putch(s, ':');
        if (sym->name) putstr(s, sym->name->chars, sym->name->len);
        else putcstr(s, "#:?");
        break;
    }
    case LS_T_STRING: {
        ls_string_t *str = (ls_string_t *)v.u.ptr;
        if (esc) print_escaped(s, str->chars, str->len);
        else     putstr(s, str->chars, str->len);
        break;
    }
    case LS_T_CONS: {
        putch(s, '(');
        ls_value_t cur = v;
        int first = 1;
        while (cur.tag == LS_T_CONS) {
            if (!first) putch(s, ' ');
            first = 0;
            print_rec(L, ((ls_cons_t *)cur.u.ptr)->car, s, esc, depth + 1);
            cur = ((ls_cons_t *)cur.u.ptr)->cdr;
        }
        if (cur.tag != LS_T_NIL) {
            putcstr(s, " . ");
            print_rec(L, cur, s, esc, depth + 1);
        }
        putch(s, ')');
        break;
    }
    case LS_T_VECTOR: {
        ls_vector_t *vec = (ls_vector_t *)v.u.ptr;
        putcstr(s, "#(");
        for (size_t i = 0; i < vec->len; i++) {
            if (i) putch(s, ' ');
            print_rec(L, vec->data[i], s, esc, depth + 1);
        }
        putch(s, ')');
        break;
    }
    case LS_T_HASHTABLE:    putcstr(s, "#<HASH-TABLE>"); break;
    case LS_T_FUNCTION: {
        ls_function_t *f = (ls_function_t *)v.u.ptr;
        putcstr(s, "#<FUNCTION ");
        if (f->name) putstr(s, f->name->name->chars, f->name->name->len);
        else putcstr(s, "(ANON)");
        putch(s, '>');
        break;
    }
    case LS_T_BYTECODE: {
        ls_bytecode_fn_t *f = (ls_bytecode_fn_t *)v.u.ptr;
        putcstr(s, "#<COMPILED-FUNCTION ");
        if (f->name) putstr(s, f->name->name->chars, f->name->name->len);
        else putcstr(s, "(ANON)");
        putch(s, '>');
        break;
    }
    case LS_T_NATIVE: {
        ls_native_t *n = (ls_native_t *)v.u.ptr;
        putcstr(s, "#<BUILTIN ");
        if (n->name) putcstr(s, n->name);
        putch(s, '>');
        break;
    }
    case LS_T_PACKAGE: {
        ls_package_t *p = (ls_package_t *)v.u.ptr;
        putcstr(s, "#<PACKAGE ");
        putstr(s, p->name->chars, p->name->len);
        putch(s, '>');
        break;
    }
    case LS_T_STREAM:       putcstr(s, "#<STREAM>"); break;
    case LS_T_FOREIGN: {
        ls_foreign_t *f = (ls_foreign_t *)v.u.ptr;
        snprintf(buf, sizeof buf, "#<FOREIGN 0x%p>", f->ptr); putcstr(s, buf);
        break;
    }
    case LS_T_FOREIGN_LIB: {
        ls_foreign_lib_t *lib = (ls_foreign_lib_t *)v.u.ptr;
        putcstr(s, "#<FOREIGN-LIB ");
        if (lib->path) putstr(s, lib->path->chars, lib->path->len);
        putch(s, '>');
        break;
    }
    case LS_T_FOREIGN_FN: {
        ls_foreign_fn_t *fn = (ls_foreign_fn_t *)v.u.ptr;
        putcstr(s, "#<FOREIGN-FN ");
        if (fn->name) putstr(s, fn->name->chars, fn->name->len);
        putch(s, '>');
        break;
    }
    case LS_T_CLASS: {
        ls_class_t *c = (ls_class_t *)v.u.ptr;
        putcstr(s, "#<CLASS ");
        if (c->name) putstr(s, c->name->name->chars, c->name->name->len);
        putch(s, '>');
        break;
    }
    case LS_T_INSTANCE: {
        ls_instance_t *in = (ls_instance_t *)v.u.ptr;
        putcstr(s, "#<");
        if (in->class_ && in->class_->name)
            putstr(s, in->class_->name->name->chars, in->class_->name->name->len);
        putch(s, '>');
        break;
    }
    case LS_T_GENERIC: {
        ls_generic_t *g = (ls_generic_t *)v.u.ptr;
        putcstr(s, "#<GENERIC-FUNCTION ");
        if (g->name) putstr(s, g->name->name->chars, g->name->name->len);
        putch(s, '>');
        break;
    }
    case LS_T_CONDITION: {
        ls_condition_t *c = (ls_condition_t *)v.u.ptr;
        putcstr(s, "#<CONDITION ");
        if (c->class_ && c->class_->name)
            putstr(s, c->class_->name->name->chars, c->class_->name->name->len);
        putch(s, '>');
        break;
    }
    default:
        snprintf(buf, sizeof buf, "#<OBJECT tag=%u>", v.tag);
        putcstr(s, buf);
    }
    return 0;
}

int ls_print_value(ls_state_t *L, ls_value_t v, ls_stream_t *s, int escape) {
    if (!s) s = L->stdout_;
    return print_rec(L, v, s, escape, 0);
}
