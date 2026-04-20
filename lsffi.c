/* lsffi.c -- Foreign Function Interface for Litesrpent.
 *
 * Provides Lisp-level functions for loading shared libraries, looking
 * up symbols, calling foreign functions with type marshalling, and
 * raw memory access (alloc / free / peek / poke).
 *
 * Windows:  LoadLibraryA / GetProcAddress / FreeLibrary
 * Linux:    dlopen / dlsym / dlclose
 *
 * Registered by ls_init_ffi(). */
#include "lscore.h"
#include "lseval.h"
#include <string.h>

/* ============================================================
 *  Platform abstraction
 * ============================================================ */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define LS_LIB_HANDLE HMODULE
#  define LS_LIB_OPEN(p)    LoadLibraryA(p)
#  define LS_LIB_SYM(h,n)   ((void*)GetProcAddress(h, n))
#  define LS_LIB_CLOSE(h)   FreeLibrary(h)
#  define LS_LIB_ERROR()     "LoadLibrary failed"
#  define LS_MEM_ALLOC(sz)   VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
#  define LS_MEM_FREE(p,sz)  VirtualFree(p, 0, MEM_RELEASE)
#else
#  include <dlfcn.h>
#  include <sys/mman.h>
#  define LS_LIB_HANDLE void*
#  define LS_LIB_OPEN(p)    dlopen(p, RTLD_NOW | RTLD_LOCAL)
#  define LS_LIB_SYM(h,n)   dlsym(h, n)
#  define LS_LIB_CLOSE(h)   dlclose(h)
#  define LS_LIB_ERROR()     dlerror()
#  define LS_MEM_ALLOC(sz)   mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
#  define LS_MEM_FREE(p,sz)  munmap(p, sz)
#endif

/* ============================================================
 *  FFI type tags (used in arg_types / ret_type fields)
 * ============================================================ */
enum {
    LS_FFI_VOID    = 0,
    LS_FFI_INT     = 1,
    LS_FFI_INT64   = 2,
    LS_FFI_FLOAT   = 3,
    LS_FFI_DOUBLE  = 4,
    LS_FFI_POINTER = 5,
    LS_FFI_STRING  = 6,
    LS_FFI_UINT8   = 7,
    LS_FFI_UINT16  = 8,
    LS_FFI_UINT32  = 9,
    LS_FFI_UINT64  = 10,
    LS_FFI_INT8    = 11,
    LS_FFI_INT16   = 12,
    LS_FFI_MAX
};

/* ============================================================
 *  Helpers
 * ============================================================ */
#define A(i) args[i]

/* Convert a keyword symbol :int, :double, etc. to an FFI type tag. */
static uint32_t keyword_to_ffi_type(ls_state_t *L, ls_value_t v) {
    if (v.tag != LS_T_SYMBOL) {
        ls_error(L, "ffi: expected a keyword for type, got tag %u", v.tag);
        return LS_FFI_VOID;
    }
    ls_symbol_t *s = (ls_symbol_t *)v.u.ptr;
    const char *n = s->name->chars;
    if (!strcmp(n, "VOID")    || !strcmp(n, "void"))    return LS_FFI_VOID;
    if (!strcmp(n, "INT")     || !strcmp(n, "int"))     return LS_FFI_INT;
    if (!strcmp(n, "INT64")   || !strcmp(n, "int64"))   return LS_FFI_INT64;
    if (!strcmp(n, "FLOAT")   || !strcmp(n, "float"))   return LS_FFI_FLOAT;
    if (!strcmp(n, "DOUBLE")  || !strcmp(n, "double"))  return LS_FFI_DOUBLE;
    if (!strcmp(n, "POINTER") || !strcmp(n, "pointer")) return LS_FFI_POINTER;
    if (!strcmp(n, "STRING")  || !strcmp(n, "string"))  return LS_FFI_STRING;
    if (!strcmp(n, "UINT8")   || !strcmp(n, "uint8"))   return LS_FFI_UINT8;
    if (!strcmp(n, "UINT16")  || !strcmp(n, "uint16"))  return LS_FFI_UINT16;
    if (!strcmp(n, "UINT32")  || !strcmp(n, "uint32"))  return LS_FFI_UINT32;
    if (!strcmp(n, "UINT64")  || !strcmp(n, "uint64"))  return LS_FFI_UINT64;
    if (!strcmp(n, "INT8")    || !strcmp(n, "int8"))    return LS_FFI_INT8;
    if (!strcmp(n, "INT16")   || !strcmp(n, "int16"))   return LS_FFI_INT16;
    ls_error(L, "ffi: unknown type keyword :%s", n);
    return LS_FFI_VOID;
}

/* Marshal a Lisp value into a uint64_t suitable for passing via a
 * function-pointer cast.  Floats/doubles use memcpy into the uint64. */
static uint64_t marshal_arg(ls_state_t *L, ls_value_t v, uint32_t type) {
    uint64_t out = 0;
    switch (type) {
    case LS_FFI_INT:
        if (v.tag == LS_T_FIXNUM) return (uint64_t)(int32_t)v.u.fixnum;
        if (v.tag == LS_T_FLONUM) return (uint64_t)(int32_t)v.u.flonum;
        return 0;
    case LS_FFI_INT64:
        if (v.tag == LS_T_FIXNUM) return (uint64_t)v.u.fixnum;
        if (v.tag == LS_T_FLONUM) return (uint64_t)(int64_t)v.u.flonum;
        return 0;
    case LS_FFI_FLOAT: {
        float f = 0.0f;
        if (v.tag == LS_T_FIXNUM) f = (float)v.u.fixnum;
        else if (v.tag == LS_T_FLONUM) f = (float)v.u.flonum;
        memcpy(&out, &f, sizeof f);
        return out;
    }
    case LS_FFI_DOUBLE: {
        double d = 0.0;
        if (v.tag == LS_T_FIXNUM) d = (double)v.u.fixnum;
        else if (v.tag == LS_T_FLONUM) d = v.u.flonum;
        memcpy(&out, &d, sizeof d);
        return out;
    }
    case LS_FFI_POINTER:
        if (v.tag == LS_T_FOREIGN) return (uint64_t)(uintptr_t)((ls_foreign_t *)v.u.ptr)->ptr;
        if (v.tag == LS_T_FIXNUM) return (uint64_t)v.u.fixnum;
        if (v.tag == LS_T_NIL)    return 0;
        return (uint64_t)(uintptr_t)v.u.ptr;
    case LS_FFI_STRING:
        if (v.tag == LS_T_STRING) return (uint64_t)(uintptr_t)((ls_string_t *)v.u.ptr)->chars;
        if (v.tag == LS_T_FOREIGN) return (uint64_t)(uintptr_t)((ls_foreign_t *)v.u.ptr)->ptr;
        if (v.tag == LS_T_NIL)    return 0;
        ls_error(L, "ffi: cannot marshal tag %u as string", v.tag);
        return 0;
    case LS_FFI_UINT8:
    case LS_FFI_INT8:
        return (v.tag == LS_T_FIXNUM) ? (uint64_t)(uint8_t)v.u.fixnum : 0;
    case LS_FFI_UINT16:
    case LS_FFI_INT16:
        return (v.tag == LS_T_FIXNUM) ? (uint64_t)(uint16_t)v.u.fixnum : 0;
    case LS_FFI_UINT32:
        return (v.tag == LS_T_FIXNUM) ? (uint64_t)(uint32_t)v.u.fixnum : 0;
    case LS_FFI_UINT64:
        return (v.tag == LS_T_FIXNUM) ? (uint64_t)v.u.fixnum : 0;
    default:
        return 0;
    }
}

/* Convert a raw return value back to Lisp. */
static ls_value_t unmarshal_ret(ls_state_t *L, uint64_t raw, uint32_t type) {
    switch (type) {
    case LS_FFI_VOID:
        return ls_nil_v();
    case LS_FFI_INT:
        return ls_make_fixnum((int64_t)(int32_t)(uint32_t)raw);
    case LS_FFI_INT64:
        return ls_make_fixnum((int64_t)raw);
    case LS_FFI_FLOAT: {
        float f;
        memcpy(&f, &raw, sizeof f);
        return ls_make_flonum((double)f);
    }
    case LS_FFI_DOUBLE: {
        double d;
        memcpy(&d, &raw, sizeof d);
        return ls_make_flonum(d);
    }
    case LS_FFI_POINTER: {
        if (raw == 0) return ls_nil_v();
        ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
        ls_foreign_t *fp = (ls_foreign_t *)v.u.ptr;
        fp->ptr = (void *)(uintptr_t)raw;
        fp->size = 0;
        fp->owned = 0;
        return v;
    }
    case LS_FFI_STRING: {
        const char *s = (const char *)(uintptr_t)raw;
        if (!s) return ls_nil_v();
        return ls_make_string(L, s, strlen(s));
    }
    case LS_FFI_UINT8:  return ls_make_fixnum((int64_t)(uint8_t)raw);
    case LS_FFI_UINT16: return ls_make_fixnum((int64_t)(uint16_t)raw);
    case LS_FFI_UINT32: return ls_make_fixnum((int64_t)(uint32_t)raw);
    case LS_FFI_UINT64: return ls_make_fixnum((int64_t)raw);
    case LS_FFI_INT8:   return ls_make_fixnum((int64_t)(int8_t)raw);
    case LS_FFI_INT16:  return ls_make_fixnum((int64_t)(int16_t)raw);
    default:
        return ls_nil_v();
    }
}

/* ============================================================
 *  (ffi-open path) -> foreign-lib
 * ============================================================ */
static ls_value_t bi_ffi_open(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_STRING) {
        ls_error(L, "ffi-open: expected string path");
        return ls_nil_v();
    }
    ls_string_t *path = (ls_string_t *)args[0].u.ptr;
    LS_LIB_HANDLE h = LS_LIB_OPEN(path->chars);
    if (!h) {
        ls_error(L, "ffi-open: cannot load '%s': %s", path->chars, LS_LIB_ERROR());
        return ls_nil_v();
    }
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN_LIB, sizeof(ls_foreign_lib_t));
    ls_foreign_lib_t *lib = (ls_foreign_lib_t *)v.u.ptr;
    lib->handle = (void *)h;
    lib->path   = path;
    return v;
}

/* ============================================================
 *  (ffi-sym lib name) -> foreign-fn
 * ============================================================ */
static ls_value_t bi_ffi_sym(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 2) {
        ls_error(L, "ffi-sym: expected (lib name)");
        return ls_nil_v();
    }
    if (args[0].tag != LS_T_FOREIGN_LIB) {
        ls_error(L, "ffi-sym: first arg must be a foreign-lib");
        return ls_nil_v();
    }
    const char *name = NULL;
    if (args[1].tag == LS_T_STRING)
        name = ((ls_string_t *)args[1].u.ptr)->chars;
    else if (args[1].tag == LS_T_SYMBOL)
        name = ((ls_symbol_t *)args[1].u.ptr)->name->chars;
    else {
        ls_error(L, "ffi-sym: name must be string or symbol");
        return ls_nil_v();
    }
    ls_foreign_lib_t *lib = (ls_foreign_lib_t *)args[0].u.ptr;
    void *sym = LS_LIB_SYM((LS_LIB_HANDLE)lib->handle, name);
    if (!sym) {
        ls_error(L, "ffi-sym: symbol '%s' not found in '%s'",
                 name, lib->path ? lib->path->chars : "?");
        return ls_nil_v();
    }
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN_FN, sizeof(ls_foreign_fn_t));
    ls_foreign_fn_t *fn = (ls_foreign_fn_t *)v.u.ptr;
    fn->entry = sym;
    fn->lib   = lib;
    fn->name  = (args[1].tag == LS_T_STRING)
                    ? (ls_string_t *)args[1].u.ptr
                    : ((ls_symbol_t *)args[1].u.ptr)->name;
    fn->abi      = 0; /* cdecl */
    fn->ret_type = LS_FFI_VOID;
    fn->n_args   = 0;
    fn->arg_types = NULL;
    return v;
}

/* ============================================================
 *  (ffi-close lib)
 * ============================================================ */
static ls_value_t bi_ffi_close(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_FOREIGN_LIB) {
        ls_error(L, "ffi-close: expected foreign-lib");
        return ls_nil_v();
    }
    ls_foreign_lib_t *lib = (ls_foreign_lib_t *)args[0].u.ptr;
    if (lib->handle) {
        LS_LIB_CLOSE((LS_LIB_HANDLE)lib->handle);
        lib->handle = NULL;
    }
    return ls_t_v();
}

/* ============================================================
 *  (ffi-call fn ret-type arg-types &rest args)
 *
 *  Calls the foreign function `fn` with up to 8 arguments.
 *  arg-types is a list of type keywords, args are the values.
 *
 *  Because x86-64 Windows uses a single calling convention (MS ABI)
 *  with first four integer/pointer args in RCX,RDX,R8,R9 and
 *  floats in XMM0-XMM3, we cast the function pointer and call it
 *  directly for up to 8 args.  GCC on Win64 handles this correctly
 *  when we cast to the right prototype.
 * ============================================================ */

/* Typedef function pointer types for 0..8 integer-sized args. */
typedef uint64_t (*ffi_fn0_t)(void);
typedef uint64_t (*ffi_fn1_t)(uint64_t);
typedef uint64_t (*ffi_fn2_t)(uint64_t, uint64_t);
typedef uint64_t (*ffi_fn3_t)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (*ffi_fn4_t)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (*ffi_fn5_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t);
typedef uint64_t (*ffi_fn6_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t);
typedef uint64_t (*ffi_fn7_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);
typedef uint64_t (*ffi_fn8_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t);

/* Float-returning variants: when the return type is float/double the
 * value is in XMM0, not RAX.  We need separate casts. */
typedef double (*ffi_fnd0_t)(void);
typedef double (*ffi_fnd1_t)(uint64_t);
typedef double (*ffi_fnd2_t)(uint64_t, uint64_t);
typedef double (*ffi_fnd3_t)(uint64_t, uint64_t, uint64_t);
typedef double (*ffi_fnd4_t)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef double (*ffi_fnd5_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t);
typedef double (*ffi_fnd6_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t);
typedef double (*ffi_fnd7_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
typedef double (*ffi_fnd8_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t);

typedef float (*ffi_fnf0_t)(void);
typedef float (*ffi_fnf1_t)(uint64_t);
typedef float (*ffi_fnf2_t)(uint64_t, uint64_t);
typedef float (*ffi_fnf3_t)(uint64_t, uint64_t, uint64_t);
typedef float (*ffi_fnf4_t)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef float (*ffi_fnf5_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t);
typedef float (*ffi_fnf6_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t);
typedef float (*ffi_fnf7_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t);
typedef float (*ffi_fnf8_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t);

static ls_value_t bi_ffi_call(ls_state_t *L, int nargs, ls_value_t *args) {
    /* (ffi-call fn ret-type arg-type-list val1 val2 ...) */
    if (nargs < 3) {
        ls_error(L, "ffi-call: need at least (fn ret-type arg-types)");
        return ls_nil_v();
    }
    if (args[0].tag != LS_T_FOREIGN_FN) {
        ls_error(L, "ffi-call: first arg must be a foreign-fn");
        return ls_nil_v();
    }
    ls_foreign_fn_t *fn = (ls_foreign_fn_t *)args[0].u.ptr;
    void *entry = fn->entry;
    if (!entry) {
        ls_error(L, "ffi-call: null entry point");
        return ls_nil_v();
    }

    uint32_t ret_type = keyword_to_ffi_type(L, args[1]);

    /* Walk arg-types list to determine argument count and types. */
    uint32_t atypes[8];
    int acount = 0;
    ls_value_t tlist = args[2];
    while (tlist.tag == LS_T_CONS && acount < 8) {
        atypes[acount] = keyword_to_ffi_type(L, LS_CAR(tlist));
        acount++;
        tlist = LS_CDR(tlist);
    }

    int val_start = 3;
    int val_count = nargs - val_start;
    if (val_count < acount) {
        ls_error(L, "ffi-call: %d args declared but only %d values given",
                 acount, val_count);
        return ls_nil_v();
    }
    if (acount > 8) {
        ls_error(L, "ffi-call: max 8 arguments supported");
        return ls_nil_v();
    }

    /* Marshal arguments. */
    uint64_t av[8] = {0};
    for (int i = 0; i < acount; i++)
        av[i] = marshal_arg(L, args[val_start + i], atypes[i]);

    /* Call through appropriately-typed function pointer. */
    uint64_t raw = 0;
    int is_float_ret  = (ret_type == LS_FFI_FLOAT);
    int is_double_ret = (ret_type == LS_FFI_DOUBLE);

    if (is_double_ret) {
        double dr = 0.0;
        switch (acount) {
        case 0: dr = ((ffi_fnd0_t)entry)();                                         break;
        case 1: dr = ((ffi_fnd1_t)entry)(av[0]);                                    break;
        case 2: dr = ((ffi_fnd2_t)entry)(av[0],av[1]);                              break;
        case 3: dr = ((ffi_fnd3_t)entry)(av[0],av[1],av[2]);                        break;
        case 4: dr = ((ffi_fnd4_t)entry)(av[0],av[1],av[2],av[3]);                  break;
        case 5: dr = ((ffi_fnd5_t)entry)(av[0],av[1],av[2],av[3],av[4]);            break;
        case 6: dr = ((ffi_fnd6_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5]);      break;
        case 7: dr = ((ffi_fnd7_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5],av[6]); break;
        case 8: dr = ((ffi_fnd8_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5],av[6],av[7]); break;
        }
        return ls_make_flonum(dr);
    } else if (is_float_ret) {
        float fr = 0.0f;
        switch (acount) {
        case 0: fr = ((ffi_fnf0_t)entry)();                                         break;
        case 1: fr = ((ffi_fnf1_t)entry)(av[0]);                                    break;
        case 2: fr = ((ffi_fnf2_t)entry)(av[0],av[1]);                              break;
        case 3: fr = ((ffi_fnf3_t)entry)(av[0],av[1],av[2]);                        break;
        case 4: fr = ((ffi_fnf4_t)entry)(av[0],av[1],av[2],av[3]);                  break;
        case 5: fr = ((ffi_fnf5_t)entry)(av[0],av[1],av[2],av[3],av[4]);            break;
        case 6: fr = ((ffi_fnf6_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5]);      break;
        case 7: fr = ((ffi_fnf7_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5],av[6]); break;
        case 8: fr = ((ffi_fnf8_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5],av[6],av[7]); break;
        }
        return ls_make_flonum((double)fr);
    } else {
        switch (acount) {
        case 0: raw = ((ffi_fn0_t)entry)();                                         break;
        case 1: raw = ((ffi_fn1_t)entry)(av[0]);                                    break;
        case 2: raw = ((ffi_fn2_t)entry)(av[0],av[1]);                              break;
        case 3: raw = ((ffi_fn3_t)entry)(av[0],av[1],av[2]);                        break;
        case 4: raw = ((ffi_fn4_t)entry)(av[0],av[1],av[2],av[3]);                  break;
        case 5: raw = ((ffi_fn5_t)entry)(av[0],av[1],av[2],av[3],av[4]);            break;
        case 6: raw = ((ffi_fn6_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5]);      break;
        case 7: raw = ((ffi_fn7_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5],av[6]); break;
        case 8: raw = ((ffi_fn8_t)entry)(av[0],av[1],av[2],av[3],av[4],av[5],av[6],av[7]); break;
        }
        return unmarshal_ret(L, raw, ret_type);
    }
}

/* ============================================================
 *  (ffi-alloc size) -> foreign
 * ============================================================ */
static ls_value_t bi_ffi_alloc(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "ffi-alloc: expected integer size");
        return ls_nil_v();
    }
    size_t sz = (size_t)args[0].u.fixnum;
    if (sz == 0) sz = 1;
    void *p = calloc(1, sz);
    if (!p) {
        ls_error(L, "ffi-alloc: out of memory (%zu bytes)", sz);
        return ls_nil_v();
    }
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ls_foreign_t *fp = (ls_foreign_t *)v.u.ptr;
    fp->ptr   = p;
    fp->size  = sz;
    fp->owned = 1;
    return v;
}

/* ============================================================
 *  (ffi-free ptr)
 * ============================================================ */
static ls_value_t bi_ffi_free(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_FOREIGN) {
        ls_error(L, "ffi-free: expected foreign pointer");
        return ls_nil_v();
    }
    ls_foreign_t *fp = (ls_foreign_t *)args[0].u.ptr;
    if (fp->ptr && fp->owned) {
        free(fp->ptr);
        fp->ptr = NULL;
        fp->owned = 0;
    }
    return ls_nil_v();
}

/* ============================================================
 *  (ffi-peek ptr offset type) -> value
 *  Read from memory at ptr+offset according to type.
 * ============================================================ */
static ls_value_t bi_ffi_peek(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 3) {
        ls_error(L, "ffi-peek: need (ptr offset type)");
        return ls_nil_v();
    }
    void *base = NULL;
    if (args[0].tag == LS_T_FOREIGN)
        base = ((ls_foreign_t *)args[0].u.ptr)->ptr;
    else if (args[0].tag == LS_T_FIXNUM)
        base = (void *)(uintptr_t)args[0].u.fixnum;
    else {
        ls_error(L, "ffi-peek: first arg must be foreign or fixnum");
        return ls_nil_v();
    }
    if (!base) {
        ls_error(L, "ffi-peek: null pointer");
        return ls_nil_v();
    }
    int64_t off = 0;
    if (args[1].tag == LS_T_FIXNUM) off = args[1].u.fixnum;
    uint32_t type = keyword_to_ffi_type(L, args[2]);
    uint8_t *p = (uint8_t *)base + off;

    switch (type) {
    case LS_FFI_INT8:    { int8_t x;   memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_UINT8:   { uint8_t x;  memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_INT16:   { int16_t x;  memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_UINT16:  { uint16_t x; memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_INT:     { int32_t x;  memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_UINT32:  { uint32_t x; memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_INT64:   { int64_t x;  memcpy(&x, p, sizeof x); return ls_make_fixnum(x); }
    case LS_FFI_UINT64:  { uint64_t x; memcpy(&x, p, sizeof x); return ls_make_fixnum((int64_t)x); }
    case LS_FFI_FLOAT:   { float f;    memcpy(&f, p, sizeof f); return ls_make_flonum(f); }
    case LS_FFI_DOUBLE:  { double d;   memcpy(&d, p, sizeof d); return ls_make_flonum(d); }
    case LS_FFI_POINTER: {
        void *pp; memcpy(&pp, p, sizeof pp);
        if (!pp) return ls_nil_v();
        ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
        ls_foreign_t *fp = (ls_foreign_t *)v.u.ptr;
        fp->ptr = pp; fp->size = 0; fp->owned = 0;
        return v;
    }
    case LS_FFI_STRING: {
        const char *s = *(const char **)p;
        if (!s) return ls_nil_v();
        return ls_make_string(L, s, strlen(s));
    }
    default:
        return ls_nil_v();
    }
}

/* ============================================================
 *  (ffi-poke ptr offset type value)
 *  Write to memory at ptr+offset according to type.
 * ============================================================ */
static ls_value_t bi_ffi_poke(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 4) {
        ls_error(L, "ffi-poke: need (ptr offset type value)");
        return ls_nil_v();
    }
    void *base = NULL;
    if (args[0].tag == LS_T_FOREIGN)
        base = ((ls_foreign_t *)args[0].u.ptr)->ptr;
    else if (args[0].tag == LS_T_FIXNUM)
        base = (void *)(uintptr_t)args[0].u.fixnum;
    else {
        ls_error(L, "ffi-poke: first arg must be foreign or fixnum");
        return ls_nil_v();
    }
    if (!base) {
        ls_error(L, "ffi-poke: null pointer");
        return ls_nil_v();
    }
    int64_t off = 0;
    if (args[1].tag == LS_T_FIXNUM) off = args[1].u.fixnum;
    uint32_t type = keyword_to_ffi_type(L, args[2]);
    ls_value_t val = args[3];
    uint8_t *p = (uint8_t *)base + off;

    switch (type) {
    case LS_FFI_INT8:
    case LS_FFI_UINT8: {
        uint8_t x = (uint8_t)(val.tag == LS_T_FIXNUM ? val.u.fixnum : 0);
        memcpy(p, &x, sizeof x);
        break;
    }
    case LS_FFI_INT16:
    case LS_FFI_UINT16: {
        uint16_t x = (uint16_t)(val.tag == LS_T_FIXNUM ? val.u.fixnum : 0);
        memcpy(p, &x, sizeof x);
        break;
    }
    case LS_FFI_INT:
    case LS_FFI_UINT32: {
        uint32_t x = (uint32_t)(val.tag == LS_T_FIXNUM ? val.u.fixnum : 0);
        memcpy(p, &x, sizeof x);
        break;
    }
    case LS_FFI_INT64:
    case LS_FFI_UINT64: {
        uint64_t x = (val.tag == LS_T_FIXNUM) ? (uint64_t)val.u.fixnum : 0;
        memcpy(p, &x, sizeof x);
        break;
    }
    case LS_FFI_FLOAT: {
        float f = (val.tag == LS_T_FLONUM) ? (float)val.u.flonum
                : (val.tag == LS_T_FIXNUM) ? (float)val.u.fixnum : 0.0f;
        memcpy(p, &f, sizeof f);
        break;
    }
    case LS_FFI_DOUBLE: {
        double d = (val.tag == LS_T_FLONUM) ? val.u.flonum
                 : (val.tag == LS_T_FIXNUM) ? (double)val.u.fixnum : 0.0;
        memcpy(p, &d, sizeof d);
        break;
    }
    case LS_FFI_POINTER: {
        void *pp = NULL;
        if (val.tag == LS_T_FOREIGN) pp = ((ls_foreign_t *)val.u.ptr)->ptr;
        else if (val.tag == LS_T_FIXNUM) pp = (void *)(uintptr_t)val.u.fixnum;
        memcpy(p, &pp, sizeof pp);
        break;
    }
    case LS_FFI_STRING: {
        const char *s = NULL;
        if (val.tag == LS_T_STRING) s = ((ls_string_t *)val.u.ptr)->chars;
        memcpy(p, &s, sizeof s);
        break;
    }
    default: break;
    }
    return ls_nil_v();
}

/* ============================================================
 *  (mem-ref ptr type offset) -> value
 *  Convenience alias: reorders to (ffi-peek ptr offset type).
 * ============================================================ */
static ls_value_t bi_mem_ref(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 3) {
        ls_error(L, "mem-ref: need (ptr type offset)");
        return ls_nil_v();
    }
    /* Reorder: (ptr type offset) -> (ptr offset type) */
    ls_value_t reordered[3];
    reordered[0] = args[0];
    reordered[1] = args[2];
    reordered[2] = args[1];
    return bi_ffi_peek(L, 3, reordered);
}

/* ============================================================
 *  (ffi-ptr-address ptr) -> fixnum
 *  Return the raw numeric address of a foreign pointer.
 * ============================================================ */
static ls_value_t bi_ffi_ptr_address(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) {
        ls_error(L, "ffi-ptr-address: expected foreign");
        return ls_nil_v();
    }
    if (args[0].tag == LS_T_FOREIGN) {
        ls_foreign_t *fp = (ls_foreign_t *)args[0].u.ptr;
        return ls_make_fixnum((int64_t)(uintptr_t)fp->ptr);
    }
    if (args[0].tag == LS_T_FOREIGN_LIB) {
        ls_foreign_lib_t *lib = (ls_foreign_lib_t *)args[0].u.ptr;
        return ls_make_fixnum((int64_t)(uintptr_t)lib->handle);
    }
    if (args[0].tag == LS_T_FOREIGN_FN) {
        ls_foreign_fn_t *fn = (ls_foreign_fn_t *)args[0].u.ptr;
        return ls_make_fixnum((int64_t)(uintptr_t)fn->entry);
    }
    return ls_make_fixnum(0);
}

/* ============================================================
 *  (ffi-mem-alloc-exec size) -> foreign
 *  Allocate executable memory (for JIT / trampoline use).
 * ============================================================ */
static ls_value_t bi_ffi_mem_alloc_exec(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "ffi-mem-alloc-exec: expected integer size");
        return ls_nil_v();
    }
    size_t sz = (size_t)args[0].u.fixnum;
    if (sz == 0) sz = 4096;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p) {
        ls_error(L, "ffi-mem-alloc-exec: allocation failed");
        return ls_nil_v();
    }
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ls_foreign_t *fp = (ls_foreign_t *)v.u.ptr;
    fp->ptr   = p;
    fp->size  = sz;
    fp->owned = 1;
    return v;
}

/* ============================================================
 *  (ffi-mem-free-exec ptr)
 * ============================================================ */
static ls_value_t bi_ffi_mem_free_exec(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_FOREIGN) {
        ls_error(L, "ffi-mem-free-exec: expected foreign");
        return ls_nil_v();
    }
    ls_foreign_t *fp = (ls_foreign_t *)args[0].u.ptr;
    if (fp->ptr && fp->owned) {
        LS_MEM_FREE(fp->ptr, fp->size);
        fp->ptr = NULL;
        fp->owned = 0;
    }
    return ls_nil_v();
}

/* ============================================================
 *  (ffi-memcpy dst dst-offset src src-offset count)
 * ============================================================ */
static ls_value_t bi_ffi_memcpy(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 5) {
        ls_error(L, "ffi-memcpy: need (dst dst-off src src-off count)");
        return ls_nil_v();
    }
    void *dst = NULL, *src = NULL;
    if (args[0].tag == LS_T_FOREIGN) dst = ((ls_foreign_t *)args[0].u.ptr)->ptr;
    else if (args[0].tag == LS_T_FIXNUM) dst = (void *)(uintptr_t)args[0].u.fixnum;
    if (args[2].tag == LS_T_FOREIGN) src = ((ls_foreign_t *)args[2].u.ptr)->ptr;
    else if (args[2].tag == LS_T_FIXNUM) src = (void *)(uintptr_t)args[2].u.fixnum;
    /* Also handle vector source for writing byte vectors to foreign memory. */
    else if (args[2].tag == LS_T_VECTOR) {
        ls_vector_t *vec = (ls_vector_t *)args[2].u.ptr;
        int64_t soff = (args[3].tag == LS_T_FIXNUM) ? args[3].u.fixnum : 0;
        int64_t count = (args[4].tag == LS_T_FIXNUM) ? args[4].u.fixnum : 0;
        int64_t doff = (args[1].tag == LS_T_FIXNUM) ? args[1].u.fixnum : 0;
        if (!dst) { ls_error(L, "ffi-memcpy: null dst"); return ls_nil_v(); }
        uint8_t *dp = (uint8_t *)dst + doff;
        for (int64_t i = 0; i < count && (soff + i) < (int64_t)vec->len; i++) {
            ls_value_t el = vec->data[soff + i];
            dp[i] = (el.tag == LS_T_FIXNUM) ? (uint8_t)el.u.fixnum : 0;
        }
        return ls_nil_v();
    }
    if (!dst || !src) {
        ls_error(L, "ffi-memcpy: null pointer");
        return ls_nil_v();
    }
    int64_t doff  = (args[1].tag == LS_T_FIXNUM) ? args[1].u.fixnum : 0;
    int64_t soff  = (args[3].tag == LS_T_FIXNUM) ? args[3].u.fixnum : 0;
    int64_t count = (args[4].tag == LS_T_FIXNUM) ? args[4].u.fixnum : 0;
    memcpy((uint8_t *)dst + doff, (uint8_t *)src + soff, (size_t)count);
    return ls_nil_v();
}

/* ============================================================
 *  (ffi-string->ptr str) -> foreign
 *  Returns a foreign pointer to the C string inside an ls_string_t.
 * ============================================================ */
static ls_value_t bi_ffi_string_to_ptr(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1 || args[0].tag != LS_T_STRING) {
        ls_error(L, "ffi-string->ptr: expected string");
        return ls_nil_v();
    }
    ls_string_t *s = (ls_string_t *)args[0].u.ptr;
    ls_value_t v = ls_make_obj(L, LS_T_FOREIGN, sizeof(ls_foreign_t));
    ls_foreign_t *fp = (ls_foreign_t *)v.u.ptr;
    fp->ptr   = s->chars;
    fp->size  = s->len + 1;
    fp->owned = 0;  /* lifetime managed by the string object */
    return v;
}

/* ============================================================
 *  (ffi-ptr->string ptr &optional len) -> string
 * ============================================================ */
static ls_value_t bi_ffi_ptr_to_string(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) {
        ls_error(L, "ffi-ptr->string: expected foreign pointer");
        return ls_nil_v();
    }
    const char *p = NULL;
    if (args[0].tag == LS_T_FOREIGN)
        p = (const char *)((ls_foreign_t *)args[0].u.ptr)->ptr;
    else if (args[0].tag == LS_T_FIXNUM)
        p = (const char *)(uintptr_t)args[0].u.fixnum;
    if (!p) return ls_nil_v();

    size_t len;
    if (nargs >= 2 && args[1].tag == LS_T_FIXNUM)
        len = (size_t)args[1].u.fixnum;
    else
        len = strlen(p);
    return ls_make_string(L, p, len);
}

/* ============================================================
 *  (ffi-sizeof type) -> fixnum
 * ============================================================ */
static ls_value_t bi_ffi_sizeof(ls_state_t *L, int nargs, ls_value_t *args) {
    if (nargs < 1) {
        ls_error(L, "ffi-sizeof: expected type keyword");
        return ls_nil_v();
    }
    uint32_t type = keyword_to_ffi_type(L, args[0]);
    size_t sz = 0;
    switch (type) {
    case LS_FFI_VOID:    sz = 0; break;
    case LS_FFI_INT8:
    case LS_FFI_UINT8:   sz = 1; break;
    case LS_FFI_INT16:
    case LS_FFI_UINT16:  sz = 2; break;
    case LS_FFI_INT:
    case LS_FFI_UINT32:
    case LS_FFI_FLOAT:   sz = 4; break;
    case LS_FFI_INT64:
    case LS_FFI_UINT64:
    case LS_FFI_DOUBLE:
    case LS_FFI_POINTER:
    case LS_FFI_STRING:  sz = 8; break;
    default:             sz = 0; break;
    }
    return ls_make_fixnum((int64_t)sz);
}

/* ============================================================
 *  Registration
 * ============================================================ */
void ls_init_ffi(ls_state_t *L) {
#define DFFI(name, fn, min, max) ls_defun(L, "LITESRPENT-SYSTEM", name, fn, min, max)
    DFFI("FFI-OPEN",           bi_ffi_open,          1, 1);
    DFFI("FFI-SYM",            bi_ffi_sym,           2, 2);
    DFFI("FFI-CLOSE",          bi_ffi_close,         1, 1);
    DFFI("FFI-CALL",           bi_ffi_call,          3, -1);
    DFFI("FFI-ALLOC",          bi_ffi_alloc,         1, 1);
    DFFI("FFI-FREE",           bi_ffi_free,          1, 1);
    DFFI("FFI-PEEK",           bi_ffi_peek,          3, 3);
    DFFI("FFI-POKE",           bi_ffi_poke,          4, 4);
    DFFI("MEM-REF",            bi_mem_ref,           3, 3);
    DFFI("FFI-PTR-ADDRESS",    bi_ffi_ptr_address,   1, 1);
    DFFI("FFI-MEM-ALLOC-EXEC", bi_ffi_mem_alloc_exec,1, 1);
    DFFI("FFI-MEM-FREE-EXEC",  bi_ffi_mem_free_exec, 1, 1);
    DFFI("FFI-MEMCPY",         bi_ffi_memcpy,        5, 5);
    DFFI("FFI-STRING->PTR",    bi_ffi_string_to_ptr, 1, 1);
    DFFI("FFI-PTR->STRING",    bi_ffi_ptr_to_string, 1, 2);
    DFFI("FFI-SIZEOF",         bi_ffi_sizeof,        1, 1);
#undef DFFI
}
