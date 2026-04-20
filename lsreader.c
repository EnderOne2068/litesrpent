/* lsreader.c -- lexer + reader for S-expressions.
 *
 * Accepts the usual Common Lisp read syntax: symbols, numbers
 * (decimal integer, hex #x, octal #o, binary #b, ratios, floats,
 * negative numbers), strings "...", characters #\x, quote '...,
 * quasiquote `..., unquote ,..., splicing ,@..., vectors #(...)
 * and a few reader macros (#+, #- feature tests and #|...|# block
 * comments, ;... line comments, #'symbol for (function symbol)).
 *
 * The reader returns the parsed S-expression; it does not evaluate.
 */
#include "lscore.h"
#include <ctype.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

typedef struct {
    ls_state_t *L;
    const char *src;
    size_t      pos;
    size_t      len;
    int         line;
    int         col;
} reader_t;

static int peek(reader_t *r) { return r->pos < r->len ? (unsigned char)r->src[r->pos] : -1; }
static int peek2(reader_t *r, size_t off) {
    return r->pos + off < r->len ? (unsigned char)r->src[r->pos + off] : -1;
}
static int get(reader_t *r) {
    if (r->pos >= r->len) return -1;
    int c = (unsigned char)r->src[r->pos++];
    if (c == '\n') { r->line++; r->col = 0; } else r->col++;
    return c;
}

static void skip_ws_and_comments(reader_t *r) {
    for (;;) {
        int c = peek(r);
        if (c == -1) return;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',') {
            if (c == ',') {
                /* in CL, comma is reserved for unquote -- not whitespace.
                 * We check upstream for that; here, only skip if preceded
                 * by nothing special. Actually never skip: rely on unquote. */
                return;
            }
            get(r);
            continue;
        }
        if (c == ';') { while (c != -1 && c != '\n') c = get(r); continue; }
        if (c == '#' && peek2(r, 1) == '|') {
            get(r); get(r); int depth = 1;
            while (depth) {
                int d = get(r);
                if (d == -1) return;
                if (d == '#' && peek(r) == '|') { get(r); depth++; }
                else if (d == '|' && peek(r) == '#') { get(r); depth--; }
            }
            continue;
        }
        return;
    }
}

static int is_sym_char(int c) {
    if (c == -1) return 0;
    if (isspace(c)) return 0;
    if (c == '(' || c == ')' || c == '"' || c == '\'' || c == '`' ||
        c == ',' || c == ';' || c == '|') return 0;
    return 1;
}

static ls_value_t read_expr(reader_t *r);

static ls_value_t read_string(reader_t *r) {
    get(r); /* opening quote */
    char buf[4096];
    size_t n = 0;
    size_t cap = sizeof buf;
    char *heap = NULL;
    char *dst = buf;
    for (;;) {
        int c = get(r);
        if (c == -1) { ls_error(r->L, "unterminated string"); break; }
        if (c == '"') break;
        if (c == '\\') {
            int d = get(r);
            switch (d) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            default: c = d;
            }
        }
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nh = (char *)malloc(ncap);
            memcpy(nh, dst, n);
            if (heap) free(heap);
            heap = nh; dst = nh; cap = ncap;
        }
        dst[n++] = (char)c;
    }
    ls_value_t v = ls_make_string(r->L, dst, n);
    if (heap) free(heap);
    return v;
}

static ls_value_t parse_number_or_symbol(ls_state_t *L, const char *buf, size_t n) {
    /* Try to parse as number.  Supported:
     *   integer: optional sign, [0-9]+
     *   ratio:   [0-9]+/[0-9]+
     *   float:   [-+]?\d+(\.\d+)?([eE][-+]?\d+)?
     */
    if (n == 0) return ls_intern(L, NULL, "||");
    size_t i = 0;
    int sign = 1;
    if (buf[0] == '+' || buf[0] == '-') { if (buf[0] == '-') sign = -1; i = 1; }
    if (i == n) goto as_symbol;
    /* check if all digits (with one optional '.', 'e', '/'), a valid number */
    int saw_digit = 0, saw_dot = 0, saw_exp = 0, saw_slash = 0;
    for (size_t j = i; j < n; j++) {
        char c = buf[j];
        if (isdigit((unsigned char)c)) { saw_digit = 1; continue; }
        if (c == '.' && !saw_dot && !saw_exp && !saw_slash) { saw_dot = 1; continue; }
        if ((c == 'e' || c == 'E' || c == 'd' || c == 'D' || c == 'f' || c == 'F')
            && saw_digit && !saw_exp && !saw_slash) {
            saw_exp = 1;
            if (j + 1 < n && (buf[j+1] == '+' || buf[j+1] == '-')) j++;
            continue;
        }
        if (c == '/' && saw_digit && !saw_dot && !saw_exp && !saw_slash) {
            saw_slash = 1; saw_digit = 0; continue;
        }
        goto as_symbol;
    }
    if (!saw_digit) goto as_symbol;

    if (saw_dot || saw_exp) {
        double d = strtod(buf, NULL);
        return ls_make_flonum(d);
    }
    if (saw_slash) {
        /* represent ratio as cons (num . denom) for now */
        long num = 0, den = 0;
        size_t k = 0;
        if (buf[0] == '+' || buf[0] == '-') k = 1;
        while (k < n && buf[k] != '/') { num = num * 10 + (buf[k] - '0'); k++; }
        if (sign < 0) num = -num;
        k++;
        while (k < n) { den = den * 10 + (buf[k] - '0'); k++; }
        if (den == 0) return ls_make_fixnum(0);
        if (num % den == 0) return ls_make_fixnum(num / den);
        /* just store as float for now */
        return ls_make_flonum((double)num / (double)den);
    }
    int64_t x = 0;
    size_t k = (buf[0] == '+' || buf[0] == '-') ? 1 : 0;
    while (k < n) { x = x * 10 + (buf[k] - '0'); k++; }
    return ls_make_fixnum(sign * x);

as_symbol:
    /* Symbol.  Handle package prefix a:b or a::b, keyword :foo,
     * uninterned #:foo, and ||-quoted names. */
    if (n >= 1 && buf[0] == ':') {
        /* keyword -- upcase the name */
        char kwn[512];
        size_t klen = n - 1;
        if (klen > 511) klen = 511;
        for (size_t k = 0; k < klen; k++) kwn[k] = (char)toupper((unsigned char)buf[k+1]);
        kwn[klen] = 0;
        ls_package_t *kp = ls_ensure_package(L, "KEYWORD");
        int ext;
        return ls_intern_sym(L, kp, kwn, klen, &ext);
    }
    /* look for package prefix */
    for (size_t j = 0; j < n; j++) {
        if (buf[j] == ':') {
            int dbl = (j + 1 < n && buf[j+1] == ':') ? 1 : 0;
            char pkg[256], name[512];
            if (j > 255) j = 255;
            memcpy(pkg, buf, j); pkg[j] = 0;
            for (size_t k = 0; k < j; k++) pkg[k] = (char)toupper((unsigned char)pkg[k]);
            size_t off = j + 1 + dbl;
            size_t m   = n - off;
            if (m > 511) m = 511;
            memcpy(name, buf + off, m); name[m] = 0;
            for (size_t k = 0; k < m; k++) name[k] = (char)toupper((unsigned char)name[k]);
            ls_package_t *p = ls_ensure_package(L, pkg);
            int ext;
            return ls_intern_sym(L, p, name, m, &ext);
        }
    }
    /* uppercase default */
    char tmp[512];
    if (n > 511) n = 511;
    for (size_t k = 0; k < n; k++) tmp[k] = (char)toupper((unsigned char)buf[k]);
    /* Special atoms NIL and T */
    if (n == 3 && tmp[0] == 'N' && tmp[1] == 'I' && tmp[2] == 'L') return ls_nil_v();
    if (n == 1 && tmp[0] == 'T') return ls_t_v();
    return ls_intern_sym(L, L->current_package, tmp, n, NULL);
}

static ls_value_t read_token(reader_t *r) {
    char buf[512];
    size_t n = 0;
    while (is_sym_char(peek(r))) {
        if (n + 1 >= sizeof buf) { ls_error(r->L, "token too long"); break; }
        buf[n++] = (char)get(r);
    }
    buf[n] = 0;
    return parse_number_or_symbol(r->L, buf, n);
}

static ls_value_t read_list(reader_t *r) {
    get(r); /* '(' */
    ls_value_t head = ls_nil_v();
    ls_value_t tail = ls_nil_v();
    for (;;) {
        skip_ws_and_comments(r);
        int c = peek(r);
        if (c == -1) { ls_error(r->L, "unterminated list"); return ls_nil_v(); }
        if (c == ')') { get(r); return head; }
        if (c == '.' && (peek2(r, 1) == ' ' || peek2(r, 1) == '\t' || peek2(r, 1) == '\n')) {
            get(r);
            ls_value_t tailv = read_expr(r);
            if (tail.tag == LS_T_CONS) ((ls_cons_t *)tail.u.ptr)->cdr = tailv;
            else head = tailv;
            skip_ws_and_comments(r);
            if (peek(r) != ')') { ls_error(r->L, "expected ')' after dotted tail"); return head; }
            get(r);
            return head;
        }
        ls_value_t item = read_expr(r);
        ls_value_t cell = ls_cons(r->L, item, ls_nil_v());
        if (head.tag == LS_T_NIL) head = tail = cell;
        else { ((ls_cons_t *)tail.u.ptr)->cdr = cell; tail = cell; }
    }
}

static ls_value_t read_quote_like(reader_t *r, const char *opname) {
    get(r);
    ls_value_t q = read_expr(r);
    ls_value_t sym = ls_intern(r->L, "COMMON-LISP", opname);
    return ls_cons(r->L, sym, ls_cons(r->L, q, ls_nil_v()));
}

static ls_value_t read_char(reader_t *r) {
    /* already consumed '#\\' */
    /* read char name: letters/digits */
    char buf[16]; size_t n = 0;
    int first = get(r);
    if (first == -1) { ls_error(r->L, "bad #\\"); return ls_nil_v(); }
    buf[n++] = (char)first;
    while (n < sizeof buf - 1 && isalpha(peek(r))) buf[n++] = (char)get(r);
    buf[n] = 0;
    if (n == 1) return ls_make_char((uint32_t)buf[0]);
    if (strcasecmp(buf, "space")   == 0) return ls_make_char(' ');
    if (strcasecmp(buf, "tab")     == 0) return ls_make_char('\t');
    if (strcasecmp(buf, "newline") == 0) return ls_make_char('\n');
    if (strcasecmp(buf, "return")  == 0) return ls_make_char('\r');
    if (strcasecmp(buf, "null")    == 0) return ls_make_char('\0');
    if (strcasecmp(buf, "linefeed")== 0) return ls_make_char('\n');
    if (strcasecmp(buf, "page")    == 0) return ls_make_char('\f');
    if (strcasecmp(buf, "backspace")==0) return ls_make_char('\b');
    if (strcasecmp(buf, "rubout")  == 0) return ls_make_char(127);
    return ls_make_char((uint32_t)buf[0]);
}

static ls_value_t read_expr(reader_t *r) {
    skip_ws_and_comments(r);
    int c = peek(r);
    if (c == -1) return ls_nil_v();
    if (c == '(') return read_list(r);
    if (c == ')') { ls_error(r->L, "unexpected ')'"); get(r); return ls_nil_v(); }
    if (c == '"') return read_string(r);
    if (c == '\'') return read_quote_like(r, "QUOTE");
    if (c == '`')  return read_quote_like(r, "QUASIQUOTE");
    if (c == ',') {
        get(r);
        if (peek(r) == '@') { get(r);
            ls_value_t q = read_expr(r);
            ls_value_t sym = ls_intern(r->L, "COMMON-LISP", "UNQUOTE-SPLICING");
            return ls_cons(r->L, sym, ls_cons(r->L, q, ls_nil_v()));
        }
        ls_value_t q = read_expr(r);
        ls_value_t sym = ls_intern(r->L, "COMMON-LISP", "UNQUOTE");
        return ls_cons(r->L, sym, ls_cons(r->L, q, ls_nil_v()));
    }
    if (c == '#') {
        get(r);
        int d = get(r);
        switch (d) {
        case '\'': {
            ls_value_t q = read_expr(r);
            ls_value_t sym = ls_intern(r->L, "COMMON-LISP", "FUNCTION");
            return ls_cons(r->L, sym, ls_cons(r->L, q, ls_nil_v()));
        }
        case '\\': return read_char(r);
        case '(': {
            /* vector literal */
            r->pos--; /* push back '(' */
            ls_value_t list = read_list(r);
            ls_vector_t *v = ls_vec_new(r->L, ls_list_length(list), 0);
            while (list.tag == LS_T_CONS) {
                ls_vec_push(r->L, v, ((ls_cons_t *)list.u.ptr)->car);
                list = ((ls_cons_t *)list.u.ptr)->cdr;
            }
            return ls_wrap(LS_T_VECTOR, v);
        }
        case 'x': case 'X': {
            int64_t x = 0; int sign = 1;
            if (peek(r) == '-') { sign = -1; get(r); }
            while (isxdigit(peek(r))) {
                int ch = get(r);
                int d2 = (ch <= '9') ? ch - '0' :
                         (ch <= 'F') ? ch - 'A' + 10 : ch - 'a' + 10;
                x = x * 16 + d2;
            }
            return ls_make_fixnum(sign * x);
        }
        case 'o': case 'O': {
            int64_t x = 0; int sign = 1;
            if (peek(r) == '-') { sign = -1; get(r); }
            while (peek(r) >= '0' && peek(r) <= '7') x = x * 8 + (get(r) - '0');
            return ls_make_fixnum(sign * x);
        }
        case 'b': case 'B': {
            int64_t x = 0; int sign = 1;
            if (peek(r) == '-') { sign = -1; get(r); }
            while (peek(r) == '0' || peek(r) == '1') x = x * 2 + (get(r) - '0');
            return ls_make_fixnum(sign * x);
        }
        case 'p': case 'P': {
            /* pathname literal: just read a string and wrap it */
            ls_value_t s = read_string(r);
            ls_value_t sym = ls_intern(r->L, "COMMON-LISP", "PATHNAME");
            return ls_cons(r->L, sym, ls_cons(r->L, s, ls_nil_v()));
        }
        case '+': case '-': {
            /* feature test -- we just accept any feature for now */
            ls_value_t feat = read_expr(r);
            ls_value_t form = read_expr(r);
            (void)feat;
            return (d == '+') ? form : ls_nil_v();
        }
        case ':': {
            /* uninterned symbol */
            char buf[256]; size_t n = 0;
            while (is_sym_char(peek(r))) buf[n++] = (char)get(r);
            buf[n] = 0;
            for (size_t k = 0; k < n; k++) buf[k] = (char)toupper((unsigned char)buf[k]);
            ls_value_t sv = ls_make_obj(r->L, LS_T_SYMBOL, sizeof(ls_symbol_t));
            ls_symbol_t *s = (ls_symbol_t *)sv.u.ptr;
            s->name = (ls_string_t *)ls_make_string(r->L, buf, n).u.ptr;
            s->package = NULL; s->value = ls_nil_v();
            s->function = ls_nil_v(); s->plist = ls_nil_v();
            s->hash = ls_hash_string(buf, n);
            return sv;
        }
        default:
            ls_error(r->L, "unknown #%c reader macro", d);
            return ls_nil_v();
        }
    }
    return read_token(r);
}

ls_value_t ls_read_from_string(ls_state_t *L, const char *src, const char **end) {
    reader_t r = { L, src, 0, strlen(src), 1, 0 };
    ls_value_t v = read_expr(&r);
    if (end) *end = src + r.pos;
    return v;
}
