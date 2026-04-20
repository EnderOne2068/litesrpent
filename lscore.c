/* lscore.c -- core value/type operations, allocator, hashtable,
 * vector, and the small utility functions every other module leans on. */
#include "lscore.h"
#include "litesrpent.h"
#include <assert.h>
#include <ctype.h>

/* ---------- Basic value builders ---------- */

ls_value_t ls_nil_v(void) {
    ls_value_t v;
    memset(&v, 0, sizeof v);
    v.tag = LS_T_NIL;
    return v;
}

ls_value_t ls_t_v(void) {
    ls_value_t v;
    memset(&v, 0, sizeof v);
    v.tag = LS_T_T;
    return v;
}

ls_value_t ls_nil(void) { return ls_nil_v(); }
ls_value_t ls_t(void)   { return ls_t_v(); }

ls_value_t ls_make_fixnum(int64_t x) {
    ls_value_t v;
    memset(&v, 0, sizeof v);
    v.tag = LS_T_FIXNUM;
    v.u.fixnum = x;
    return v;
}

ls_value_t ls_make_flonum(double x) {
    ls_value_t v;
    memset(&v, 0, sizeof v);
    v.tag = LS_T_FLONUM;
    v.u.flonum = x;
    return v;
}

ls_value_t ls_make_char(uint32_t cp) {
    ls_value_t v;
    memset(&v, 0, sizeof v);
    v.tag = LS_T_CHAR;
    v.u.character = cp;
    return v;
}

ls_value_t ls_wrap(uint16_t tag, void *p) {
    ls_value_t v;
    memset(&v, 0, sizeof v);
    v.tag = tag;
    v.u.ptr = p;
    return v;
}

int ls_is_nil    (ls_value_t v) { return v.tag == LS_T_NIL; }
int ls_is_t      (ls_value_t v) { return v.tag == LS_T_T; }
int ls_is_fixnum (ls_value_t v) { return v.tag == LS_T_FIXNUM; }
int ls_is_flonum (ls_value_t v) { return v.tag == LS_T_FLONUM; }
int ls_is_number (ls_value_t v) {
    return v.tag == LS_T_FIXNUM || v.tag == LS_T_FLONUM
        || v.tag == LS_T_RATIO  || v.tag == LS_T_BIGNUM
        || v.tag == LS_T_COMPLEX;
}
int ls_is_string (ls_value_t v) { return v.tag == LS_T_STRING; }
int ls_is_symbol (ls_value_t v) { return v.tag == LS_T_SYMBOL; }
int ls_is_cons   (ls_value_t v) { return v.tag == LS_T_CONS; }
int ls_is_list   (ls_value_t v) { return v.tag == LS_T_CONS || v.tag == LS_T_NIL; }
int ls_is_vector (ls_value_t v) { return v.tag == LS_T_VECTOR; }
int ls_is_hash   (ls_value_t v) { return v.tag == LS_T_HASHTABLE; }
int ls_is_fn     (ls_value_t v) {
    return v.tag == LS_T_FUNCTION || v.tag == LS_T_BYTECODE
        || v.tag == LS_T_NATIVE   || v.tag == LS_T_GENERIC;
}

int64_t ls_to_fixnum(ls_value_t v) {
    if (v.tag == LS_T_FIXNUM) return v.u.fixnum;
    if (v.tag == LS_T_FLONUM) return (int64_t)v.u.flonum;
    return 0;
}
double ls_to_flonum(ls_value_t v) {
    if (v.tag == LS_T_FLONUM) return v.u.flonum;
    if (v.tag == LS_T_FIXNUM) return (double)v.u.fixnum;
    return 0.0;
}
const char *ls_to_string(ls_value_t v, size_t *len_out) {
    if (v.tag == LS_T_STRING) {
        ls_string_t *s = (ls_string_t *)v.u.ptr;
        if (len_out) *len_out = s->len;
        return s->chars;
    }
    if (len_out) *len_out = 0;
    return "";
}

/* ---------- Heap object allocation ---------- */

ls_value_t ls_make_obj(ls_state_t *L, uint16_t tag, uint32_t size) {
    ls_obj_header_t *h = (ls_obj_header_t *)calloc(1, size);
    if (!h) {
        ls_error(L, "out of memory (allocating %u bytes, tag %u)", size, tag);
        return ls_nil_v();
    }
    h->tag = tag;
    h->size = size;
    h->flags = 0;
    h->next = L->gc.all_objects;
    L->gc.all_objects = h;
    L->gc.bytes_allocated += size;
    return ls_wrap(tag, h);
}

/* Typed accessors */
ls_cons_t       *ls_cons_p      (ls_value_t v) { return v.tag == LS_T_CONS      ? (ls_cons_t       *)v.u.ptr : NULL; }
ls_symbol_t     *ls_symbol_p    (ls_value_t v) { return v.tag == LS_T_SYMBOL    ? (ls_symbol_t     *)v.u.ptr : NULL; }
ls_string_t     *ls_string_p    (ls_value_t v) { return v.tag == LS_T_STRING    ? (ls_string_t     *)v.u.ptr : NULL; }
ls_vector_t     *ls_vector_p    (ls_value_t v) { return v.tag == LS_T_VECTOR    ? (ls_vector_t     *)v.u.ptr : NULL; }
ls_hashtable_t  *ls_hash_p      (ls_value_t v) { return v.tag == LS_T_HASHTABLE ? (ls_hashtable_t  *)v.u.ptr : NULL; }
ls_function_t   *ls_function_p  (ls_value_t v) { return v.tag == LS_T_FUNCTION  ? (ls_function_t   *)v.u.ptr : NULL; }
ls_bytecode_fn_t*ls_bytecode_p  (ls_value_t v) { return v.tag == LS_T_BYTECODE  ? (ls_bytecode_fn_t*)v.u.ptr : NULL; }
ls_native_t     *ls_native_p    (ls_value_t v) { return v.tag == LS_T_NATIVE    ? (ls_native_t     *)v.u.ptr : NULL; }
ls_package_t    *ls_package_p   (ls_value_t v) { return v.tag == LS_T_PACKAGE   ? (ls_package_t    *)v.u.ptr : NULL; }
ls_stream_t     *ls_stream_p    (ls_value_t v) { return v.tag == LS_T_STREAM    ? (ls_stream_t     *)v.u.ptr : NULL; }
ls_class_t      *ls_class_p     (ls_value_t v) { return v.tag == LS_T_CLASS     ? (ls_class_t      *)v.u.ptr : NULL; }
ls_instance_t   *ls_instance_p  (ls_value_t v) { return v.tag == LS_T_INSTANCE  ? (ls_instance_t   *)v.u.ptr : NULL; }

/* ---------- Strings ---------- */

ls_value_t ls_make_string(ls_state_t *L, const char *s, size_t n) {
    ls_value_t v = ls_make_obj(L, LS_T_STRING, sizeof(ls_string_t));
    ls_string_t *str = (ls_string_t *)v.u.ptr;
    str->len = n;
    str->cap = n + 1;
    str->chars = (char *)malloc(n + 1);
    if (!str->chars) { ls_error(L, "out of memory"); return ls_nil_v(); }
    if (s && n) memcpy(str->chars, s, n);
    str->chars[n] = 0;
    return v;
}

/* ---------- Cons cells ---------- */

ls_value_t ls_cons(ls_state_t *L, ls_value_t car, ls_value_t cdr) {
    ls_value_t v = ls_make_obj(L, LS_T_CONS, sizeof(ls_cons_t));
    ls_cons_t *c = (ls_cons_t *)v.u.ptr;
    c->car = car;
    c->cdr = cdr;
    return v;
}

ls_value_t ls_car(ls_value_t v) {
    if (v.tag == LS_T_NIL) return v;
    if (v.tag != LS_T_CONS) {
        ls_value_t z = { 0 };
        z.tag = LS_T_NIL;
        return z;
    }
    return ((ls_cons_t *)v.u.ptr)->car;
}
ls_value_t ls_cdr(ls_value_t v) {
    if (v.tag == LS_T_NIL) return v;
    if (v.tag != LS_T_CONS) {
        ls_value_t z = { 0 };
        z.tag = LS_T_NIL;
        return z;
    }
    return ((ls_cons_t *)v.u.ptr)->cdr;
}

size_t ls_list_length(ls_value_t list) {
    size_t n = 0;
    while (list.tag == LS_T_CONS) { n++; list = ((ls_cons_t *)list.u.ptr)->cdr; }
    return n;
}

ls_value_t ls_list_nth(ls_value_t list, size_t n) {
    while (n-- && list.tag == LS_T_CONS) list = ((ls_cons_t *)list.u.ptr)->cdr;
    if (list.tag != LS_T_CONS) return ls_nil_v();
    return ((ls_cons_t *)list.u.ptr)->car;
}

ls_value_t ls_list_reverse(ls_state_t *L, ls_value_t list) {
    ls_value_t acc = ls_nil_v();
    while (list.tag == LS_T_CONS) {
        acc  = ls_cons(L, ((ls_cons_t *)list.u.ptr)->car, acc);
        list = ((ls_cons_t *)list.u.ptr)->cdr;
    }
    return acc;
}

ls_value_t ls_list_append(ls_state_t *L, ls_value_t a, ls_value_t b) {
    if (a.tag == LS_T_NIL) return b;
    /* naive: reverse a, then cons onto b */
    ls_value_t ra = ls_list_reverse(L, a);
    ls_value_t out = b;
    while (ra.tag == LS_T_CONS) {
        out = ls_cons(L, ((ls_cons_t *)ra.u.ptr)->car, out);
        ra  = ((ls_cons_t *)ra.u.ptr)->cdr;
    }
    return out;
}

/* ---------- Vector ---------- */

ls_vector_t *ls_vec_new(ls_state_t *L, size_t cap, int adjustable) {
    ls_value_t v = ls_make_obj(L, LS_T_VECTOR, sizeof(ls_vector_t));
    ls_vector_t *vec = (ls_vector_t *)v.u.ptr;
    vec->len = 0;
    vec->cap = cap ? cap : 4;
    vec->fill_ptr = (size_t)-1;
    vec->adjustable = adjustable;
    vec->data = (ls_value_t *)calloc(vec->cap, sizeof(ls_value_t));
    if (!vec->data) { ls_error(L, "out of memory"); }
    return vec;
}

void ls_vec_push(ls_state_t *L, ls_vector_t *v, ls_value_t x) {
    (void)L;
    if (v->len == v->cap) {
        size_t ncap = v->cap * 2;
        ls_value_t *nd = (ls_value_t *)realloc(v->data, ncap * sizeof(ls_value_t));
        if (!nd) return;
        memset(nd + v->cap, 0, (ncap - v->cap) * sizeof(ls_value_t));
        v->data = nd;
        v->cap  = ncap;
    }
    v->data[v->len++] = x;
}

/* ---------- Hashtable (robin-hood) ---------- */

static void hash_grow(ls_state_t *L, ls_hashtable_t *h);

uint32_t ls_hash_string(const char *s, size_t n) {
    /* FNV-1a-ish */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

uint32_t ls_hash_value(ls_value_t v, ls_hash_test_t t) {
    uint32_t h;
    switch (v.tag) {
    case LS_T_NIL:      return 0xdeadbeefu;
    case LS_T_T:        return 0xfee1dead;
    case LS_T_FIXNUM:   {
        uint64_t x = (uint64_t)v.u.fixnum;
        x ^= (x >> 33); x *= 0xff51afd7ed558ccdULL;
        x ^= (x >> 33); x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= (x >> 33);
        return (uint32_t)x;
    }
    case LS_T_FLONUM:   {
        uint64_t x; memcpy(&x, &v.u.flonum, 8);
        x ^= (x >> 33); x *= 0xff51afd7ed558ccdULL; x ^= (x >> 33);
        return (uint32_t)x;
    }
    case LS_T_CHAR:     return v.u.character ^ 0xc0ffee;
    case LS_T_SYMBOL: {
        ls_symbol_t *s = (ls_symbol_t *)v.u.ptr;
        return s->hash;
    }
    case LS_T_STRING: {
        ls_string_t *s = (ls_string_t *)v.u.ptr;
        if (t == LS_HASH_EQUALP) {
            uint32_t hh = 2166136261u;
            for (size_t i = 0; i < s->len; i++) {
                hh ^= (uint8_t)tolower((unsigned char)s->chars[i]);
                hh *= 16777619u;
            }
            return hh ? hh : 1;
        }
        return ls_hash_string(s->chars, s->len);
    }
    case LS_T_CONS: {
        if (t == LS_HASH_EQUAL || t == LS_HASH_EQUALP) {
            /* hash a bounded number of elements */
            uint32_t hh = 0x9e3779b9u;
            int depth = 0;
            while (v.tag == LS_T_CONS && depth < 8) {
                hh ^= ls_hash_value(((ls_cons_t*)v.u.ptr)->car, t) + 0x9e3779b9u + (hh<<6) + (hh>>2);
                v = ((ls_cons_t*)v.u.ptr)->cdr;
                depth++;
            }
            return hh ? hh : 1;
        }
        /* fall through for EQ/EQL: hash pointer identity */
    }
    /* fall through */
    default: {
        uintptr_t p = (uintptr_t)v.u.ptr;
        h = (uint32_t)(p ^ (p >> 32));
        return h ? h : 1;
    }
    }
}

int ls_value_equal(ls_value_t a, ls_value_t b, ls_hash_test_t t) {
    if (a.tag != b.tag) {
        /* fixnum-flonum equality under EQL is false; under EQUAL also false.
         * But = (numeric comparison) is separate. */
        return 0;
    }
    switch (a.tag) {
    case LS_T_NIL: case LS_T_T: return 1;
    case LS_T_FIXNUM: return a.u.fixnum == b.u.fixnum;
    case LS_T_FLONUM: return a.u.flonum == b.u.flonum;
    case LS_T_CHAR:
        if (t == LS_HASH_EQUALP)
            return tolower((unsigned char)a.u.character) == tolower((unsigned char)b.u.character);
        return a.u.character == b.u.character;
    case LS_T_SYMBOL: return a.u.ptr == b.u.ptr;
    case LS_T_STRING: {
        ls_string_t *sa = (ls_string_t*)a.u.ptr, *sb = (ls_string_t*)b.u.ptr;
        if (t == LS_HASH_EQ || t == LS_HASH_EQL) return sa == sb;
        if (sa->len != sb->len) return 0;
        if (t == LS_HASH_EQUALP) {
            for (size_t i = 0; i < sa->len; i++)
                if (tolower((unsigned char)sa->chars[i]) != tolower((unsigned char)sb->chars[i]))
                    return 0;
            return 1;
        }
        return memcmp(sa->chars, sb->chars, sa->len) == 0;
    }
    case LS_T_CONS:
        if (t == LS_HASH_EQ || t == LS_HASH_EQL) return a.u.ptr == b.u.ptr;
        {
            ls_value_t va = a, vb = b;
            while (va.tag == LS_T_CONS && vb.tag == LS_T_CONS) {
                ls_cons_t *ca = (ls_cons_t*)va.u.ptr, *cb = (ls_cons_t*)vb.u.ptr;
                if (!ls_value_equal(ca->car, cb->car, t)) return 0;
                va = ca->cdr; vb = cb->cdr;
            }
            return ls_value_equal(va, vb, t);
        }
    case LS_T_VECTOR:
        if (t == LS_HASH_EQ || t == LS_HASH_EQL) return a.u.ptr == b.u.ptr;
        {
            ls_vector_t *va = (ls_vector_t*)a.u.ptr, *vb = (ls_vector_t*)b.u.ptr;
            if (va->len != vb->len) return 0;
            for (size_t i = 0; i < va->len; i++)
                if (!ls_value_equal(va->data[i], vb->data[i], t)) return 0;
            return 1;
        }
    default:
        return a.u.ptr == b.u.ptr;
    }
}

ls_hashtable_t *ls_hash_new(ls_state_t *L, ls_hash_test_t test, size_t cap_hint) {
    ls_value_t v = ls_make_obj(L, LS_T_HASHTABLE, sizeof(ls_hashtable_t));
    ls_hashtable_t *h = (ls_hashtable_t *)v.u.ptr;
    size_t cap = 16;
    while (cap < cap_hint * 2) cap *= 2;
    h->cap = cap;
    h->count = 0;
    h->test = test;
    h->entries = (ls_hash_entry_t *)calloc(cap, sizeof(ls_hash_entry_t));
    return h;
}

static void hash_insert_raw(ls_hashtable_t *h, uint32_t hash, ls_value_t k, ls_value_t v) {
    uint32_t mask = (uint32_t)(h->cap - 1);
    uint32_t idx = hash & mask;
    uint32_t dist = 0;
    ls_hash_entry_t e; e.hash = hash; e.dist = 0; e.key = k; e.val = v;
    for (;;) {
        if (h->entries[idx].hash == 0) {
            h->entries[idx] = e;
            h->entries[idx].dist = dist;
            h->count++;
            return;
        }
        if (h->entries[idx].dist < dist) {
            ls_hash_entry_t tmp = h->entries[idx];
            h->entries[idx] = e;
            h->entries[idx].dist = dist;
            e = tmp;
            dist = e.dist;
        }
        idx = (idx + 1) & mask;
        dist++;
    }
}

static void hash_grow(ls_state_t *L, ls_hashtable_t *h) {
    (void)L;
    size_t old_cap = h->cap;
    ls_hash_entry_t *old = h->entries;
    h->cap *= 2;
    h->entries = (ls_hash_entry_t *)calloc(h->cap, sizeof(ls_hash_entry_t));
    h->count = 0;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i].hash)
            hash_insert_raw(h, old[i].hash, old[i].key, old[i].val);
    free(old);
}

void ls_hash_put(ls_state_t *L, ls_hashtable_t *h, ls_value_t k, ls_value_t v) {
    if (h->count * 10 > h->cap * 9) hash_grow(L, h);
    uint32_t hash = ls_hash_value(k, h->test);
    if (hash == 0) hash = 1;
    /* first, look up to update in place */
    uint32_t mask = (uint32_t)(h->cap - 1);
    uint32_t idx = hash & mask;
    uint32_t dist = 0;
    for (;;) {
        if (h->entries[idx].hash == 0) break;
        if (h->entries[idx].hash == hash && ls_value_equal(h->entries[idx].key, k, h->test)) {
            h->entries[idx].val = v;
            return;
        }
        if (h->entries[idx].dist < dist) break;
        idx = (idx + 1) & mask; dist++;
    }
    hash_insert_raw(h, hash, k, v);
}

int ls_hash_get_sv(ls_hashtable_t *h, ls_value_t k, ls_value_t *out) {
    if (!h) return 0;
    uint32_t hash = ls_hash_value(k, h->test);
    if (hash == 0) hash = 1;
    uint32_t mask = (uint32_t)(h->cap - 1);
    uint32_t idx = hash & mask;
    uint32_t dist = 0;
    for (;;) {
        if (h->entries[idx].hash == 0) return 0;
        if (h->entries[idx].hash == hash && ls_value_equal(h->entries[idx].key, k, h->test)) {
            *out = h->entries[idx].val;
            return 1;
        }
        if (h->entries[idx].dist < dist) return 0;
        idx = (idx + 1) & mask; dist++;
    }
}

ls_value_t ls_hash_get(ls_hashtable_t *h, ls_value_t k) {
    ls_value_t v;
    if (ls_hash_get_sv(h, k, &v)) return v;
    return ls_nil_v();
}

int ls_hash_remove(ls_hashtable_t *h, ls_value_t k) {
    if (!h) return 0;
    uint32_t hash = ls_hash_value(k, h->test);
    if (hash == 0) hash = 1;
    uint32_t mask = (uint32_t)(h->cap - 1);
    uint32_t idx = hash & mask;
    uint32_t dist = 0;
    for (;;) {
        if (h->entries[idx].hash == 0) return 0;
        if (h->entries[idx].hash == hash && ls_value_equal(h->entries[idx].key, k, h->test)) {
            /* backward shift */
            uint32_t nidx = (idx + 1) & mask;
            while (h->entries[nidx].hash != 0 && h->entries[nidx].dist > 0) {
                h->entries[idx] = h->entries[nidx];
                h->entries[idx].dist--;
                idx = nidx;
                nidx = (nidx + 1) & mask;
            }
            memset(&h->entries[idx], 0, sizeof(ls_hash_entry_t));
            h->count--;
            return 1;
        }
        if (h->entries[idx].dist < dist) return 0;
        idx = (idx + 1) & mask; dist++;
    }
}

/* ---------- Symbol / package ---------- */

ls_package_t *ls_find_package(ls_state_t *L, const char *name, size_t namelen) {
    /* linear scan of package hashtable since pkg names are keys as strings */
    ls_value_t name_v = ls_make_string(L, name, namelen);
    ls_value_t pkg_v;
    if (ls_hash_get_sv(L->packages, name_v, &pkg_v)) return (ls_package_t *)pkg_v.u.ptr;
    return NULL;
}

ls_package_t *ls_ensure_package(ls_state_t *L, const char *name) {
    size_t n = strlen(name);
    ls_package_t *p = ls_find_package(L, name, n);
    if (p) return p;
    ls_value_t v = ls_make_obj(L, LS_T_PACKAGE, sizeof(ls_package_t));
    p = (ls_package_t *)v.u.ptr;
    p->name       = (ls_string_t *)ls_make_string(L, name, n).u.ptr;
    p->internal   = ls_hash_new(L, LS_HASH_EQUAL, 64);
    p->external   = ls_hash_new(L, LS_HASH_EQUAL, 16);
    p->use_list   = ls_nil_v();
    p->nicknames  = ls_nil_v();
    ls_hash_put(L, L->packages, ls_make_string(L, name, n), v);
    return p;
}

ls_value_t ls_intern_sym(ls_state_t *L, ls_package_t *pkg, const char *name, size_t namelen, int *is_external) {
    ls_value_t key = ls_make_string(L, name, namelen);
    ls_value_t out;
    if (ls_hash_get_sv(pkg->external, key, &out)) { if (is_external) *is_external = 1; return out; }
    if (ls_hash_get_sv(pkg->internal, key, &out)) { if (is_external) *is_external = 0; return out; }
    /* also search USE-list externally */
    ls_value_t uses = pkg->use_list;
    while (uses.tag == LS_T_CONS) {
        ls_package_t *up = (ls_package_t *)((ls_cons_t *)uses.u.ptr)->car.u.ptr;
        if (ls_hash_get_sv(up->external, key, &out)) { if (is_external) *is_external = 1; return out; }
        uses = ((ls_cons_t *)uses.u.ptr)->cdr;
    }
    /* create new internal symbol */
    ls_value_t sv = ls_make_obj(L, LS_T_SYMBOL, sizeof(ls_symbol_t));
    ls_symbol_t *s = (ls_symbol_t *)sv.u.ptr;
    s->name     = (ls_string_t *)key.u.ptr;
    s->package  = pkg;
    s->value    = ls_nil_v();
    s->function = ls_nil_v();
    s->plist    = ls_nil_v();
    s->sym_flags = 0;
    s->hash     = ls_hash_string(name, namelen);
    if (pkg == L->pkg_keyword) {
        s->sym_flags |= LS_SYM_KEYWORD | LS_SYM_CONSTANT;
        s->value      = sv;
        ls_hash_put(L, pkg->external, key, sv);
        if (is_external) *is_external = 1;
    } else {
        ls_hash_put(L, pkg->internal, key, sv);
        if (is_external) *is_external = 0;
    }
    return sv;
}

ls_value_t ls_intern(ls_state_t *L, const char *pkg, const char *name) {
    ls_package_t *p = pkg ? ls_ensure_package(L, pkg) : L->current_package;
    int ext;
    return ls_intern_sym(L, p, name, strlen(name), &ext);
}

void ls_export_symbol(ls_state_t *L, ls_package_t *pkg, ls_symbol_t *sym) {
    ls_value_t key = ls_make_string(L, sym->name->chars, sym->name->len);
    ls_value_t sv; sv.tag = LS_T_SYMBOL; sv.flags = 0; sv.u.ptr = sym;
    /* Remove from internal if present, add to external. */
    ls_hash_remove(pkg->internal, key);
    ls_hash_put(L, pkg->external, key, sv);
}

ls_value_t ls_make_symbol(ls_state_t *L, const char *name) {
    return ls_intern(L, NULL, name);
}

/* ---------- Varargs funcall ---------- */

ls_value_t ls_funcall(ls_state_t *L, ls_value_t fn, int nargs, ...) {
    ls_value_t args[32];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 32; i++)
        args[i] = va_arg(ap, ls_value_t);
    va_end(ap);
    return ls_apply(L, fn, nargs, args);
}

/* ---------- Error reporting ---------- */

void ls_error(ls_state_t *L, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(L->err_buf, sizeof L->err_buf, fmt, ap);
    va_end(ap);
    /* long-jump to outermost escape frame if any */
    if (L->esc_top) {
        L->esc_top->kind = 2;
        L->esc_top->value = ls_nil_v();
        longjmp(L->esc_top->buf, 1);
    }
    fprintf(stderr, "litesrpent: error: %s\n", L->err_buf);
}

void ls_type_error(ls_state_t *L, const char *expected, ls_value_t got) {
    (void)got;
    ls_error(L, "type error: expected %s", expected);
}
void ls_arity_error(ls_state_t *L, const char *name, int got, int min, int max) {
    ls_error(L, "arity error in %s: got %d, expected %d..%d", name, got, min, max);
}
void ls_undefined_function_error(ls_state_t *L, ls_symbol_t *s) {
    ls_error(L, "undefined function: %s", s && s->name ? s->name->chars : "?");
}
void ls_unbound_variable_error(ls_state_t *L, ls_symbol_t *s) {
    ls_error(L, "unbound variable: %s", s && s->name ? s->name->chars : "?");
}

const char *ls_last_error(ls_state_t *L) { return L->err_buf; }

int ls_signal(ls_state_t *L, const char *type, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(L->err_buf, sizeof L->err_buf, fmt, ap);
    va_end(ap);
    (void)type;
    if (L->esc_top) {
        L->esc_top->kind = 2;
        longjmp(L->esc_top->buf, 1);
    }
    return -1;
}

/* Global get/set convenience used by embedders. */
ls_value_t ls_getvar(ls_state_t *L, ls_value_t sym) {
    ls_symbol_t *s = ls_symbol_p(sym);
    if (!s) { ls_error(L, "getvar: not a symbol"); return ls_nil_v(); }
    if (!(s->sym_flags & LS_SYM_HAS_VALUE)) {
        ls_unbound_variable_error(L, s);
        return ls_nil_v();
    }
    return s->value;
}
void ls_setvar(ls_state_t *L, ls_value_t sym, ls_value_t val) {
    ls_symbol_t *s = ls_symbol_p(sym);
    if (!s) { ls_error(L, "setvar: not a symbol"); return; }
    s->value = val;
    s->sym_flags |= LS_SYM_HAS_VALUE;
}

void ls_defun(ls_state_t *L, const char *pkg, const char *name,
              ls_native_fn fn, int min_args, int max_args) {
    ls_value_t sv = ls_intern(L, pkg, name);
    ls_value_t nv = ls_make_obj(L, LS_T_NATIVE, sizeof(ls_native_t));
    ls_native_t *n = (ls_native_t *)nv.u.ptr;
    n->fn = fn;
    n->name = name;
    n->min_args = (int16_t)min_args;
    n->max_args = (int16_t)max_args;
    ls_symbol_t *s = (ls_symbol_t *)sv.u.ptr;
    s->function = nv;
    s->sym_flags |= LS_SYM_HAS_FN;
    /* Also export from the package. */
    if (s->package) {
        ls_value_t key = ls_wrap(LS_T_STRING, s->name);
        ls_hash_put(L, s->package->external, key, sv);
    }
}
