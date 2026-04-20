/* lsbuiltins.c -- Common Lisp builtin functions: arithmetic, cons,
 * sequence, string, hashtable, I/O, type predicates, control flow,
 * format, and system utilities.
 *
 * Each builtin is a static C function with signature:
 *   ls_value_t fn(ls_state_t *L, int nargs, ls_value_t *args);
 *
 * Registration happens in ls_register_builtins() called from ls_new(). */
#include "lscore.h"
#include "lseval.h"
#include <ctype.h>
#include <math.h>
#include <time.h>

/* ============================================================
 *  Helpers
 * ============================================================ */
#define A(i) a[i]
#define ARGN(name, cnt) if (n < cnt) { ls_arity_error(L, name, n, cnt, cnt); return ls_nil_v(); }

/* Parse common sequence keyword args :test, :test-not, :key starting from index `start`. */
typedef struct { ls_value_t test_fn; ls_value_t key_fn; int test_not; int from_end; int start; int end; } seqkw_t;
static void parse_seqkw(int n, ls_value_t *a, int start, seqkw_t *k) {
    k->test_fn = ls_nil_v(); k->key_fn = ls_nil_v();
    k->test_not = 0; k->from_end = 0; k->start = 0; k->end = -1;
    for (int i = start; i + 1 < n; i += 2) {
        ls_symbol_t *kw = ls_symbol_p(a[i]);
        if (!kw) continue;
        const char *kn = kw->name->chars;
        if      (!strcmp(kn,"TEST"))      k->test_fn  = a[i+1];
        else if (!strcmp(kn,"TEST-NOT"))  { k->test_fn = a[i+1]; k->test_not = 1; }
        else if (!strcmp(kn,"KEY"))       k->key_fn   = a[i+1];
        else if (!strcmp(kn,"FROM-END"))  k->from_end = (a[i+1].tag != LS_T_NIL);
        else if (!strcmp(kn,"START") && a[i+1].tag == LS_T_FIXNUM) k->start = (int)a[i+1].u.fixnum;
        else if (!strcmp(kn,"END")   && a[i+1].tag == LS_T_FIXNUM) k->end   = (int)a[i+1].u.fixnum;
    }
}
static int seqkw_match(ls_state_t *L, seqkw_t *k, ls_value_t item, ls_value_t elem) {
    ls_value_t probe = elem;
    if (k->key_fn.tag != LS_T_NIL) probe = ls_apply(L, k->key_fn, 1, &elem);
    int matched;
    if (k->test_fn.tag != LS_T_NIL) {
        ls_value_t args[2]; args[0] = item; args[1] = probe;
        ls_value_t r = ls_apply(L, k->test_fn, 2, args);
        matched = (r.tag != LS_T_NIL);
    } else {
        matched = ls_value_equal(item, probe, LS_HASH_EQL);
    }
    if (k->test_not) matched = !matched;
    return matched;
}
static double num_as_f(ls_value_t v) {
    if (v.tag == LS_T_FIXNUM) return (double)v.u.fixnum;
    if (v.tag == LS_T_FLONUM) return v.u.flonum;
    return 0.0;
}
static int is_num(ls_value_t v) { return v.tag == LS_T_FIXNUM || v.tag == LS_T_FLONUM; }

/* Promote: if both fixnums, stay fixnum; else float. */
static ls_value_t num_result(int64_t i, double f, int as_float) {
    return as_float ? ls_make_flonum(f) : ls_make_fixnum(i);
}

/* ============================================================
 *  ARITHMETIC
 * ============================================================ */
static ls_value_t bi_add(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    int has_float = 0; int64_t si = 0; double sf = 0.0;
    for (int i = 0; i < n; i++) {
        if (a[i].tag == LS_T_FLONUM) { has_float = 1; sf += a[i].u.flonum; }
        else if (a[i].tag == LS_T_FIXNUM) { si += a[i].u.fixnum; sf += (double)a[i].u.fixnum; }
    }
    return has_float ? ls_make_flonum(sf) : ls_make_fixnum(si);
}
static ls_value_t bi_sub(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n == 0) return ls_make_fixnum(0);
    int has_float = (a[0].tag == LS_T_FLONUM);
    int64_t si = a[0].tag == LS_T_FIXNUM ? a[0].u.fixnum : 0;
    double sf = num_as_f(a[0]);
    if (n == 1) return has_float ? ls_make_flonum(-sf) : ls_make_fixnum(-si);
    for (int i = 1; i < n; i++) {
        if (a[i].tag == LS_T_FLONUM) { has_float = 1; sf -= a[i].u.flonum; }
        else if (a[i].tag == LS_T_FIXNUM) { si -= a[i].u.fixnum; sf -= (double)a[i].u.fixnum; }
    }
    return has_float ? ls_make_flonum(sf) : ls_make_fixnum(si);
}
static ls_value_t bi_mul(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    int has_float = 0; int64_t si = 1; double sf = 1.0;
    for (int i = 0; i < n; i++) {
        if (a[i].tag == LS_T_FLONUM) { has_float = 1; sf *= a[i].u.flonum; }
        else if (a[i].tag == LS_T_FIXNUM) { si *= a[i].u.fixnum; sf *= (double)a[i].u.fixnum; }
    }
    return has_float ? ls_make_flonum(sf) : ls_make_fixnum(si);
}
static ls_value_t bi_div(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1) { ls_error(L, "/: need >= 1 arg"); return ls_nil_v(); }
    double v = num_as_f(a[0]);
    if (n == 1) { if (v == 0.0) { ls_error(L, "division by zero"); return ls_nil_v(); } return ls_make_flonum(1.0/v); }
    for (int i = 1; i < n; i++) {
        double d = num_as_f(a[i]);
        if (d == 0.0) { ls_error(L, "division by zero"); return ls_nil_v(); }
        v /= d;
    }
    if (a[0].tag == LS_T_FIXNUM) {
        /* Check if exact integer division */
        int all_int = 1;
        for (int i = 0; i < n; i++) if (a[i].tag != LS_T_FIXNUM) all_int = 0;
        if (all_int) {
            int64_t num = a[0].u.fixnum;
            for (int i = 1; i < n; i++) num /= a[i].u.fixnum;
            double check = num_as_f(a[0]);
            for (int i = 1; i < n; i++) check /= num_as_f(a[i]);
            if ((double)num == check) return ls_make_fixnum(num);
        }
    }
    return ls_make_flonum(v);
}
static ls_value_t bi_mod(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("mod", 2);
    if (a[1].tag == LS_T_FIXNUM && a[1].u.fixnum == 0) { ls_error(L, "mod by zero"); return ls_nil_v(); }
    if (a[0].tag == LS_T_FIXNUM && a[1].tag == LS_T_FIXNUM)
        return ls_make_fixnum(a[0].u.fixnum % a[1].u.fixnum);
    return ls_make_flonum(fmod(num_as_f(a[0]), num_as_f(a[1])));
}
static ls_value_t bi_rem(ls_state_t *L, int n, ls_value_t *a) { return bi_mod(L, n, a); }
static ls_value_t bi_abs(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("abs", 1);
    if (a[0].tag == LS_T_FIXNUM) return ls_make_fixnum(a[0].u.fixnum < 0 ? -a[0].u.fixnum : a[0].u.fixnum);
    return ls_make_flonum(fabs(num_as_f(a[0])));
}
/* helper: produce two values (quotient remainder) using L->mv */
static ls_value_t division_two_values(ls_state_t *L, int64_t q, double r_double, int int_inputs, int64_t r_int) {
    ls_value_t qv = ls_make_fixnum(q);
    ls_value_t rv = int_inputs ? ls_make_fixnum(r_int) : ls_make_flonum(r_double);
    L->mv.n = 2;
    L->mv.v[0] = qv;
    L->mv.v[1] = rv;
    return qv;
}
static ls_value_t bi_floor(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || n > 2) ls_error(L, "floor: 1 or 2 args required");
    double x = num_as_f(a[0]);
    double y = (n == 2) ? num_as_f(a[1]) : 1.0;
    if (y == 0.0) ls_error(L, "floor: division by zero");
    int int_in = (a[0].tag == LS_T_FIXNUM) && (n < 2 || a[1].tag == LS_T_FIXNUM);
    int64_t q = (int64_t)floor(x / y);
    if (int_in) {
        int64_t yi = (n==2) ? a[1].u.fixnum : 1;
        int64_t r = a[0].u.fixnum - q * yi;
        return division_two_values(L, q, 0, 1, r);
    }
    return division_two_values(L, q, x - q*y, 0, 0);
}
static ls_value_t bi_ceiling(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || n > 2) ls_error(L, "ceiling: 1 or 2 args required");
    double x = num_as_f(a[0]);
    double y = (n == 2) ? num_as_f(a[1]) : 1.0;
    if (y == 0.0) ls_error(L, "ceiling: division by zero");
    int int_in = (a[0].tag == LS_T_FIXNUM) && (n < 2 || a[1].tag == LS_T_FIXNUM);
    int64_t q = (int64_t)ceil(x / y);
    if (int_in) {
        int64_t yi = (n==2) ? a[1].u.fixnum : 1;
        int64_t r = a[0].u.fixnum - q * yi;
        return division_two_values(L, q, 0, 1, r);
    }
    return division_two_values(L, q, x - q*y, 0, 0);
}
static ls_value_t bi_round(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || n > 2) ls_error(L, "round: 1 or 2 args required");
    double x = num_as_f(a[0]);
    double y = (n == 2) ? num_as_f(a[1]) : 1.0;
    if (y == 0.0) ls_error(L, "round: division by zero");
    int int_in = (a[0].tag == LS_T_FIXNUM) && (n < 2 || a[1].tag == LS_T_FIXNUM);
    int64_t q = (int64_t)round(x / y);
    if (int_in) {
        int64_t yi = (n==2) ? a[1].u.fixnum : 1;
        int64_t r = a[0].u.fixnum - q * yi;
        return division_two_values(L, q, 0, 1, r);
    }
    return division_two_values(L, q, x - q*y, 0, 0);
}
static ls_value_t bi_truncate(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1 || n > 2) ls_error(L, "truncate: 1 or 2 args required");
    double x = num_as_f(a[0]);
    double y = (n == 2) ? num_as_f(a[1]) : 1.0;
    if (y == 0.0) ls_error(L, "truncate: division by zero");
    int int_in = (a[0].tag == LS_T_FIXNUM) && (n < 2 || a[1].tag == LS_T_FIXNUM);
    int64_t q = (int64_t)(x / y);  /* truncate toward zero */
    if (int_in) {
        int64_t yi = (n==2) ? a[1].u.fixnum : 1;
        int64_t r = a[0].u.fixnum - q * yi;
        return division_two_values(L, q, 0, 1, r);
    }
    return division_two_values(L, q, x - q*y, 0, 0);
}
static ls_value_t bi_max(ls_state_t *L, int n, ls_value_t *a) { (void)L; double m = num_as_f(a[0]); int fi = (a[0].tag==LS_T_FIXNUM); for(int i=1;i<n;i++){double v=num_as_f(a[i]); if(v>m){m=v;fi=(a[i].tag==LS_T_FIXNUM);}} return fi?ls_make_fixnum((int64_t)m):ls_make_flonum(m); }
static ls_value_t bi_min(ls_state_t *L, int n, ls_value_t *a) { (void)L; double m = num_as_f(a[0]); int fi = (a[0].tag==LS_T_FIXNUM); for(int i=1;i<n;i++){double v=num_as_f(a[i]); if(v<m){m=v;fi=(a[i].tag==LS_T_FIXNUM);}} return fi?ls_make_fixnum((int64_t)m):ls_make_flonum(m); }
static ls_value_t bi_expt(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("expt",2);
    /* Integer base + non-negative integer exponent => fixnum result */
    if (a[0].tag == LS_T_FIXNUM && a[1].tag == LS_T_FIXNUM && a[1].u.fixnum >= 0) {
        int64_t base = a[0].u.fixnum;
        int64_t exp = a[1].u.fixnum;
        int64_t r = 1;
        int overflow = 0;
        while (exp > 0) {
            if (exp & 1) {
                int64_t old = r;
                r *= base;
                if (base != 0 && r / base != old) { overflow = 1; break; }
            }
            exp >>= 1;
            if (exp > 0) {
                int64_t nb = base * base;
                if (base != 0 && nb / base != base) { overflow = 1; break; }
                base = nb;
            }
        }
        if (!overflow) return ls_make_fixnum(r);
    }
    return ls_make_flonum(pow(num_as_f(a[0]),num_as_f(a[1])));
}
static ls_value_t bi_sqrt(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("sqrt",1); return ls_make_flonum(sqrt(num_as_f(a[0]))); }
static ls_value_t bi_sin(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("sin",1); return ls_make_flonum(sin(num_as_f(a[0]))); }
static ls_value_t bi_cos(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("cos",1); return ls_make_flonum(cos(num_as_f(a[0]))); }
static ls_value_t bi_tan(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("tan",1); return ls_make_flonum(tan(num_as_f(a[0]))); }
static ls_value_t bi_asin(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("asin",1); return ls_make_flonum(asin(num_as_f(a[0]))); }
static ls_value_t bi_acos(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("acos",1); return ls_make_flonum(acos(num_as_f(a[0]))); }
static ls_value_t bi_atan(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(n>=2)return ls_make_flonum(atan2(num_as_f(a[0]),num_as_f(a[1]))); return ls_make_flonum(atan(num_as_f(a[0]))); }
static ls_value_t bi_log(ls_state_t *L, int n, ls_value_t *a) { (void)L; if(n>=2)return ls_make_flonum(log(num_as_f(a[0]))/log(num_as_f(a[1]))); return ls_make_flonum(log(num_as_f(a[0]))); }
static ls_value_t bi_exp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("exp",1); return ls_make_flonum(exp(num_as_f(a[0]))); }

/* Numeric comparisons */
static ls_value_t bi_numeq(ls_state_t *L, int n, ls_value_t *a) { (void)L; for(int i=1;i<n;i++) if(num_as_f(a[i-1])!=num_as_f(a[i])) return ls_nil_v(); return ls_t_v(); }
static ls_value_t bi_numlt(ls_state_t *L, int n, ls_value_t *a) { (void)L; for(int i=1;i<n;i++) if(!(num_as_f(a[i-1])<num_as_f(a[i]))) return ls_nil_v(); return ls_t_v(); }
static ls_value_t bi_numle(ls_state_t *L, int n, ls_value_t *a) { (void)L; for(int i=1;i<n;i++) if(!(num_as_f(a[i-1])<=num_as_f(a[i]))) return ls_nil_v(); return ls_t_v(); }
static ls_value_t bi_numgt(ls_state_t *L, int n, ls_value_t *a) { (void)L; for(int i=1;i<n;i++) if(!(num_as_f(a[i-1])>num_as_f(a[i]))) return ls_nil_v(); return ls_t_v(); }
static ls_value_t bi_numge(ls_state_t *L, int n, ls_value_t *a) { (void)L; for(int i=1;i<n;i++) if(!(num_as_f(a[i-1])>=num_as_f(a[i]))) return ls_nil_v(); return ls_t_v(); }
static ls_value_t bi_zerop(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("zerop",1); return num_as_f(a[0])==0.0 ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_plusp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("plusp",1); return num_as_f(a[0])>0.0 ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_minusp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("minusp",1); return num_as_f(a[0])<0.0 ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_numberp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("numberp",1); return is_num(a[0]) ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_integerp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("integerp",1); return a[0].tag==LS_T_FIXNUM ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_floatp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("floatp",1); return a[0].tag==LS_T_FLONUM ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_evenp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("evenp",1); return (a[0].tag==LS_T_FIXNUM && a[0].u.fixnum%2==0) ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_oddp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("oddp",1); return (a[0].tag==LS_T_FIXNUM && a[0].u.fixnum%2!=0) ? ls_t_v() : ls_nil_v(); }
static ls_value_t bi_1plus(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("1+",1); if(a[0].tag==LS_T_FIXNUM) return ls_make_fixnum(a[0].u.fixnum+1); return ls_make_flonum(num_as_f(a[0])+1.0); }
static ls_value_t bi_1minus(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("1-",1); if(a[0].tag==LS_T_FIXNUM) return ls_make_fixnum(a[0].u.fixnum-1); return ls_make_flonum(num_as_f(a[0])-1.0); }

/* Bitwise */
static ls_value_t bi_logand(ls_state_t *L, int n, ls_value_t *a) { (void)L; int64_t r = ~(int64_t)0; for(int i=0;i<n;i++) r &= a[i].u.fixnum; return ls_make_fixnum(r); }
static ls_value_t bi_logior(ls_state_t *L, int n, ls_value_t *a) { (void)L; int64_t r = 0; for(int i=0;i<n;i++) r |= a[i].u.fixnum; return ls_make_fixnum(r); }
static ls_value_t bi_logxor(ls_state_t *L, int n, ls_value_t *a) { (void)L; int64_t r = 0; for(int i=0;i<n;i++) r ^= a[i].u.fixnum; return ls_make_fixnum(r); }
static ls_value_t bi_lognot(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("lognot",1); return ls_make_fixnum(~a[0].u.fixnum); }
static ls_value_t bi_ash(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("ash",2); int64_t v=a[0].u.fixnum; int64_t s=a[1].u.fixnum; return ls_make_fixnum(s>=0?(v<<s):(v>>(-s))); }

/* ============================================================
 *  CONS / LIST
 * ============================================================ */
static ls_value_t bi_cons(ls_state_t *L, int n, ls_value_t *a) { ARGN("cons",2); return ls_cons(L, a[0], a[1]); }
static ls_value_t bi_car(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("car",1); return ls_car(a[0]); }
static ls_value_t bi_cdr(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("cdr",1); return ls_cdr(a[0]); }
static ls_value_t bi_caar(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("caar",1); return ls_car(ls_car(a[0])); }
static ls_value_t bi_cadr(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("cadr",1); return ls_car(ls_cdr(a[0])); }
static ls_value_t bi_cddr(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("cddr",1); return ls_cdr(ls_cdr(a[0])); }
static ls_value_t bi_cdar(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("cdar",1); return ls_cdr(ls_car(a[0])); }
static ls_value_t bi_caddr(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("caddr",1); return ls_car(ls_cdr(ls_cdr(a[0]))); }
static ls_value_t bi_rplaca(ls_state_t *L, int n, ls_value_t *a) { ARGN("rplaca",2); if(a[0].tag!=LS_T_CONS) { ls_type_error(L,"cons",a[0]); return ls_nil_v(); } ((ls_cons_t*)a[0].u.ptr)->car = a[1]; return a[0]; }
static ls_value_t bi_rplacd(ls_state_t *L, int n, ls_value_t *a) { ARGN("rplacd",2); if(a[0].tag!=LS_T_CONS) { ls_type_error(L,"cons",a[0]); return ls_nil_v(); } ((ls_cons_t*)a[0].u.ptr)->cdr = a[1]; return a[0]; }
static ls_value_t bi_list(ls_state_t *L, int n, ls_value_t *a) {
    ls_value_t r = ls_nil_v();
    for (int i = n-1; i >= 0; i--) r = ls_cons(L, a[i], r);
    return r;
}
static ls_value_t bi_liststar(ls_state_t *L, int n, ls_value_t *a) {
    if (n == 0) return ls_nil_v();
    ls_value_t r = a[n-1];
    for (int i = n-2; i >= 0; i--) r = ls_cons(L, a[i], r);
    return r;
}
static ls_value_t bi_length(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("length",1);
    if (a[0].tag == LS_T_NIL) return ls_make_fixnum(0);
    if (a[0].tag == LS_T_CONS) return ls_make_fixnum((int64_t)ls_list_length(a[0]));
    if (a[0].tag == LS_T_STRING) return ls_make_fixnum((int64_t)((ls_string_t*)a[0].u.ptr)->len);
    if (a[0].tag == LS_T_VECTOR) return ls_make_fixnum((int64_t)((ls_vector_t*)a[0].u.ptr)->len);
    return ls_make_fixnum(0);
}
static ls_value_t bi_append(ls_state_t *L, int n, ls_value_t *a) {
    ls_value_t r = ls_nil_v();
    for (int i = n-1; i >= 0; i--) r = ls_list_append(L, a[i], r);
    return r;
}
static ls_value_t bi_reverse(ls_state_t *L, int n, ls_value_t *a) { ARGN("reverse",1); return ls_list_reverse(L, a[0]); }
static ls_value_t bi_nth(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("nth",2); return ls_list_nth(a[1], (size_t)a[0].u.fixnum); }
static ls_value_t bi_nthcdr(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("nthcdr",2); ls_value_t v = a[1];
    for (int64_t i = 0; i < a[0].u.fixnum && v.tag == LS_T_CONS; i++) v = ls_cdr(v);
    return v;
}
static ls_value_t bi_last(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("last",1); ls_value_t v = a[0];
    while (v.tag == LS_T_CONS && ls_cdr(v).tag == LS_T_CONS) v = ls_cdr(v);
    return v;
}
static ls_value_t bi_copy_list(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("copy-list",1);
    ls_value_t h = ls_nil_v(), t = ls_nil_v();
    ls_value_t v = a[0];
    while (v.tag == LS_T_CONS) {
        ls_value_t c = ls_cons(L, ls_car(v), ls_nil_v());
        if (h.tag == LS_T_NIL) h = t = c;
        else { ((ls_cons_t*)t.u.ptr)->cdr = c; t = c; }
        v = ls_cdr(v);
    }
    if (t.tag == LS_T_CONS && v.tag != LS_T_NIL) ((ls_cons_t*)t.u.ptr)->cdr = v;
    return h;
}

static ls_value_t bi_member(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("member",2);
    /* Parse keyword args: :test FN, :key FN, :test-not FN */
    ls_value_t test_fn = ls_nil_v();
    ls_value_t key_fn  = ls_nil_v();
    int test_not = 0;
    for (int i = 2; i + 1 < n; i += 2) {
        ls_symbol_t *kw = ls_symbol_p(a[i]);
        if (!kw) continue;
        const char *kn = kw->name->chars;
        if (strcmp(kn, "TEST") == 0) test_fn = a[i+1];
        else if (strcmp(kn, "KEY") == 0) key_fn = a[i+1];
        else if (strcmp(kn, "TEST-NOT") == 0) { test_fn = a[i+1]; test_not = 1; }
    }
    ls_value_t v = a[1];
    while (v.tag == LS_T_CONS) {
        ls_value_t elem = ls_car(v);
        ls_value_t probe = elem;
        if (key_fn.tag != LS_T_NIL) probe = ls_apply(L, key_fn, 1, &elem);
        int matched;
        if (test_fn.tag != LS_T_NIL) {
            ls_value_t args2[2]; args2[0] = a[0]; args2[1] = probe;
            ls_value_t r = ls_apply(L, test_fn, 2, args2);
            matched = (r.tag != LS_T_NIL);
        } else {
            matched = ls_value_equal(a[0], probe, LS_HASH_EQL);
        }
        if (test_not) matched = !matched;
        if (matched) return v;
        v = ls_cdr(v);
    }
    return ls_nil_v();
}
static ls_value_t bi_assoc(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("assoc",2);
    seqkw_t k; parse_seqkw(n, a, 2, &k);
    ls_value_t v = a[1];
    while (v.tag == LS_T_CONS) {
        ls_value_t entry = ls_car(v);
        if (entry.tag == LS_T_CONS && seqkw_match(L, &k, a[0], ls_car(entry))) return entry;
        v = ls_cdr(v);
    }
    return ls_nil_v();
}
static ls_value_t bi_mapcar(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("mapcar",2);
    ls_value_t fn = a[0], list = a[1];
    ls_value_t h = ls_nil_v(), t = ls_nil_v();
    while (list.tag == LS_T_CONS) {
        ls_value_t x = ls_car(list);
        ls_value_t r = ls_apply(L, fn, 1, &x);
        ls_value_t c = ls_cons(L, r, ls_nil_v());
        if (h.tag == LS_T_NIL) h = t = c;
        else { ((ls_cons_t*)t.u.ptr)->cdr = c; t = c; }
        list = ls_cdr(list);
    }
    return h;
}
static ls_value_t bi_mapc(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("mapc",2);
    ls_value_t fn = a[0], list = a[1], orig = list;
    while (list.tag == LS_T_CONS) {
        ls_value_t x = ls_car(list);
        ls_apply(L, fn, 1, &x);
        list = ls_cdr(list);
    }
    return orig;
}
static ls_value_t bi_reduce(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) ls_error(L, "reduce: too few arguments");
    ls_value_t fn = a[0], list = a[1];
    /* Parse keyword args: :initial-value, :from-end, :key, :start, :end */
    int has_initial = 0;
    ls_value_t initial = ls_nil_v();
    ls_value_t key_fn = ls_nil_v();
    int from_end = 0;
    int kstart = 0, kend = -1;
    for (int i = 2; i + 1 < n; i += 2) {
        if (a[i].tag != LS_T_SYMBOL) continue;
        ls_symbol_t *kw = (ls_symbol_t *)a[i].u.ptr;
        const char *nm = kw->name->chars;
        if      (!strcmp(nm,"INITIAL-VALUE")) { initial = a[i+1]; has_initial = 1; }
        else if (!strcmp(nm,"FROM-END"))      from_end = (a[i+1].tag != LS_T_NIL);
        else if (!strcmp(nm,"KEY"))           key_fn = a[i+1];
        else if (!strcmp(nm,"START")) { if (a[i+1].tag == LS_T_FIXNUM) kstart = (int)a[i+1].u.fixnum; }
        else if (!strcmp(nm,"END"))   { if (a[i+1].tag == LS_T_FIXNUM) kend = (int)a[i+1].u.fixnum; }
    }
    /* Convert sequence to a fresh array for [start,end) slice */
    int total = 0;
    if (list.tag == LS_T_VECTOR) {
        ls_vector_t *v = (ls_vector_t *)list.u.ptr;
        total = (int)v->len;
    } else {
        ls_value_t p = list;
        while (p.tag == LS_T_CONS) { total++; p = ls_cdr(p); }
    }
    if (kend < 0 || kend > total) kend = total;
    if (kstart < 0) kstart = 0;
    int slen = kend - kstart;
    if (slen < 0) slen = 0;
    ls_value_t *items = NULL;
    if (slen > 0) {
        items = (ls_value_t *)malloc(sizeof(ls_value_t) * (size_t)slen);
        if (!items) ls_error(L, "reduce: out of memory");
    }
    if (list.tag == LS_T_VECTOR) {
        ls_vector_t *v = (ls_vector_t *)list.u.ptr;
        for (int i = 0; i < slen; i++) items[i] = v->data[kstart + i];
    } else {
        ls_value_t p = list;
        for (int i = 0; i < kstart && p.tag == LS_T_CONS; i++) p = ls_cdr(p);
        for (int i = 0; i < slen && p.tag == LS_T_CONS; i++) {
            items[i] = ls_car(p); p = ls_cdr(p);
        }
    }
    /* Apply :key */
    if (key_fn.tag != LS_T_NIL) {
        for (int i = 0; i < slen; i++)
            items[i] = ls_apply(L, key_fn, 1, &items[i]);
    }
    /* Empty sequence */
    if (slen == 0) {
        ls_value_t r = has_initial ? initial : ls_apply(L, fn, 0, NULL);
        if (items) free(items);
        return r;
    }
    ls_value_t acc;
    if (from_end) {
        if (has_initial) {
            acc = initial;
            for (int i = slen - 1; i >= 0; i--) {
                ls_value_t x[2]; x[0] = items[i]; x[1] = acc;
                acc = ls_apply(L, fn, 2, x);
            }
        } else {
            acc = items[slen - 1];
            for (int i = slen - 2; i >= 0; i--) {
                ls_value_t x[2]; x[0] = items[i]; x[1] = acc;
                acc = ls_apply(L, fn, 2, x);
            }
        }
    } else {
        int start_i;
        if (has_initial) { acc = initial; start_i = 0; }
        else             { acc = items[0]; start_i = 1; }
        for (int i = start_i; i < slen; i++) {
            ls_value_t x[2]; x[0] = acc; x[1] = items[i];
            acc = ls_apply(L, fn, 2, x);
        }
    }
    if (items) free(items);
    return acc;
}
static ls_value_t bi_remove_if(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("remove-if",2);
    ls_value_t fn = a[0], list = a[1];
    ls_value_t h = ls_nil_v(), t = ls_nil_v();
    while (list.tag == LS_T_CONS) {
        ls_value_t x = ls_car(list);
        ls_value_t r = ls_apply(L, fn, 1, &x);
        if (r.tag == LS_T_NIL) {
            ls_value_t c = ls_cons(L, x, ls_nil_v());
            if (h.tag == LS_T_NIL) h = t = c;
            else { ((ls_cons_t*)t.u.ptr)->cdr = c; t = c; }
        }
        list = ls_cdr(list);
    }
    return h;
}
static ls_value_t bi_remove_if_not(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("remove-if-not",2);
    ls_value_t fn = a[0], list = a[1];
    ls_value_t h = ls_nil_v(), t = ls_nil_v();
    while (list.tag == LS_T_CONS) {
        ls_value_t x = ls_car(list);
        ls_value_t r = ls_apply(L, fn, 1, &x);
        if (r.tag != LS_T_NIL) {
            ls_value_t c = ls_cons(L, x, ls_nil_v());
            if (h.tag == LS_T_NIL) h = t = c;
            else { ((ls_cons_t*)t.u.ptr)->cdr = c; t = c; }
        }
        list = ls_cdr(list);
    }
    return h;
}
static ls_value_t bi_some(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("some",2);
    ls_value_t fn = a[0], list = a[1];
    while (list.tag == LS_T_CONS) { ls_value_t x = ls_car(list); ls_value_t r = ls_apply(L, fn, 1, &x); if (r.tag != LS_T_NIL) return r; list = ls_cdr(list); }
    return ls_nil_v();
}
static ls_value_t bi_every(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("every",2);
    ls_value_t fn = a[0], list = a[1]; ls_value_t r = ls_t_v();
    while (list.tag == LS_T_CONS) { ls_value_t x = ls_car(list); r = ls_apply(L, fn, 1, &x); if (r.tag == LS_T_NIL) return r; list = ls_cdr(list); }
    return r;
}

/* ============================================================
 *  TYPE PREDICATES
 * ============================================================ */
static ls_value_t bi_consp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("consp",1); return a[0].tag==LS_T_CONS?ls_t_v():ls_nil_v(); }
static ls_value_t bi_listp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("listp",1); return (a[0].tag==LS_T_CONS||a[0].tag==LS_T_NIL)?ls_t_v():ls_nil_v(); }
static ls_value_t bi_atom(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("atom",1); return a[0].tag!=LS_T_CONS?ls_t_v():ls_nil_v(); }
static ls_value_t bi_null(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("null",1); return a[0].tag==LS_T_NIL?ls_t_v():ls_nil_v(); }
static ls_value_t bi_symbolp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("symbolp",1); return a[0].tag==LS_T_SYMBOL?ls_t_v():ls_nil_v(); }
static ls_value_t bi_stringp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("stringp",1); return a[0].tag==LS_T_STRING?ls_t_v():ls_nil_v(); }
static ls_value_t bi_vectorp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("vectorp",1); return a[0].tag==LS_T_VECTOR?ls_t_v():ls_nil_v(); }
static ls_value_t bi_functionp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("functionp",1); return ls_is_fn(a[0])?ls_t_v():ls_nil_v(); }
static ls_value_t bi_characterp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("characterp",1); return a[0].tag==LS_T_CHAR?ls_t_v():ls_nil_v(); }
static ls_value_t bi_keywordp(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("keywordp",1); return (a[0].tag==LS_T_SYMBOL && ((ls_symbol_t*)a[0].u.ptr)->sym_flags & LS_SYM_KEYWORD)?ls_t_v():ls_nil_v(); }
static ls_value_t bi_hash_table_p(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("hash-table-p",1); return a[0].tag==LS_T_HASHTABLE?ls_t_v():ls_nil_v(); }

/* EQ / EQL / EQUAL / EQUALP */
static ls_value_t bi_eq(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("eq",2); return ls_value_equal(a[0],a[1],LS_HASH_EQ)?ls_t_v():ls_nil_v(); }
static ls_value_t bi_eql(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("eql",2); return ls_value_equal(a[0],a[1],LS_HASH_EQL)?ls_t_v():ls_nil_v(); }
static ls_value_t bi_equal(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("equal",2); return ls_value_equal(a[0],a[1],LS_HASH_EQUAL)?ls_t_v():ls_nil_v(); }
static ls_value_t bi_equalp(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("equalp",2); return ls_value_equal(a[0],a[1],LS_HASH_EQUALP)?ls_t_v():ls_nil_v(); }
static ls_value_t bi_not(ls_state_t *L,int n,ls_value_t *a) { (void)L; ARGN("not",1); return a[0].tag==LS_T_NIL?ls_t_v():ls_nil_v(); }

/* ============================================================
 *  STRINGS
 * ============================================================ */
static ls_value_t bi_string(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("string",1);
    if (a[0].tag == LS_T_STRING) return a[0];
    if (a[0].tag == LS_T_SYMBOL) { ls_symbol_t *s = (ls_symbol_t*)a[0].u.ptr; return ls_wrap(LS_T_STRING, s->name); }
    if (a[0].tag == LS_T_CHAR) { char c = (char)a[0].u.character; return ls_make_string(L, &c, 1); }
    return ls_make_string(L, "", 0);
}
static ls_value_t bi_string_upcase(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("string-upcase",1);
    ls_string_t *s = ls_string_p(a[0]); if(!s) { ls_type_error(L,"string",a[0]); return ls_nil_v(); }
    ls_value_t r = ls_make_string(L, s->chars, s->len);
    ls_string_t *rs = (ls_string_t*)r.u.ptr;
    for(size_t i=0;i<rs->len;i++) rs->chars[i]=(char)toupper((unsigned char)rs->chars[i]);
    return r;
}
static ls_value_t bi_string_downcase(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("string-downcase",1);
    ls_string_t *s = ls_string_p(a[0]); if(!s) { ls_type_error(L,"string",a[0]); return ls_nil_v(); }
    ls_value_t r = ls_make_string(L, s->chars, s->len);
    ls_string_t *rs = (ls_string_t*)r.u.ptr;
    for(size_t i=0;i<rs->len;i++) rs->chars[i]=(char)tolower((unsigned char)rs->chars[i]);
    return r;
}
static ls_value_t bi_string_equal(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("string=",2);
    ls_string_t *sa=ls_string_p(a[0]), *sb=ls_string_p(a[1]);
    if(!sa||!sb) return ls_nil_v();
    return (sa->len==sb->len && memcmp(sa->chars,sb->chars,sa->len)==0)?ls_t_v():ls_nil_v();
}
static ls_value_t bi_string_concat(ls_state_t *L, int n, ls_value_t *a) {
    size_t total = 0;
    for(int i=0;i<n;i++) { ls_string_t *s=ls_string_p(a[i]); if(s) total+=s->len; }
    char *buf = (char*)malloc(total+1);
    size_t off = 0;
    for(int i=0;i<n;i++) { ls_string_t *s=ls_string_p(a[i]); if(s){memcpy(buf+off,s->chars,s->len); off+=s->len;} }
    buf[off]=0;
    ls_value_t r = ls_make_string(L, buf, total);
    free(buf);
    return r;
}
static ls_value_t bi_subseq(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("subseq",2);
    if (a[0].tag == LS_T_STRING) {
        ls_string_t *s = (ls_string_t*)a[0].u.ptr;
        int64_t start = a[1].u.fixnum;
        int64_t end = n >= 3 ? a[2].u.fixnum : (int64_t)s->len;
        if (start < 0) start = 0;
        if (end > (int64_t)s->len) end = (int64_t)s->len;
        if (end < start) end = start;
        return ls_make_string(L, s->chars + start, (size_t)(end - start));
    }
    if (a[0].tag == LS_T_VECTOR) {
        ls_vector_t *v = (ls_vector_t*)a[0].u.ptr;
        int64_t start = a[1].u.fixnum;
        int64_t end = n >= 3 ? a[2].u.fixnum : (int64_t)v->len;
        ls_vector_t *nv = ls_vec_new(L, (size_t)(end-start), 0);
        for (int64_t i = start; i < end; i++) ls_vec_push(L, nv, v->data[i]);
        return ls_wrap(LS_T_VECTOR, nv);
    }
    return ls_nil_v();
}
static ls_value_t bi_char_code(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("char-code",1); return ls_make_fixnum(a[0].u.character); }
static ls_value_t bi_code_char(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("code-char",1); return ls_make_char((uint32_t)a[0].u.fixnum); }
static ls_value_t bi_char_int(ls_state_t *L, int n, ls_value_t *a) { return bi_char_code(L,n,a); }
static ls_value_t bi_string_length(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("string-length",1); ls_string_t *s=ls_string_p(a[0]); return ls_make_fixnum(s?s->len:0); }
static ls_value_t bi_char_at(ls_state_t *L, int n, ls_value_t *a) { (void)L; ARGN("char",2); ls_string_t *s=ls_string_p(a[0]); if(!s) return ls_nil_v(); size_t i=(size_t)a[1].u.fixnum; return i<s->len?ls_make_char((unsigned char)s->chars[i]):ls_nil_v(); }
static ls_value_t bi_parse_integer(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("parse-integer",1);
    ls_string_t *s = ls_string_p(a[0]); if (!s) return ls_nil_v();
    char *end; long long val = strtoll(s->chars, &end, 10);
    return ls_make_fixnum(val);
}

/* ============================================================
 *  VECTORS / ARRAYS
 * ============================================================ */
static ls_value_t bi_make_array(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("make-array",1);
    size_t sz = (a[0].tag == LS_T_FIXNUM) ? (size_t)a[0].u.fixnum : 0;
    ls_vector_t *v = ls_vec_new(L, sz, 1);
    v->len = sz;
    return ls_wrap(LS_T_VECTOR, v);
}
static ls_value_t bi_vector(ls_state_t *L, int n, ls_value_t *a) {
    ls_vector_t *v = ls_vec_new(L, n, 0);
    for(int i=0;i<n;i++) ls_vec_push(L, v, a[i]);
    return ls_wrap(LS_T_VECTOR, v);
}
static ls_value_t bi_aref(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("aref",2);
    ls_vector_t *v = ls_vector_p(a[0]); if(!v) { ls_type_error(L,"vector",a[0]); return ls_nil_v(); }
    size_t idx = (size_t)a[1].u.fixnum;
    if (idx >= v->len) { ls_error(L,"aref: index %zu out of bounds (len=%zu)", idx, v->len); return ls_nil_v(); }
    return v->data[idx];
}
static ls_value_t bi_aset(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("(setf aref)",3);
    ls_vector_t *v = ls_vector_p(a[0]); if(!v) { ls_type_error(L,"vector",a[0]); return ls_nil_v(); }
    size_t idx = (size_t)a[1].u.fixnum;
    if (idx >= v->len) { ls_error(L,"setf aref out of bounds"); return ls_nil_v(); }
    v->data[idx] = a[2];
    return a[2];
}
static ls_value_t bi_vector_push(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("vector-push-extend",2);
    ls_vector_t *v = ls_vector_p(a[1]); if(!v) { ls_type_error(L,"vector",a[1]); return ls_nil_v(); }
    ls_vec_push(L, v, a[0]);
    return ls_make_fixnum((int64_t)v->len - 1);
}

/* ============================================================
 *  HASH TABLES
 * ============================================================ */
static ls_value_t bi_make_hash_table(ls_state_t *L, int n, ls_value_t *a) {
    (void)a;
    /* TODO: parse :test keyword */
    ls_hashtable_t *h = ls_hash_new(L, LS_HASH_EQUAL, 16);
    return ls_wrap(LS_T_HASHTABLE, h);
}
static ls_value_t bi_gethash(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("gethash",2);
    ls_hashtable_t *h = ls_hash_p(a[1]); if(!h) { ls_type_error(L,"hash-table",a[1]); return ls_nil_v(); }
    ls_value_t out;
    if (ls_hash_get_sv(h, a[0], &out)) {
        L->mv.n = 2; L->mv.v[0] = out; L->mv.v[1] = ls_t_v();
        return out;
    }
    ls_value_t def = n >= 3 ? a[2] : ls_nil_v();
    L->mv.n = 2; L->mv.v[0] = def; L->mv.v[1] = ls_nil_v();
    return def;
}
static ls_value_t bi_puthash(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("(setf gethash)",3);
    ls_hashtable_t *h = ls_hash_p(a[1]); if(!h) { ls_type_error(L,"hash-table",a[1]); return ls_nil_v(); }
    ls_hash_put(L, h, a[0], a[2]);
    return a[2];
}
static ls_value_t bi_remhash(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("remhash",2);
    ls_hashtable_t *h = ls_hash_p(a[1]); if(!h) return ls_nil_v();
    return ls_hash_remove(h, a[0]) ? ls_t_v() : ls_nil_v();
}
static ls_value_t bi_hash_table_count(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("hash-table-count",1);
    ls_hashtable_t *h = ls_hash_p(a[0]); return ls_make_fixnum(h ? (int64_t)h->count : 0);
}
static ls_value_t bi_maphash(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("maphash",2);
    ls_value_t fn = a[0];
    ls_hashtable_t *h = ls_hash_p(a[1]); if(!h) return ls_nil_v();
    for (size_t i = 0; i < h->cap; i++) {
        if (h->entries[i].hash) {
            ls_value_t xa[2]; xa[0] = h->entries[i].key; xa[1] = h->entries[i].val;
            ls_apply(L, fn, 2, xa);
        }
    }
    return ls_nil_v();
}

/* ============================================================
 *  I/O
 * ============================================================ */
static ls_value_t bi_print(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("print",1);
    ls_print_value(L, a[0], L->stdout_, 1);
    fputc('\n', stdout); fflush(stdout);
    return a[0];
}
static ls_value_t bi_prin1(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("prin1",1);
    ls_print_value(L, a[0], L->stdout_, 1);
    fflush(stdout);
    return a[0];
}
static ls_value_t bi_princ(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("princ",1);
    ls_print_value(L, a[0], L->stdout_, 0);
    fflush(stdout);
    return a[0];
}
static ls_value_t bi_terpri(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n; fputc('\n', stdout); fflush(stdout);
    return ls_nil_v();
}
static ls_value_t bi_fresh_line(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n;
    /* For interactive terminals we don't track column; emit a newline. */
    fputc('\n', stdout); fflush(stdout);
    return ls_nil_v();
}
static ls_value_t bi_force_output(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n; fflush(stdout); return ls_nil_v();
}
static ls_value_t bi_finish_output(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n; fflush(stdout); return ls_nil_v();
}
static ls_value_t bi_clear_input(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n;
    /* Best-effort: read until newline or EOF without blocking the terminal. */
    return ls_nil_v();
}
static ls_value_t bi_clear_output(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n; return ls_nil_v();
}
static ls_value_t bi_write_char(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("write-char",1);
    fputc((int)a[0].u.character, stdout); fflush(stdout);
    return a[0];
}
static ls_value_t bi_read_char(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n;
    int c = fgetc(stdin);
    if (c == EOF) return ls_nil_v();
    return ls_make_char((uint32_t)c);
}
static ls_value_t bi_read_line(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n;
    char buf[4096]; size_t pos = 0;
    int c;
    while ((c = fgetc(stdin)) != EOF && c != '\n' && pos < sizeof(buf)-1)
        buf[pos++] = (char)c;
    if (pos == 0 && c == EOF) return ls_nil_v();
    return ls_make_string(L, buf, pos);
}
/* Read one Lisp form from the given source.  source==NULL means stdin;
 * otherwise it's a string-stream's buffer to consume from. */
static int read_one_char(ls_stream_t *s) {
    if (s) {
        if (s->bufpos >= s->buflen) return EOF;
        return (unsigned char)s->buffer[s->bufpos++];
    }
    return fgetc(stdin);
}

static ls_value_t bi_read(ls_state_t *L, int n, ls_value_t *a) {
    ls_stream_t *src = NULL;
    if (n >= 1 && a[0].tag == LS_T_STREAM) {
        src = (ls_stream_t *)a[0].u.ptr;
        if (!src->string_stream) src = NULL;  /* fall back to stdin for fp streams */
    }
    char buf[4096]; size_t pos = 0;
    int paren_depth = 0, in_string = 0, saw_nonws = 0;
    for (;;) {
        int c = read_one_char(src);
        if (c == EOF) break;
        buf[pos++] = (char)c;
        if (!in_string) {
            if (c == '"') in_string = 1;
            if (c == '(') { paren_depth++; saw_nonws = 1; }
            if (c == ')') { paren_depth--; if (paren_depth <= 0 && saw_nonws) break; }
            if (!isspace(c)) saw_nonws = 1;
            if (paren_depth == 0 && saw_nonws && (isspace(c) || c == ')')) break;
        } else {
            if (c == '"') in_string = 0;
            if (c == '\\') { c = read_one_char(src); if (c!=EOF) buf[pos++]=(char)c; }
        }
        if (pos >= sizeof(buf)-2) break;
    }
    return ls_read_from_string(L, buf, NULL);
}
static ls_value_t bi_load(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("load",1);
    ls_string_t *s = ls_string_p(a[0]); if(!s) { ls_type_error(L,"string",a[0]); return ls_nil_v(); }
    ls_value_t out;
    int r = ls_eval_file(L, s->chars, &out);
    return r == 0 ? ls_t_v() : ls_nil_v();
}

/* ============================================================
 *  FORMAT (simplified)
 * ============================================================ */
/* Write a buffer to a destination stream value: T -> stdout, NIL -> ignored,
 * STREAM -> append/printf.  Returns 1 if dst is a string stream (caller may
 * want to handle differently). */
static void write_to_dest(ls_value_t dest, const char *buf, size_t n) {
    if (dest.tag == LS_T_STREAM) {
        ls_stream_t *s = (ls_stream_t *)dest.u.ptr;
        if (s->string_stream) {
            if (s->bufpos + n + 1 > s->bufcap) {
                size_t nc = s->bufcap ? s->bufcap * 2 : 64;
                while (nc < s->bufpos + n + 1) nc *= 2;
                s->buffer = (char *)realloc(s->buffer, nc);
                s->bufcap = nc;
            }
            memcpy(s->buffer + s->bufpos, buf, n);
            s->bufpos += n;
            if (s->bufpos > s->buflen) s->buflen = s->bufpos;
            s->buffer[s->bufpos] = 0;
        } else if (s->fp) {
            fwrite(buf, 1, n, s->fp); fflush(s->fp);
        }
        return;
    }
    /* T or any non-nil/non-stream -> stdout */
    if (dest.tag != LS_T_NIL) { fwrite(buf, 1, n, stdout); fflush(stdout); }
}

static ls_value_t bi_format(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("format",2);
    int to_string = (a[0].tag == LS_T_NIL);
    int to_stream = (a[0].tag == LS_T_STREAM);
    ls_string_t *fmt = ls_string_p(a[1]);
    if (!fmt) { ls_type_error(L,"string",a[1]); return ls_nil_v(); }
    char out[4096]; size_t op = 0;
    int ai = 2;
    for (size_t fi = 0; fi < fmt->len && op < sizeof(out)-16; fi++) {
        char c = fmt->chars[fi];
        if (c == '~' && fi + 1 < fmt->len) {
            fi++;
            char d = fmt->chars[fi];
            switch (d) {
            case 'A': case 'a':
                if (ai < n) {
                    if (a[ai].tag == LS_T_STRING) {
                        ls_string_t *s = (ls_string_t*)a[ai].u.ptr;
                        size_t copy = s->len < sizeof(out)-op-1 ? s->len : sizeof(out)-op-1;
                        memcpy(out+op, s->chars, copy); op += copy;
                    } else {
                        ls_stream_t tmp; memset(&tmp,0,sizeof tmp);
                        tmp.string_stream = 1;
                        ls_print_value(L, a[ai], &tmp, 0);
                        if (tmp.buffer) {
                            size_t copy = tmp.buflen < sizeof(out)-op-1 ? tmp.buflen : sizeof(out)-op-1;
                            memcpy(out+op, tmp.buffer, copy); op += copy;
                            free(tmp.buffer);
                        }
                    }
                    ai++;
                }
                break;
            case 'S': case 's':
                if (ai < n) {
                    ls_stream_t tmp; memset(&tmp,0,sizeof tmp);
                    tmp.string_stream = 1;
                    ls_print_value(L, a[ai], &tmp, 1);
                    if (tmp.buffer) {
                        size_t copy = tmp.buflen < sizeof(out)-op-1 ? tmp.buflen : sizeof(out)-op-1;
                        memcpy(out+op, tmp.buffer, copy); op += copy;
                        free(tmp.buffer);
                    }
                    ai++;
                }
                break;
            case 'D': case 'd':
                if (ai < n && (a[ai].tag == LS_T_FIXNUM || a[ai].tag == LS_T_FLONUM)) {
                    int wrote = snprintf(out+op, sizeof(out)-op, "%lld", (long long)(a[ai].tag == LS_T_FIXNUM ? a[ai].u.fixnum : (int64_t)a[ai].u.flonum));
                    op += wrote; ai++;
                }
                break;
            case 'F': case 'f':
                if (ai < n) {
                    int wrote = snprintf(out+op, sizeof(out)-op, "%f", num_as_f(a[ai]));
                    op += wrote; ai++;
                }
                break;
            case 'X': case 'x':
                if (ai < n) { int w = snprintf(out+op,sizeof(out)-op,"%llx",(long long)a[ai].u.fixnum); op+=w; ai++; }
                break;
            case 'B': case 'b':
                if (ai < n) {
                    uint64_t v = (uint64_t)a[ai].u.fixnum; ai++;
                    if (v == 0) out[op++] = '0';
                    else { char tmp2[65]; int k=0; while(v){tmp2[k++]='0'+(v&1);v>>=1;} for(int j=k-1;j>=0;j--) out[op++]=tmp2[j]; }
                }
                break;
            case '%': out[op++] = '\n'; break;
            case '&':
                /* fresh-line: emit newline only if last output char isn't already \n */
                if (op == 0 || out[op-1] != '\n') out[op++] = '\n';
                break;
            case '~': out[op++] = '~'; break;
            case 'T': case 't':
                out[op++] = '\t'; break;
            case 'C': case 'c':
                if (ai < n && a[ai].tag == LS_T_CHAR) { out[op++] = (char)a[ai].u.character; ai++; }
                break;
            case '{': {
                /* find matching ~} */
                size_t body_start = fi + 1;
                int depth = 1;
                size_t scan = body_start;
                while (scan < fmt->len && depth > 0) {
                    if (fmt->chars[scan] == '~' && scan + 1 < fmt->len) {
                        if (fmt->chars[scan+1] == '{') depth++;
                        else if (fmt->chars[scan+1] == '}') depth--;
                        scan += 2;
                    } else scan++;
                }
                if (depth != 0) { /* unbalanced */ break; }
                size_t body_end = scan - 2;  /* position of '~' before '}' */
                size_t body_len = body_end - body_start;
                /* Get the list arg */
                if (ai < n) {
                    ls_value_t lst = a[ai]; ai++;
                    /* iterate; reapply body for each pass that consumes args */
                    while (lst.tag == LS_T_CONS) {
                        /* sub-format using body_start..body_len with successive items */
                        ls_value_t sub_args[16]; int sai = 0;
                        ls_value_t cur = lst;
                        /* count tildes that consume args in body to know how many to consume per pass */
                        /* Simple approach: iterate body once per item, popping args as they're used */
                        for (size_t bi = 0; bi < body_len && op < sizeof(out)-16; bi++) {
                            char bc = fmt->chars[body_start + bi];
                            if (bc == '~' && bi + 1 < body_len) {
                                bi++;
                                char bd = fmt->chars[body_start + bi];
                                if ((bd == 'A' || bd == 'a' || bd == 'S' || bd == 's' ||
                                     bd == 'D' || bd == 'd' || bd == 'F' || bd == 'f' ||
                                     bd == 'X' || bd == 'x' || bd == 'B' || bd == 'b' ||
                                     bd == 'C' || bd == 'c') && cur.tag == LS_T_CONS) {
                                    sub_args[sai++] = ls_car(cur);
                                    cur = ls_cdr(cur);
                                    /* render */
                                    ls_value_t v = sub_args[sai-1];
                                    if (bd == '%') { out[op++] = '\n'; continue; }
                                    char tmpbuf[256];
                                    if (bd == 'D' || bd == 'd') {
                                        int w = snprintf(tmpbuf, sizeof tmpbuf, "%lld",
                                            (long long)(v.tag == LS_T_FIXNUM ? v.u.fixnum : (int64_t)num_as_f(v)));
                                        for (int k=0; k<w && op<sizeof(out)-1; k++) out[op++] = tmpbuf[k];
                                    } else if (bd == 'F' || bd == 'f') {
                                        int w = snprintf(tmpbuf, sizeof tmpbuf, "%f", num_as_f(v));
                                        for (int k=0; k<w && op<sizeof(out)-1; k++) out[op++] = tmpbuf[k];
                                    } else if (bd == 'X' || bd == 'x') {
                                        int w = snprintf(tmpbuf, sizeof tmpbuf, "%llx", (long long)v.u.fixnum);
                                        for (int k=0; k<w && op<sizeof(out)-1; k++) out[op++] = tmpbuf[k];
                                    } else if (bd == 'C' || bd == 'c') {
                                        if (v.tag == LS_T_CHAR) out[op++] = (char)v.u.character;
                                    } else {
                                        ls_stream_t ts; memset(&ts,0,sizeof ts); ts.string_stream=1;
                                        ls_print_value(L, v, &ts, (bd == 'S' || bd == 's'));
                                        if (ts.buffer) {
                                            size_t copy = ts.buflen < sizeof(out)-op-1 ? ts.buflen : sizeof(out)-op-1;
                                            memcpy(out+op, ts.buffer, copy); op += copy; free(ts.buffer);
                                        }
                                    }
                                } else if (bd == '%') { out[op++] = '\n'; }
                                else if (bd == '&') { if (op==0||out[op-1]!='\n') out[op++]='\n'; }
                                else if (bd == '~') { out[op++] = '~'; }
                                else if (bd == 'T' || bd == 't') { out[op++] = '\t'; }
                                else if (bd == '^') {
                                    /* Terminate iteration if no more items remain. */
                                    if (cur.tag != LS_T_CONS) {
                                        /* Mark outer loop done by emptying lst,
                                         * then break inner body loop. */
                                        lst = ls_nil_v();
                                        goto post_iter;
                                    }
                                }
                                else { out[op++] = '~'; out[op++] = bd; }
                            } else {
                                out[op++] = bc;
                            }
                        }
                        /* advance lst by however many items the body consumed.
                         * Above: each body iteration starts with a fresh cur=lst,
                         * the items consumed land in sub_args[0..sai-1], and we should
                         * advance lst past those. */
                        for (int k = 0; k < sai && lst.tag == LS_T_CONS; k++) lst = ls_cdr(lst);
                        if (sai == 0) break;  /* no items consumed -> avoid infinite loop */
                        continue;
                    post_iter: break;
                    }
                }
                fi = body_end + 1;  /* skip past '}' */
                break;
            }
            default: out[op++] = '~'; out[op++] = d; break;
            }
        } else {
            out[op++] = c;
        }
    }
    out[op] = 0;
    if (to_string) return ls_make_string(L, out, op);
    if (to_stream) { write_to_dest(a[0], out, op); return ls_nil_v(); }
    fputs(out, stdout); fflush(stdout);
    return ls_nil_v();
}

/* ============================================================
 *  SYMBOL / PACKAGE ops
 * ============================================================ */
static ls_value_t bi_symbol_name(ls_state_t *L,int n,ls_value_t *a) { ARGN("symbol-name",1); ls_symbol_t *s=ls_symbol_p(a[0]); if(!s){ ls_type_error(L,"symbol",a[0]); return ls_nil_v(); } return ls_wrap(LS_T_STRING,s->name); }
static ls_value_t bi_symbol_package(ls_state_t *L,int n,ls_value_t *a) { ARGN("symbol-package",1); ls_symbol_t *s=ls_symbol_p(a[0]); if(!s) return ls_nil_v(); return s->package?ls_wrap(LS_T_PACKAGE,s->package):ls_nil_v(); }
static ls_value_t bi_symbol_value(ls_state_t *L,int n,ls_value_t *a) { ARGN("symbol-value",1); ls_symbol_t *s=ls_symbol_p(a[0]); if(!s) return ls_nil_v(); return s->value; }
static ls_value_t bi_symbol_function(ls_state_t *L,int n,ls_value_t *a) { ARGN("symbol-function",1); ls_symbol_t *s=ls_symbol_p(a[0]); if(!s) return ls_nil_v(); return s->function; }
static ls_value_t bi_intern(ls_state_t *L,int n,ls_value_t *a) {
    ARGN("intern",1);
    ls_string_t *s = ls_string_p(a[0]); if(!s) return ls_nil_v();
    ls_package_t *pkg = n >= 2 ? ls_package_p(a[1]) : L->current_package;
    return ls_intern_sym(L, pkg, s->chars, s->len, NULL);
}
static ls_value_t bi_find_package(ls_state_t *L,int n,ls_value_t *a) {
    ARGN("find-package",1);
    ls_string_t *s = ls_string_p(a[0]); if(!s) return ls_nil_v();
    ls_package_t *p = ls_find_package(L, s->chars, s->len);
    return p ? ls_wrap(LS_T_PACKAGE, p) : ls_nil_v();
}
static ls_value_t bi_export(ls_state_t *L,int n,ls_value_t *a) {
    ARGN("export",1);
    ls_symbol_t *s = ls_symbol_p(a[0]); if(!s) return ls_nil_v();
    ls_package_t *pkg = s->package ? s->package : L->current_package;
    ls_hash_put(L, pkg->external, ls_wrap(LS_T_STRING, s->name), a[0]);
    return ls_t_v();
}

/* ============================================================
 *  VALUES / MULTIPLE-VALUE
 * ============================================================ */
static ls_value_t bi_values(ls_state_t *L, int n, ls_value_t *a) {
    L->mv.n = n;
    for (int i = 0; i < n && i < LS_MV_MAX; i++) L->mv.v[i] = a[i];
    return n > 0 ? a[0] : ls_nil_v();
}

/* ============================================================
 *  FUNCALL / APPLY / EVAL
 * ============================================================ */
static ls_value_t bi_funcall(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("funcall",1);
    return ls_apply(L, a[0], n-1, a+1);
}
static ls_value_t bi_apply_builtin(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("apply",2);
    ls_value_t fn = a[0];
    /* collect args: if n==2, last is list; if n>2, spread leading then append list */
    ls_value_t all[64]; int c = 0;
    for (int i = 1; i < n-1 && c < 63; i++) all[c++] = a[i];
    ls_value_t last = a[n-1];
    while (last.tag == LS_T_CONS && c < 63) { all[c++] = ls_car(last); last = ls_cdr(last); }
    return ls_apply(L, fn, c, all);
}
static ls_value_t bi_eval(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("eval",1);
    return ls_eval(L, a[0], L->genv);
}

/* ============================================================
 *  SYSTEM / MISC
 * ============================================================ */
static ls_value_t bi_type_of(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("type-of",1);
    const char *names[] = {
        "NULL","BOOLEAN","FIXNUM","FLOAT","CHARACTER","SYMBOL","STRING",
        "CONS","VECTOR","HASH-TABLE","FUNCTION","COMPILED-FUNCTION",
        "FUNCTION","MACRO","SPECIAL","PACKAGE","STREAM",
        "FOREIGN-POINTER","FOREIGN-LIB","FOREIGN-FN",
        "STANDARD-CLASS","STANDARD-OBJECT","GENERIC-FUNCTION","METHOD",
        "CONDITION","RESTART","PATHNAME","RATIO","BIGNUM","COMPLEX",
        "READTABLE","RANDOM-STATE","STRUCTURE","ARRAY"
    };
    uint32_t t = a[0].tag;
    const char *nm = (t < sizeof(names)/sizeof(names[0])) ? names[t] : "T";
    return ls_intern(L, "COMMON-LISP", nm);
}

/* TYPEP: (typep object type-spec). Recognizes common type specifiers. */
static int type_match(ls_state_t *L, ls_value_t obj, const char *tname) {
    if (!tname) return 0;
    if (strcmp(tname,"T") == 0) return 1;
    if (strcmp(tname,"NIL") == 0) return 0;
    if (strcmp(tname,"NULL") == 0) return obj.tag == LS_T_NIL;
    if (strcmp(tname,"BOOLEAN") == 0) return obj.tag == LS_T_NIL || obj.tag == LS_T_T;
    if (strcmp(tname,"INTEGER") == 0 || strcmp(tname,"FIXNUM") == 0) return obj.tag == LS_T_FIXNUM;
    if (strcmp(tname,"FLOAT") == 0 || strcmp(tname,"DOUBLE-FLOAT") == 0 ||
        strcmp(tname,"SINGLE-FLOAT") == 0 || strcmp(tname,"REAL") == 0)
        return obj.tag == LS_T_FLONUM || obj.tag == LS_T_FIXNUM;
    if (strcmp(tname,"NUMBER") == 0)
        return obj.tag == LS_T_FIXNUM || obj.tag == LS_T_FLONUM;
    if (strcmp(tname,"RATIONAL") == 0) return obj.tag == LS_T_FIXNUM;
    if (strcmp(tname,"CHARACTER") == 0) return obj.tag == LS_T_CHAR;
    if (strcmp(tname,"SYMBOL") == 0) return obj.tag == LS_T_SYMBOL || obj.tag == LS_T_NIL;
    if (strcmp(tname,"KEYWORD") == 0) {
        if (obj.tag != LS_T_SYMBOL) return 0;
        ls_symbol_t *s = (ls_symbol_t*)obj.u.ptr;
        return s->package && s->package->name &&
               strcmp(s->package->name->chars, "KEYWORD") == 0;
    }
    if (strcmp(tname,"STRING") == 0 || strcmp(tname,"SIMPLE-STRING") == 0 ||
        strcmp(tname,"BASE-STRING") == 0)
        return obj.tag == LS_T_STRING;
    if (strcmp(tname,"CONS") == 0) return obj.tag == LS_T_CONS;
    if (strcmp(tname,"LIST") == 0) return obj.tag == LS_T_CONS || obj.tag == LS_T_NIL;
    if (strcmp(tname,"ATOM") == 0) return obj.tag != LS_T_CONS;
    if (strcmp(tname,"VECTOR") == 0 || strcmp(tname,"SIMPLE-VECTOR") == 0)
        return obj.tag == LS_T_VECTOR;
    if (strcmp(tname,"ARRAY") == 0)
        return obj.tag == LS_T_VECTOR || obj.tag == LS_T_ARRAY || obj.tag == LS_T_STRING;
    if (strcmp(tname,"SEQUENCE") == 0)
        return obj.tag == LS_T_CONS || obj.tag == LS_T_NIL ||
               obj.tag == LS_T_VECTOR || obj.tag == LS_T_STRING;
    if (strcmp(tname,"HASH-TABLE") == 0) return obj.tag == LS_T_HASHTABLE;
    if (strcmp(tname,"FUNCTION") == 0)
        return obj.tag == LS_T_FUNCTION || obj.tag == LS_T_NATIVE ||
               obj.tag == LS_T_BYTECODE;
    if (strcmp(tname,"PACKAGE") == 0) return obj.tag == LS_T_PACKAGE;
    if (strcmp(tname,"STREAM") == 0) return obj.tag == LS_T_STREAM;
    if (strcmp(tname,"STANDARD-OBJECT") == 0) return obj.tag == LS_T_INSTANCE;
    if (strcmp(tname,"STANDARD-CLASS") == 0) return obj.tag == LS_T_CLASS;
    if (strcmp(tname,"CONDITION") == 0) return obj.tag == LS_T_INSTANCE;
    /* If the type is a class name, defer to a class-of comparison via type-of. */
    (void)L;
    return 0;
}

static ls_value_t bi_typep(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) ls_error(L, "typep: 2 args required");
    ls_value_t obj = a[0], spec = a[1];
    /* Atomic type spec: a symbol */
    if (spec.tag == LS_T_SYMBOL) {
        const char *nm = ((ls_symbol_t*)spec.u.ptr)->name->chars;
        return type_match(L, obj, nm) ? ls_t_v() : ls_nil_v();
    }
    /* Compound: (or T1 T2...), (and T1 T2...), (not T), (member ...), (eql x), (satisfies fn) */
    if (spec.tag == LS_T_CONS) {
        ls_value_t hd = ls_car(spec);
        ls_value_t rest = ls_cdr(spec);
        if (hd.tag != LS_T_SYMBOL) return ls_nil_v();
        const char *op = ((ls_symbol_t*)hd.u.ptr)->name->chars;
        if (strcmp(op,"OR") == 0) {
            while (rest.tag == LS_T_CONS) {
                ls_value_t sub_args[2] = { obj, ls_car(rest) };
                if (bi_typep(L, 2, sub_args).tag == LS_T_T) return ls_t_v();
                rest = ls_cdr(rest);
            }
            return ls_nil_v();
        }
        if (strcmp(op,"AND") == 0) {
            while (rest.tag == LS_T_CONS) {
                ls_value_t sub_args[2] = { obj, ls_car(rest) };
                if (bi_typep(L, 2, sub_args).tag != LS_T_T) return ls_nil_v();
                rest = ls_cdr(rest);
            }
            return ls_t_v();
        }
        if (strcmp(op,"NOT") == 0) {
            ls_value_t sub_args[2] = { obj, ls_car(rest) };
            return (bi_typep(L, 2, sub_args).tag == LS_T_T) ? ls_nil_v() : ls_t_v();
        }
        if (strcmp(op,"MEMBER") == 0) {
            while (rest.tag == LS_T_CONS) {
                ls_value_t v = ls_car(rest);
                if (ls_value_equal(obj, v, LS_HASH_EQL)) return ls_t_v();
                rest = ls_cdr(rest);
            }
            return ls_nil_v();
        }
        if (strcmp(op,"EQL") == 0) {
            return ls_value_equal(obj, ls_car(rest), LS_HASH_EQL) ? ls_t_v() : ls_nil_v();
        }
        if (strcmp(op,"SATISFIES") == 0) {
            ls_value_t fn = ls_car(rest);
            ls_value_t fnv;
            if (fn.tag == LS_T_SYMBOL) {
                ls_symbol_t *s = (ls_symbol_t*)fn.u.ptr;
                if (!(s->sym_flags & LS_SYM_HAS_FN)) return ls_nil_v();
                fnv = s->function;
            } else fnv = fn;
            ls_value_t res = ls_apply(L, fnv, 1, &obj);
            return (res.tag != LS_T_NIL && res.tag != LS_T_T) ? ls_t_v() :
                   (res.tag == LS_T_T) ? ls_t_v() : ls_nil_v();
        }
        /* (integer LOW HIGH) -- ignore bounds, just check integer */
        if (strcmp(op,"INTEGER") == 0 || strcmp(op,"FIXNUM") == 0)
            return obj.tag == LS_T_FIXNUM ? ls_t_v() : ls_nil_v();
        if (strcmp(op,"FLOAT") == 0 || strcmp(op,"REAL") == 0)
            return (obj.tag == LS_T_FLONUM || obj.tag == LS_T_FIXNUM) ?
                   ls_t_v() : ls_nil_v();
    }
    return ls_nil_v();
}

static ls_value_t bi_subtypep(ls_state_t *L, int n, ls_value_t *a) {
    /* Conservative: returns (values nil nil) -- "don't know" */
    (void)L; (void)a; (void)n;
    L->mv.n = 2; L->mv.v[0] = ls_nil_v(); L->mv.v[1] = ls_nil_v();
    return ls_nil_v();
}

/* String streams. */
static ls_value_t bi_make_string_output_stream(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n;
    ls_value_t v = ls_make_obj(L, LS_T_STREAM, sizeof(ls_stream_t));
    ls_stream_t *s = (ls_stream_t *)v.u.ptr;
    s->direction = 2;
    s->string_stream = 1;
    s->ungot_char = -1;
    s->bufcap = 256;
    s->buffer = (char *)malloc(s->bufcap);
    s->buflen = 0;
    return v;
}
static ls_value_t bi_make_string_input_stream(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1) ls_error(L, "make-string-input-stream: needs string");
    ls_string_t *str = ls_string_p(a[0]);
    if (!str) { ls_type_error(L,"string",a[0]); return ls_nil_v(); }
    ls_value_t v = ls_make_obj(L, LS_T_STREAM, sizeof(ls_stream_t));
    ls_stream_t *s = (ls_stream_t *)v.u.ptr;
    s->direction = 1;
    s->string_stream = 1;
    s->ungot_char = -1;
    s->bufcap = str->len + 1;
    s->buffer = (char *)malloc(s->bufcap);
    memcpy(s->buffer, str->chars, str->len);
    s->buffer[str->len] = 0;
    s->buflen = str->len;
    s->bufpos = 0;
    return v;
}
static ls_value_t bi_get_output_stream_string(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1) ls_error(L, "get-output-stream-string: needs stream");
    ls_stream_t *s = ls_stream_p(a[0]);
    if (!s || !s->string_stream) { ls_error(L,"not a string output stream"); return ls_nil_v(); }
    ls_value_t r = ls_make_string(L, s->buffer ? s->buffer : "", s->buflen);
    s->buflen = 0;  /* reset */
    return r;
}

static ls_value_t bi_char_name(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("char-name",1);
    if (a[0].tag != LS_T_CHAR) { ls_type_error(L,"character",a[0]); return ls_nil_v(); }
    uint32_t c = a[0].u.character;
    const char *nm = NULL;
    switch (c) {
    case ' ':  nm = "Space"; break;
    case '\n': nm = "Newline"; break;
    case '\t': nm = "Tab"; break;
    case '\r': nm = "Return"; break;
    case '\b': nm = "Backspace"; break;
    case '\f': nm = "Page"; break;
    case 0:    nm = "Null"; break;
    case 127:  nm = "Rubout"; break;
    case 27:   nm = "Escape"; break;
    }
    if (nm) return ls_make_string(L, nm, strlen(nm));
    return ls_nil_v();
}
static ls_value_t bi_gensym(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n;
    static int counter = 0;
    char buf[32]; snprintf(buf, sizeof buf, "G%d", counter++);
    ls_value_t sv = ls_make_obj(L, LS_T_SYMBOL, sizeof(ls_symbol_t));
    ls_symbol_t *s = (ls_symbol_t*)sv.u.ptr;
    s->name = (ls_string_t*)ls_make_string(L, buf, strlen(buf)).u.ptr;
    s->hash = ls_hash_string(buf, strlen(buf));
    return sv;
}
static ls_value_t bi_error(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("error",1);
    if (a[0].tag == LS_T_STRING) ls_error(L, "%s", ((ls_string_t*)a[0].u.ptr)->chars);
    else ls_error(L, "error signaled");
    return ls_nil_v();
}
static ls_value_t bi_get_internal_real_time(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n;
    return ls_make_fixnum((int64_t)(clock() * 1000 / CLOCKS_PER_SEC));
}
static ls_value_t bi_get_universal_time(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)a; (void)n;
    return ls_make_fixnum((int64_t)time(NULL) + 2208988800LL);
}
static ls_value_t bi_room(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n;
    printf("Litesrpent memory: %zu bytes allocated\n", L->gc.bytes_allocated);
    return ls_nil_v();
}
static ls_value_t bi_gc(ls_state_t *L, int n, ls_value_t *a) {
    (void)a; (void)n;
    ls_gc_collect(L);
    return ls_nil_v();
}
static ls_value_t bi_quit(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    int code = n >= 1 && a[0].tag == LS_T_FIXNUM ? (int)a[0].u.fixnum : 0;
    exit(code);
    return ls_nil_v();
}

/* Coerce */
static ls_value_t bi_coerce(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("coerce",2);
    /* simplified: (coerce x 'float) (coerce x 'string) (coerce x 'list) (coerce x 'vector) */
    if (a[1].tag == LS_T_SYMBOL) {
        ls_symbol_t *ts = (ls_symbol_t*)a[1].u.ptr;
        if (strcmp(ts->name->chars, "FLOAT") == 0 || strcmp(ts->name->chars, "SINGLE-FLOAT") == 0 || strcmp(ts->name->chars, "DOUBLE-FLOAT") == 0)
            return ls_make_flonum(num_as_f(a[0]));
        if (strcmp(ts->name->chars, "LIST") == 0) {
            if (a[0].tag == LS_T_VECTOR) {
                ls_vector_t *v = (ls_vector_t*)a[0].u.ptr;
                ls_value_t r = ls_nil_v();
                for (size_t i = v->len; i-- > 0;) r = ls_cons(L, v->data[i], r);
                return r;
            }
            if (a[0].tag == LS_T_STRING) {
                ls_string_t *s = (ls_string_t*)a[0].u.ptr;
                ls_value_t r = ls_nil_v();
                for (size_t i = s->len; i-- > 0;) r = ls_cons(L, ls_make_char((unsigned char)s->chars[i]), r);
                return r;
            }
        }
        if (strcmp(ts->name->chars, "STRING") == 0) {
            return bi_string(L, 1, a);
        }
    }
    return a[0];
}

/* sort */
static ls_state_t *sort_L; static ls_value_t sort_fn;
static int sort_cmp(const void *x, const void *y) {
    ls_value_t xa[2]; xa[0] = *(const ls_value_t*)x; xa[1] = *(const ls_value_t*)y;
    ls_value_t r = ls_apply(sort_L, sort_fn, 2, xa);
    return (r.tag != LS_T_NIL) ? -1 : 1;
}
static ls_value_t bi_sort(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("sort",2);
    if (a[0].tag == LS_T_VECTOR) {
        ls_vector_t *v = (ls_vector_t*)a[0].u.ptr;
        sort_L = L; sort_fn = a[1];
        qsort(v->data, v->len, sizeof(ls_value_t), sort_cmp);
        return a[0];
    }
    if (a[0].tag == LS_T_CONS || a[0].tag == LS_T_NIL) {
        size_t len = ls_list_length(a[0]);
        ls_value_t *arr = (ls_value_t*)malloc(len * sizeof(ls_value_t));
        ls_value_t cur = a[0]; for(size_t i=0;i<len;i++){arr[i]=ls_car(cur);cur=ls_cdr(cur);}
        sort_L = L; sort_fn = a[1];
        qsort(arr, len, sizeof(ls_value_t), sort_cmp);
        ls_value_t r = ls_nil_v();
        for(size_t i=len;i-->0;) r = ls_cons(L, arr[i], r);
        free(arr);
        return r;
    }
    return a[0];
}

/* random */
static uint64_t lsrand_state = 0x123456789abcdef0ULL;
static uint64_t lsrand_next(void) {
    /* xorshift64* */
    uint64_t x = lsrand_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    lsrand_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static ls_value_t bi_random(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("random",1);
    if (a[0].tag == LS_T_FIXNUM) {
        int64_t lim = a[0].u.fixnum;
        if (lim <= 0) ls_error(L, "random: limit must be positive");
        return ls_make_fixnum((int64_t)(lsrand_next() % (uint64_t)lim));
    }
    if (a[0].tag == LS_T_FLONUM) {
        double lim = a[0].u.flonum;
        if (lim <= 0.0) ls_error(L, "random: limit must be positive");
        return ls_make_flonum(((double)lsrand_next() / (double)UINT64_MAX) * lim);
    }
    ls_error(L, "random: bad arg");
    return ls_nil_v();
}
static ls_value_t bi_make_random_state(ls_state_t *L, int n, ls_value_t *a) {
    (void)L;
    if (n >= 1 && a[0].tag == LS_T_FIXNUM) lsrand_state = (uint64_t)a[0].u.fixnum * 6364136223846793005ULL + 1;
    else lsrand_state = (uint64_t)time(NULL) * 6364136223846793005ULL + 1;
    return ls_make_fixnum((int64_t)lsrand_state);
}

/* boundp / fboundp */
static ls_value_t bi_boundp(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("boundp",1);
    if (a[0].tag != LS_T_SYMBOL) return ls_nil_v();
    ls_symbol_t *s = (ls_symbol_t*)a[0].u.ptr;
    return (s->sym_flags & LS_SYM_HAS_VALUE) ? ls_t_v() : ls_nil_v();
}
static ls_value_t bi_fboundp(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("fboundp",1);
    if (a[0].tag != LS_T_SYMBOL) return ls_nil_v();
    ls_symbol_t *s = (ls_symbol_t*)a[0].u.ptr;
    return (s->function.tag != LS_T_NIL) ? ls_t_v() : ls_nil_v();
}
static ls_value_t bi_makunbound(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("makunbound",1);
    if (a[0].tag != LS_T_SYMBOL) return a[0];
    ls_symbol_t *s = (ls_symbol_t*)a[0].u.ptr;
    s->value = ls_nil_v();
    s->sym_flags &= ~LS_SYM_HAS_VALUE;
    return a[0];
}
static ls_value_t bi_fmakunbound(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("fmakunbound",1);
    if (a[0].tag != LS_T_SYMBOL) return a[0];
    ls_symbol_t *s = (ls_symbol_t*)a[0].u.ptr;
    s->function = ls_nil_v();
    return a[0];
}

/* sleep */
#ifdef _WIN32
extern void __stdcall Sleep(unsigned long);
#endif
static ls_value_t bi_sleep(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("sleep",1);
    double secs = num_as_f(a[0]);
    if (secs < 0) secs = 0;
#ifdef _WIN32
    Sleep((unsigned long)(secs * 1000.0));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#endif
    return ls_nil_v();
}

/* signum / isqrt */
static ls_value_t bi_signum(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("signum",1);
    if (a[0].tag == LS_T_FIXNUM) {
        int64_t i = a[0].u.fixnum;
        return ls_make_fixnum(i > 0 ? 1 : (i < 0 ? -1 : 0));
    }
    if (a[0].tag == LS_T_FLONUM) {
        double f = a[0].u.flonum;
        return ls_make_flonum(f > 0 ? 1.0 : (f < 0 ? -1.0 : 0.0));
    }
    return ls_make_fixnum(0);
}
static ls_value_t bi_isqrt(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("isqrt",1);
    if (a[0].tag != LS_T_FIXNUM) ls_error(L, "isqrt: needs a fixnum");
    int64_t v = a[0].u.fixnum;
    if (v < 0) ls_error(L, "isqrt: negative argument");
    if (v < 2) return ls_make_fixnum(v);
    int64_t lo = 0, hi = v;
    while (lo < hi) {
        int64_t m = lo + (hi - lo + 1) / 2;
        if (m <= v / m) lo = m; else hi = m - 1;
    }
    return ls_make_fixnum(lo);
}

/* macroexpand-1 / macroexpand */
static ls_value_t bi_macroexpand_1(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("macroexpand-1",1);
    ls_value_t form = a[0];
    if (form.tag != LS_T_CONS) return form;
    ls_value_t head = ls_car(form);
    if (head.tag != LS_T_SYMBOL) return form;
    ls_symbol_t *s = (ls_symbol_t*)head.u.ptr;
    if (s->function.tag == LS_T_FUNCTION) {
        ls_function_t *fn = (ls_function_t*)s->function.u.ptr;
        if (fn->fn_flags & LS_FN_MACRO) {
            ls_value_t rest = ls_cdr(form);
            int nargs = (int)ls_list_length(rest);
            ls_value_t *args = (ls_value_t*)malloc(sizeof(ls_value_t) * (nargs > 0 ? (size_t)nargs : 1));
            ls_value_t cur = rest;
            for (int i = 0; i < nargs; i++) { args[i] = ls_car(cur); cur = ls_cdr(cur); }
            ls_value_t r = ls_apply(L, s->function, nargs, args);
            free(args);
            return r;
        }
    }
    return form;
}
static ls_value_t bi_macroexpand(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("macroexpand",1);
    ls_value_t cur = a[0];
    for (int safety = 0; safety < 1000; safety++) {
        ls_value_t next = bi_macroexpand_1(L, 1, &cur);
        if (next.tag == cur.tag && next.u.ptr == cur.u.ptr) break;
        cur = next;
    }
    return cur;
}

/* identity */
static ls_value_t bi_identity(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("identity",1);
    return a[0];
}

/* string-capitalize (string-upcase / string-downcase already defined above) */
static ls_value_t bi_string_capitalize(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("string-capitalize",1);
    ls_string_t *s = ls_string_p(a[0]);
    if (!s) { ls_type_error(L,"string",a[0]); return ls_nil_v(); }
    ls_value_t r = ls_make_string(L, s->chars, s->len);
    ls_string_t *rs = (ls_string_t*)r.u.ptr;
    int word_start = 1;
    for (size_t i = 0; i < rs->len; i++) {
        unsigned char c = (unsigned char)rs->chars[i];
        if (isalnum(c)) {
            rs->chars[i] = (char)(word_start ? toupper(c) : tolower(c));
            word_start = 0;
        } else {
            word_start = 1;
        }
    }
    return r;
}

/* char predicates / conversions */
static ls_value_t bi_char_upcase(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("char-upcase",1);
    if (a[0].tag != LS_T_CHAR) return a[0];
    return ls_make_char((unsigned char)toupper((unsigned char)a[0].u.character));
}
static ls_value_t bi_char_downcase(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("char-downcase",1);
    if (a[0].tag != LS_T_CHAR) return a[0];
    return ls_make_char((unsigned char)tolower((unsigned char)a[0].u.character));
}
static ls_value_t bi_alpha_char_p(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("alpha-char-p",1);
    if (a[0].tag != LS_T_CHAR) return ls_nil_v();
    return isalpha((unsigned char)a[0].u.character) ? ls_t_v() : ls_nil_v();
}
static ls_value_t bi_digit_char_p(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("digit-char-p",1);
    if (a[0].tag != LS_T_CHAR) return ls_nil_v();
    int c = (int)a[0].u.character;
    if (c >= '0' && c <= '9') return ls_make_fixnum(c - '0');
    return ls_nil_v();
}
static ls_value_t bi_alphanumericp(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("alphanumericp",1);
    if (a[0].tag != LS_T_CHAR) return ls_nil_v();
    return isalnum((unsigned char)a[0].u.character) ? ls_t_v() : ls_nil_v();
}
static ls_value_t bi_upper_case_p(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("upper-case-p",1);
    if (a[0].tag != LS_T_CHAR) return ls_nil_v();
    return isupper((unsigned char)a[0].u.character) ? ls_t_v() : ls_nil_v();
}
static ls_value_t bi_lower_case_p(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; ARGN("lower-case-p",1);
    if (a[0].tag != LS_T_CHAR) return ls_nil_v();
    return islower((unsigned char)a[0].u.character) ? ls_t_v() : ls_nil_v();
}

/* endp */
static ls_value_t bi_endp(ls_state_t *L, int n, ls_value_t *a) {
    ARGN("endp",1);
    if (a[0].tag == LS_T_NIL) return ls_t_v();
    if (a[0].tag == LS_T_CONS) return ls_nil_v();
    ls_error(L, "endp: not a proper list tail");
    return ls_nil_v();
}

/* %handler-case: protect a thunk with an error handler.
 * (%handler-case TYPE BODY-THUNK HANDLER-FN)
 * If BODY-THUNK errors during evaluation, the error is caught and
 * HANDLER-FN is called with the condition (a string for now). */
static ls_value_t bi_pct_handler_case(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 3) ls_error(L, "%%handler-case: need 3 args");
    /* a[0] = type (ignored for the simple version), a[1] = body thunk, a[2] = handler. */
    ls_value_t body = a[1], handler = a[2];
    ls_escape_t e; memset(&e, 0, sizeof e);
    e.kind = 2; e.next = L->esc_top; L->esc_top = &e;
    ls_value_t result;
    if (setjmp(e.buf) == 0) {
        result = ls_apply(L, body, 0, NULL);
        L->esc_top = e.next;
        return result;
    } else {
        L->esc_top = e.next;
        /* Build a simple condition value: the error message as a string. */
        ls_value_t cond_v = ls_make_string(L, L->err_buf, strlen(L->err_buf));
        return ls_apply(L, handler, 1, &cond_v);
    }
}

/* %handler-bind: like %handler-case but lighter -- just installs handlers
 * and runs the body. We treat it the same as %handler-case for now. */
static ls_value_t bi_pct_handler_bind(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) ls_error(L, "%%handler-bind: need 2 args");
    /* a[0] = handlers list, a[1] = body thunk */
    ls_value_t handlers = a[0], body = a[1];
    ls_escape_t e; memset(&e, 0, sizeof e);
    e.kind = 2; e.next = L->esc_top; L->esc_top = &e;
    ls_value_t result;
    if (setjmp(e.buf) == 0) {
        result = ls_apply(L, body, 0, NULL);
        L->esc_top = e.next;
        return result;
    } else {
        L->esc_top = e.next;
        /* Run the first handler with the condition string. */
        if (handlers.tag == LS_T_CONS) {
            ls_value_t entry = ls_car(handlers);
            if (entry.tag == LS_T_CONS) {
                ls_value_t hfn = ls_cdr(entry);
                if (hfn.tag == LS_T_CONS) hfn = ls_car(hfn);
                ls_value_t cond_v = ls_make_string(L, L->err_buf, strlen(L->err_buf));
                return ls_apply(L, hfn, 1, &cond_v);
            }
        }
        return ls_nil_v();
    }
}

/* %restart-case: like handler-case but for restarts. Simplified: just runs body,
 * ignoring restart-specs. */
static ls_value_t bi_pct_restart_case(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) ls_error(L, "%%restart-case: need 2 args");
    ls_value_t body = a[1];
    return ls_apply(L, body, 0, NULL);
}

/* %restart-bind: same simplification. */
static ls_value_t bi_pct_restart_bind(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) ls_error(L, "%%restart-bind: need 2 args");
    ls_value_t body = a[1];
    return ls_apply(L, body, 0, NULL);
}

/* signal / cerror: simplified to ls_error. */
static ls_value_t bi_signal(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 1) ls_error(L, "signal: need a condition");
    if (a[0].tag == LS_T_STRING) ls_error(L, "%s", ((ls_string_t*)a[0].u.ptr)->chars);
    ls_error(L, "signal");
    return ls_nil_v();
}
static ls_value_t bi_cerror(ls_state_t *L, int n, ls_value_t *a) {
    if (n < 2) ls_error(L, "cerror: need at least 2 args");
    if (a[1].tag == LS_T_STRING) ls_error(L, "%s", ((ls_string_t*)a[1].u.ptr)->chars);
    ls_error(L, "cerror");
    return ls_nil_v();
}
static ls_value_t bi_make_condition(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)n;
    /* Trivial: return the type symbol. */
    return n >= 1 ? a[0] : ls_nil_v();
}
static ls_value_t bi_find_restart(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)n; (void)a;
    return ls_nil_v();
}
static ls_value_t bi_invoke_restart(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)n; (void)a;
    ls_error(L, "invoke-restart: no restart found");
    return ls_nil_v();
}
static ls_value_t bi_compute_restarts(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)n; (void)a;
    return ls_nil_v();
}
static ls_value_t bi_find_class(ls_state_t *L, int n, ls_value_t *a) {
    (void)L; (void)n;
    /* Trivial: return the type symbol. */
    return n >= 1 ? a[0] : ls_nil_v();
}

/* ============================================================
 *  REGISTRATION
 * ============================================================ */
void ls_register_builtins(ls_state_t *L) {
#define DEF(name, fn, min, max) ls_defun(L, "COMMON-LISP", name, fn, min, max)
    /* Arithmetic */
    DEF("+",  bi_add, 0, -1);   DEF("-",  bi_sub, 0, -1);
    DEF("*",  bi_mul, 0, -1);   DEF("/",  bi_div, 1, -1);
    DEF("MOD", bi_mod, 2, 2);   DEF("REM", bi_rem, 2, 2);
    DEF("ABS", bi_abs, 1, 1);
    DEF("FLOOR", bi_floor, 1, 2); DEF("CEILING", bi_ceiling, 1, 2);
    DEF("ROUND", bi_round, 1, 2); DEF("TRUNCATE", bi_truncate, 1, 2);
    DEF("MAX", bi_max, 1, -1);  DEF("MIN", bi_min, 1, -1);
    DEF("EXPT", bi_expt, 2, 2); DEF("SQRT", bi_sqrt, 1, 1);
    DEF("SIN", bi_sin, 1, 1);   DEF("COS", bi_cos, 1, 1);
    DEF("TAN", bi_tan, 1, 1);   DEF("ASIN", bi_asin, 1, 1);
    DEF("ACOS", bi_acos, 1, 1); DEF("ATAN", bi_atan, 1, 2);
    DEF("LOG", bi_log, 1, 2);   DEF("EXP", bi_exp, 1, 1);
    DEF("1+", bi_1plus, 1, 1);  DEF("1-", bi_1minus, 1, 1);
    DEF("=", bi_numeq, 1, -1);  DEF("<", bi_numlt, 1, -1);
    DEF("<=", bi_numle, 1, -1); DEF(">", bi_numgt, 1, -1);
    DEF(">=", bi_numge, 1, -1);
    DEF("ZEROP", bi_zerop, 1, 1);  DEF("PLUSP", bi_plusp, 1, 1); DEF("MINUSP", bi_minusp, 1, 1);
    DEF("NUMBERP", bi_numberp, 1, 1); DEF("INTEGERP", bi_integerp, 1, 1);
    DEF("FLOATP", bi_floatp, 1, 1); DEF("EVENP", bi_evenp, 1, 1); DEF("ODDP", bi_oddp, 1, 1);
    DEF("LOGAND", bi_logand, 0, -1); DEF("LOGIOR", bi_logior, 0, -1);
    DEF("LOGXOR", bi_logxor, 0, -1); DEF("LOGNOT", bi_lognot, 1, 1);
    DEF("ASH", bi_ash, 2, 2);

    /* Cons/list */
    DEF("CONS", bi_cons, 2, 2); DEF("CAR", bi_car, 1, 1); DEF("CDR", bi_cdr, 1, 1);
    DEF("FIRST", bi_car, 1, 1); DEF("REST", bi_cdr, 1, 1); DEF("SECOND", bi_cadr, 1, 1);
    DEF("THIRD", bi_caddr, 1, 1);
    DEF("CAAR", bi_caar, 1, 1); DEF("CADR", bi_cadr, 1, 1);
    DEF("CDDR", bi_cddr, 1, 1); DEF("CDAR", bi_cdar, 1, 1); DEF("CADDR", bi_caddr, 1, 1);
    DEF("RPLACA", bi_rplaca, 2, 2); DEF("RPLACD", bi_rplacd, 2, 2);
    DEF("LIST", bi_list, 0, -1); DEF("LIST*", bi_liststar, 1, -1);
    DEF("LENGTH", bi_length, 1, 1); DEF("APPEND", bi_append, 0, -1);
    DEF("REVERSE", bi_reverse, 1, 1); DEF("NTH", bi_nth, 2, 2);
    DEF("NTHCDR", bi_nthcdr, 2, 2); DEF("LAST", bi_last, 1, 1);
    DEF("COPY-LIST", bi_copy_list, 1, 1);
    DEF("MEMBER", bi_member, 2, -1); DEF("ASSOC", bi_assoc, 2, -1);
    DEF("MAPCAR", bi_mapcar, 2, 2); DEF("MAPC", bi_mapc, 2, 2);
    DEF("REDUCE", bi_reduce, 2, -1);
    DEF("REMOVE-IF", bi_remove_if, 2, 2); DEF("REMOVE-IF-NOT", bi_remove_if_not, 2, 2);
    DEF("SOME", bi_some, 2, 2); DEF("EVERY", bi_every, 2, 2);

    /* Predicates */
    DEF("CONSP", bi_consp, 1, 1); DEF("LISTP", bi_listp, 1, 1);
    DEF("ATOM", bi_atom, 1, 1); DEF("NULL", bi_null, 1, 1);
    DEF("SYMBOLP", bi_symbolp, 1, 1); DEF("STRINGP", bi_stringp, 1, 1);
    DEF("VECTORP", bi_vectorp, 1, 1); DEF("FUNCTIONP", bi_functionp, 1, 1);
    DEF("CHARACTERP", bi_characterp, 1, 1); DEF("KEYWORDP", bi_keywordp, 1, 1);
    DEF("HASH-TABLE-P", bi_hash_table_p, 1, 1);
    DEF("EQ", bi_eq, 2, 2); DEF("EQL", bi_eql, 2, 2);
    DEF("EQUAL", bi_equal, 2, 2); DEF("EQUALP", bi_equalp, 2, 2);
    DEF("NOT", bi_not, 1, 1);

    /* Strings */
    DEF("STRING", bi_string, 1, 1);
    DEF("STRING-UPCASE", bi_string_upcase, 1, 1);
    DEF("STRING-DOWNCASE", bi_string_downcase, 1, 1);
    DEF("STRING=", bi_string_equal, 2, 2);
    DEF("CONCATENATE", bi_string_concat, 0, -1);
    DEF("SUBSEQ", bi_subseq, 2, 3);
    DEF("CHAR-CODE", bi_char_code, 1, 1); DEF("CODE-CHAR", bi_code_char, 1, 1);
    DEF("CHAR-INT", bi_char_int, 1, 1); DEF("CHAR", bi_char_at, 2, 2);
    DEF("STRING-LENGTH", bi_string_length, 1, 1);
    DEF("PARSE-INTEGER", bi_parse_integer, 1, 1);

    /* Vectors */
    DEF("MAKE-ARRAY", bi_make_array, 1, 1); DEF("VECTOR", bi_vector, 0, -1);
    DEF("AREF", bi_aref, 2, 2); DEF("SVREF", bi_aref, 2, 2);
    DEF("SETF-AREF", bi_aset, 3, 3);
    DEF("VECTOR-PUSH-EXTEND", bi_vector_push, 2, 2);

    /* Hash tables */
    DEF("MAKE-HASH-TABLE", bi_make_hash_table, 0, -1);
    DEF("GETHASH", bi_gethash, 2, 3); DEF("PUTHASH", bi_puthash, 3, 3);
    DEF("REMHASH", bi_remhash, 2, 2);
    DEF("HASH-TABLE-COUNT", bi_hash_table_count, 1, 1);
    DEF("MAPHASH", bi_maphash, 2, 2);

    /* I/O */
    DEF("PRINT", bi_print, 1, 1); DEF("PRIN1", bi_prin1, 1, 1);
    DEF("PRINC", bi_princ, 1, 1); DEF("TERPRI", bi_terpri, 0, 0);
    DEF("WRITE-CHAR", bi_write_char, 1, 1); DEF("READ-CHAR", bi_read_char, 0, 0);
    DEF("READ-LINE", bi_read_line, 0, 1); DEF("READ", bi_read, 0, 3);
    DEF("LOAD", bi_load, 1, 1);
    DEF("FORMAT", bi_format, 2, -1);
    DEF("FRESH-LINE", bi_fresh_line, 0, 0);
    DEF("FORCE-OUTPUT", bi_force_output, 0, 1);
    DEF("FINISH-OUTPUT", bi_finish_output, 0, 1);
    DEF("CLEAR-INPUT", bi_clear_input, 0, 1);
    DEF("CLEAR-OUTPUT", bi_clear_output, 0, 1);

    /* Symbols / packages */
    DEF("SYMBOL-NAME", bi_symbol_name, 1, 1);
    DEF("SYMBOL-PACKAGE", bi_symbol_package, 1, 1);
    DEF("SYMBOL-VALUE", bi_symbol_value, 1, 1);
    DEF("SYMBOL-FUNCTION", bi_symbol_function, 1, 1);
    DEF("INTERN", bi_intern, 1, 2);
    DEF("FIND-PACKAGE", bi_find_package, 1, 1);
    DEF("EXPORT", bi_export, 1, 1);

    /* Multi-value */
    DEF("VALUES", bi_values, 0, -1);

    /* Funcall / apply / eval */
    DEF("FUNCALL", bi_funcall, 1, -1);
    DEF("APPLY", bi_apply_builtin, 2, -1);
    DEF("EVAL", bi_eval, 1, 1);

    /* System */
    DEF("TYPE-OF", bi_type_of, 1, 1);
    DEF("TYPEP", bi_typep, 2, 2);
    DEF("SUBTYPEP", bi_subtypep, 2, 2);
    DEF("CHAR-NAME", bi_char_name, 1, 1);
    DEF("MAKE-STRING-OUTPUT-STREAM", bi_make_string_output_stream, 0, 0);
    DEF("MAKE-STRING-INPUT-STREAM", bi_make_string_input_stream, 1, 3);
    DEF("GET-OUTPUT-STREAM-STRING", bi_get_output_stream_string, 1, 1);
    DEF("GENSYM", bi_gensym, 0, 0);
    DEF("ERROR", bi_error, 1, -1);
    DEF("GET-INTERNAL-REAL-TIME", bi_get_internal_real_time, 0, 0);
    DEF("GET-UNIVERSAL-TIME", bi_get_universal_time, 0, 0);
    DEF("ROOM", bi_room, 0, 0);
    DEF("GC", bi_gc, 0, 0);
    DEF("QUIT", bi_quit, 0, 1); DEF("EXIT", bi_quit, 0, 1);
    DEF("COERCE", bi_coerce, 2, 2);
    DEF("SORT", bi_sort, 2, 2);

    /* Random */
    DEF("RANDOM", bi_random, 1, 2);
    DEF("MAKE-RANDOM-STATE", bi_make_random_state, 0, 1);

    /* Bound checks */
    DEF("BOUNDP", bi_boundp, 1, 1);
    DEF("FBOUNDP", bi_fboundp, 1, 1);
    DEF("MAKUNBOUND", bi_makunbound, 1, 1);
    DEF("FMAKUNBOUND", bi_fmakunbound, 1, 1);

    /* Sleep / signum / isqrt */
    DEF("SLEEP", bi_sleep, 1, 1);
    DEF("SIGNUM", bi_signum, 1, 1);
    DEF("ISQRT", bi_isqrt, 1, 1);

    /* Macro expansion */
    DEF("MACROEXPAND-1", bi_macroexpand_1, 1, 2);
    DEF("MACROEXPAND",   bi_macroexpand,   1, 2);

    /* Identity */
    DEF("IDENTITY", bi_identity, 1, 1);

    /* String case (STRING-UPCASE / STRING-DOWNCASE registered earlier) */
    DEF("STRING-CAPITALIZE", bi_string_capitalize, 1, 1);

    /* Char ops */
    DEF("CHAR-UPCASE", bi_char_upcase, 1, 1);
    DEF("CHAR-DOWNCASE", bi_char_downcase, 1, 1);
    DEF("ALPHA-CHAR-P", bi_alpha_char_p, 1, 1);
    DEF("DIGIT-CHAR-P", bi_digit_char_p, 1, 2);
    DEF("ALPHANUMERICP", bi_alphanumericp, 1, 1);
    DEF("UPPER-CASE-P", bi_upper_case_p, 1, 1);
    DEF("LOWER-CASE-P", bi_lower_case_p, 1, 1);

    /* Misc list */
    DEF("ENDP", bi_endp, 1, 1);

    /* Condition system primitives */
    DEF("%HANDLER-CASE", bi_pct_handler_case, 3, 3);
    DEF("%HANDLER-BIND", bi_pct_handler_bind, 2, 2);
    DEF("%RESTART-CASE", bi_pct_restart_case, 2, 2);
    DEF("%RESTART-BIND", bi_pct_restart_bind, 2, 2);
    DEF("SIGNAL", bi_signal, 1, -1);
    DEF("CERROR", bi_cerror, 2, -1);
    DEF("MAKE-CONDITION", bi_make_condition, 1, -1);
    DEF("FIND-RESTART", bi_find_restart, 1, 2);
    DEF("INVOKE-RESTART", bi_invoke_restart, 1, -1);
    DEF("COMPUTE-RESTARTS", bi_compute_restarts, 0, 1);
    DEF("FIND-CLASS", bi_find_class, 1, 2);

    /* Constants */
    ls_value_t pisym = ls_intern(L, "COMMON-LISP", "PI");
    ls_symbol_t *pi_s = (ls_symbol_t*)pisym.u.ptr;
    pi_s->value = ls_make_flonum(3.14159265358979323846);
    pi_s->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_CONSTANT;

    ls_value_t mfb = ls_intern(L, "COMMON-LISP", "MOST-POSITIVE-FIXNUM");
    ((ls_symbol_t*)mfb.u.ptr)->value = ls_make_fixnum(INT64_MAX);
    ((ls_symbol_t*)mfb.u.ptr)->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_CONSTANT;

    ls_value_t mnfb = ls_intern(L, "COMMON-LISP", "MOST-NEGATIVE-FIXNUM");
    ((ls_symbol_t*)mnfb.u.ptr)->value = ls_make_fixnum(INT64_MIN);
    ((ls_symbol_t*)mnfb.u.ptr)->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_CONSTANT;

#undef DEF
}
