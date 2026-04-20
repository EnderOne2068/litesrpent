/* lscore.h -- internal core types and macros shared by every module. */
#ifndef LS_CORE_H
#define LS_CORE_H

#include "litesrpent.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Type tags.  Must match order used by the printer, compiler, AOT
 * transpiler and FFI marshaller.  Do not renumber without touching
 * lsprinter.c and lsaot.c. */
enum {
    LS_T_NIL = 0,
    LS_T_T,
    LS_T_FIXNUM,
    LS_T_FLONUM,
    LS_T_CHAR,
    LS_T_SYMBOL,
    LS_T_STRING,
    LS_T_CONS,
    LS_T_VECTOR,
    LS_T_HASHTABLE,
    LS_T_FUNCTION,        /* interpreted closure */
    LS_T_BYTECODE,        /* compiled bytecode closure */
    LS_T_NATIVE,          /* C callback */
    LS_T_MACRO,
    LS_T_SPECIAL,         /* special form marker */
    LS_T_PACKAGE,
    LS_T_STREAM,
    LS_T_FOREIGN,         /* opaque FFI handle / pointer */
    LS_T_FOREIGN_LIB,     /* HMODULE / void* from dlopen */
    LS_T_FOREIGN_FN,      /* ptr + cif for calling */
    LS_T_CLASS,           /* CLOS class */
    LS_T_INSTANCE,        /* CLOS instance */
    LS_T_GENERIC,         /* generic function */
    LS_T_METHOD,
    LS_T_CONDITION,
    LS_T_RESTART,
    LS_T_PATHNAME,
    LS_T_RATIO,
    LS_T_BIGNUM,
    LS_T_COMPLEX,
    LS_T_READTABLE,
    LS_T_RANDOM_STATE,
    LS_T_STRUCTURE,
    LS_T_ARRAY,           /* multi-dim array (vector is the 1-d specialization) */
    LS_T_MAX
};

/* GC flags. */
#define LS_GC_MARK       0x01u
#define LS_GC_PINNED     0x02u
#define LS_GC_CONSTANT   0x04u
#define LS_GC_FOREIGN_OWNED 0x08u

/* ------- Heap object layout -------
 *
 * Every heap object begins with an ls_obj_header so the GC can
 * visit it uniformly.  The tag in the ls_value_t mirrors the tag
 * in the header; the redundancy lets us short-circuit for
 * immediates. */
typedef struct ls_obj_header {
    uint16_t tag;
    uint16_t flags;
    uint32_t size;        /* bytes of the whole object, incl. trailing data */
    struct ls_obj_header *next;  /* intrusive free list in the nursery */
} ls_obj_header_t;

/* Cons cell. */
typedef struct ls_cons {
    ls_obj_header_t h;
    ls_value_t car;
    ls_value_t cdr;
} ls_cons_t;

/* Interned string.  char_ is NUL-terminated for C interop. */
typedef struct ls_string {
    ls_obj_header_t h;
    size_t len;
    size_t cap;
    char  *chars;
} ls_string_t;

/* Symbol.  A symbol is (name, package, value, function, plist). */
typedef struct ls_symbol {
    ls_obj_header_t h;
    ls_string_t *name;
    struct ls_package *package;    /* home package or NULL */
    ls_value_t value;              /* global value slot, LS_T_NIL = unbound sentinel */
    ls_value_t function;           /* function slot */
    ls_value_t plist;              /* property list */
    uint32_t   sym_flags;          /* :constant :special etc */
    uint32_t   hash;
} ls_symbol_t;

#define LS_SYM_CONSTANT   0x01u
#define LS_SYM_SPECIAL    0x02u    /* dynamically scoped (defparameter/defvar) */
#define LS_SYM_KEYWORD    0x04u
#define LS_SYM_HAS_VALUE  0x08u
#define LS_SYM_HAS_FN     0x10u
#define LS_SYM_HAS_MACRO  0x20u

/* Vector (1-d, element-type T for simplicity; specialized arrays
 * use LS_T_ARRAY).  fill_ptr supports adjustable vectors. */
typedef struct ls_vector {
    ls_obj_header_t h;
    size_t len;
    size_t cap;
    size_t fill_ptr;      /* (size_t)-1 if no fill pointer */
    int    adjustable;
    ls_value_t *data;
} ls_vector_t;

/* Multi-dimensional array. */
typedef struct ls_array {
    ls_obj_header_t h;
    uint32_t rank;
    uint32_t element_type;
    size_t   total_size;
    size_t   *dims;
    void     *data;       /* raw bytes if specialized, ls_value_t* otherwise */
} ls_array_t;

/* Hash table -- open addressing with robin-hood displacement. */
typedef struct ls_hash_entry {
    uint32_t hash;
    uint32_t dist;
    ls_value_t key;
    ls_value_t val;
} ls_hash_entry_t;

typedef enum {
    LS_HASH_EQ, LS_HASH_EQL, LS_HASH_EQUAL, LS_HASH_EQUALP
} ls_hash_test_t;

typedef struct ls_hashtable {
    ls_obj_header_t h;
    size_t count;
    size_t cap;            /* always power of two */
    ls_hash_test_t test;
    ls_hash_entry_t *entries;
} ls_hashtable_t;

/* Interpreted function (closure). */
typedef struct ls_function {
    ls_obj_header_t h;
    ls_value_t lambda_list;
    ls_value_t body;
    struct ls_env *closure;
    ls_symbol_t *name;         /* NULL for anonymous */
    int16_t min_args, max_args;
    uint16_t fn_flags;
    ls_value_t docstring;
} ls_function_t;

#define LS_FN_MACRO          0x01u
#define LS_FN_SPECIAL_OP     0x02u
#define LS_FN_HAS_REST       0x04u
#define LS_FN_HAS_OPTIONAL   0x08u
#define LS_FN_HAS_KEY        0x10u
#define LS_FN_HAS_AUX        0x20u

/* Native (C) callback. */
typedef struct ls_native {
    ls_obj_header_t h;
    ls_native_fn fn;
    const char *name;
    int16_t min_args, max_args;
} ls_native_t;

/* Bytecode closure. */
struct ls_proto;
typedef struct ls_bytecode_fn {
    ls_obj_header_t h;
    struct ls_proto *proto;
    ls_value_t *upvals;           /* captured free vars */
    uint32_t  nupvals;
    void     *jit_entry;          /* NULL until JIT-compiled */
    uint32_t  call_count;         /* bumped by VM on each call; triggers JIT */
    ls_symbol_t *name;
} ls_bytecode_fn_t;

/* Package. */
typedef struct ls_package {
    ls_obj_header_t h;
    ls_string_t *name;
    ls_hashtable_t *internal;      /* name-string -> symbol */
    ls_hashtable_t *external;      /* exported symbols */
    ls_value_t use_list;           /* list of used packages */
    ls_value_t nicknames;
} ls_package_t;

/* I/O stream. */
typedef struct ls_stream {
    ls_obj_header_t h;
    uint32_t direction;            /* 1=in, 2=out, 3=io */
    uint32_t element_type;         /* character or byte */
    FILE   *fp;
    char   *buffer;                /* for string streams */
    size_t  buflen, bufpos, bufcap;
    int     string_stream;
    int     owns_fp;
    int     ungot_char;            /* -1 if none */
    int     eof;
} ls_stream_t;

/* CLOS class. */
typedef struct ls_class {
    ls_obj_header_t h;
    ls_symbol_t *name;
    ls_value_t direct_supers;      /* list of classes */
    ls_value_t direct_slots;       /* list of slot-definitions */
    ls_value_t precedence_list;    /* CPL, list of classes */
    ls_value_t effective_slots;
    ls_value_t metaclass;
    uint32_t   instance_size;      /* bytes */
    uint32_t   n_slots;
    int        finalized;
    ls_hashtable_t *slot_index;    /* slot-name -> index */
} ls_class_t;

/* CLOS instance. */
typedef struct ls_instance {
    ls_obj_header_t h;
    ls_class_t *class_;
    ls_value_t *slots;             /* length class_->n_slots */
} ls_instance_t;

/* Generic function. */
typedef struct ls_generic {
    ls_obj_header_t h;
    ls_symbol_t *name;
    ls_value_t lambda_list;
    ls_value_t methods;
    ls_value_t combination;        /* :standard, :and, :or, ... */
    uint32_t   n_required;
} ls_generic_t;

/* Method. */
typedef struct ls_method {
    ls_obj_header_t h;
    ls_value_t specializers;       /* list of classes or (eql ...) */
    ls_value_t qualifiers;         /* list of keywords: :before :after :around */
    ls_value_t lambda;             /* the underlying function */
    ls_value_t generic;            /* back-pointer */
} ls_method_t;

/* Condition (simple: carries a class, a plist of initargs, and a
 * format control/args for the default message). */
typedef struct ls_condition {
    ls_obj_header_t h;
    ls_class_t *class_;
    ls_value_t initargs;           /* plist */
    ls_value_t format_control;
    ls_value_t format_arguments;
} ls_condition_t;

/* Restart. */
typedef struct ls_restart {
    ls_obj_header_t h;
    ls_symbol_t *name;
    ls_value_t test_fn;
    ls_value_t report_fn;
    ls_value_t interactive_fn;
    ls_value_t invoke_fn;
    int        active;
} ls_restart_t;

/* Foreign (FFI) handles. */
typedef struct ls_foreign {
    ls_obj_header_t h;
    void *ptr;
    size_t size;                   /* 0 if unknown */
    int owned;
} ls_foreign_t;

typedef struct ls_foreign_lib {
    ls_obj_header_t h;
    void *handle;                  /* HMODULE or dlopen result */
    ls_string_t *path;
} ls_foreign_lib_t;

typedef struct ls_foreign_fn {
    ls_obj_header_t h;
    void *entry;
    ls_foreign_lib_t *lib;
    ls_string_t *name;
    uint32_t abi;                  /* 0=cdecl, 1=stdcall, 2=winapi */
    uint32_t ret_type;
    uint32_t n_args;
    uint32_t *arg_types;
} ls_foreign_fn_t;

/* Environment -- chain of frames of (symbol -> value) bindings.
 * The global environment is ls_state::globals (a hashtable on the
 * symbol itself), but lexical frames use ls_env to keep closure
 * semantics simple. */
typedef struct ls_env_binding {
    ls_symbol_t *sym;
    ls_value_t   val;
    uint32_t     special;          /* 1 if this is a dynamic binding */
} ls_env_binding_t;

typedef struct ls_env {
    ls_obj_header_t h;
    struct ls_env *parent;
    size_t count;
    size_t cap;
    ls_env_binding_t *bindings;
} ls_env_t;

/* Bytecode prototype (shared by all closures over it). */
typedef struct ls_proto {
    ls_obj_header_t h;
    uint32_t *code;
    uint32_t  code_len;
    ls_value_t *consts;
    uint32_t  n_consts;
    ls_symbol_t **upvals;
    uint32_t  n_upvals;
    uint32_t  n_args;
    uint32_t  n_opt;
    uint32_t  n_locals;
    uint32_t  has_rest;
    ls_symbol_t *name;
    ls_value_t  source;            /* optional original S-expression */
    uint32_t  *line_info;          /* pc -> line number */
    uint32_t  line_info_len;
} ls_proto_t;

/* ------- Dynamic binding stack (for special variables) ------- */
typedef struct ls_dynframe {
    ls_symbol_t *sym;
    ls_value_t   old_val;
    uint32_t     had_value;
    struct ls_dynframe *next;
} ls_dynframe_t;

/* ------- Exception / condition handler stack ------- */
typedef struct ls_handler {
    ls_class_t *type;
    ls_value_t  fn;
    struct ls_handler *next;
} ls_handler_t;

typedef struct ls_restart_frame {
    ls_restart_t *restart;
    struct ls_restart_frame *next;
} ls_restart_frame_t;

/* setjmp escape frame for non-local exits, throws, and conditions. */
typedef struct ls_escape {
    jmp_buf buf;
    ls_value_t tag;
    ls_value_t value;             /* return value when long-jumped */
    int kind;                     /* 0=block, 1=catch, 2=condition, 3=tagbody */
    struct ls_escape *next;
} ls_escape_t;

/* ------- GC arena -------
 *
 * Simple mark-and-sweep over a linked list of allocations.  Not
 * fast, but fully correct and easy to audit.  Can be swapped for a
 * generational copying collector later without touching user code. */
typedef struct ls_gc {
    ls_obj_header_t *all_objects;
    size_t bytes_allocated;
    size_t next_gc;
    int    disabled;
    ls_value_t *gray_stack;
    size_t     gray_count;
    size_t     gray_cap;
} ls_gc_t;

/* ------- Multiple values ------- */
#define LS_MV_MAX 32
typedef struct ls_mv {
    int n;
    ls_value_t v[LS_MV_MAX];
} ls_mv_t;

/* ------- Main state ------- */
struct ls_state {
    ls_gc_t gc;
    ls_env_t *genv;               /* root environment (global/lexical) */
    ls_hashtable_t *packages;     /* name -> package */
    ls_package_t *pkg_cl;         /* COMMON-LISP */
    ls_package_t *pkg_cl_user;    /* COMMON-LISP-USER */
    ls_package_t *pkg_keyword;    /* KEYWORD */
    ls_package_t *pkg_system;     /* LITESRPENT-SYSTEM internals */
    ls_package_t *current_package;

    /* Interned strings (for fast symbol lookup in reader). */
    ls_hashtable_t *symbol_cache;

    /* Classes of builtin types -- used by typep, class-of, CLOS. */
    ls_class_t *class_t;
    ls_class_t *class_standard_object;
    ls_class_t *class_cons;
    ls_class_t *class_symbol;
    ls_class_t *class_string;
    ls_class_t *class_number;
    ls_class_t *class_integer;
    ls_class_t *class_fixnum;
    ls_class_t *class_float;
    ls_class_t *class_vector;
    ls_class_t *class_hashtable;
    ls_class_t *class_function;
    ls_class_t *class_condition;
    ls_class_t *class_error;
    ls_class_t *class_simple_error;
    ls_class_t *class_type_error;
    ls_class_t *class_arith_error;
    ls_class_t *class_undefined_function;
    ls_class_t *class_unbound_variable;

    /* Standard streams. */
    ls_stream_t *stdin_;
    ls_stream_t *stdout_;
    ls_stream_t *stderr_;

    /* Dynamic / handler / escape stacks. */
    ls_dynframe_t *dyn_top;
    ls_handler_t  *hnd_top;
    ls_restart_frame_t *restart_top;
    ls_escape_t  *esc_top;

    /* Multiple values.  After each call, values[0..nvalues-1] holds
     * whatever (values ...) produced. */
    ls_mv_t mv;

    /* Last error message. */
    char err_buf[1024];

    /* Common literal symbols cached to dodge hashtable lookups. */
    ls_symbol_t *sym_quote;
    ls_symbol_t *sym_quasiquote;
    ls_symbol_t *sym_unquote;
    ls_symbol_t *sym_unquote_splicing;
    ls_symbol_t *sym_function;
    ls_symbol_t *sym_lambda;
    ls_symbol_t *sym_if;
    ls_symbol_t *sym_let;
    ls_symbol_t *sym_letstar;
    ls_symbol_t *sym_progn;
    ls_symbol_t *sym_setq;
    ls_symbol_t *sym_defun;
    ls_symbol_t *sym_defmacro;
    ls_symbol_t *sym_defvar;
    ls_symbol_t *sym_defparameter;
    ls_symbol_t *sym_defconstant;
    ls_symbol_t *sym_defgeneric;
    ls_symbol_t *sym_defmethod;
    ls_symbol_t *sym_defclass;
    ls_symbol_t *sym_block;
    ls_symbol_t *sym_return_from;
    ls_symbol_t *sym_tagbody;
    ls_symbol_t *sym_go;
    ls_symbol_t *sym_catch;
    ls_symbol_t *sym_throw;
    ls_symbol_t *sym_unwind_protect;
    ls_symbol_t *sym_handler_case;
    ls_symbol_t *sym_handler_bind;
    ls_symbol_t *sym_restart_case;
    ls_symbol_t *sym_restart_bind;
    ls_symbol_t *sym_multiple_value_bind;
    ls_symbol_t *sym_multiple_value_call;
    ls_symbol_t *sym_multiple_value_prog1;
    ls_symbol_t *sym_the;
    ls_symbol_t *sym_declare;
    ls_symbol_t *sym_eval_when;
    ls_symbol_t *sym_flet;
    ls_symbol_t *sym_labels;
    ls_symbol_t *sym_macrolet;
    ls_symbol_t *sym_symbol_macrolet;
    ls_symbol_t *sym_load_time_value;
    ls_symbol_t *sym_locally;
    ls_symbol_t *sym_and;
    ls_symbol_t *sym_or;
    ls_symbol_t *sym_when;
    ls_symbol_t *sym_unless;
    ls_symbol_t *sym_cond;
    ls_symbol_t *sym_case;
    ls_symbol_t *sym_typecase;
    ls_symbol_t *sym_ampersand_rest;
    ls_symbol_t *sym_ampersand_optional;
    ls_symbol_t *sym_ampersand_key;
    ls_symbol_t *sym_ampersand_aux;
    ls_symbol_t *sym_ampersand_body;
    ls_symbol_t *sym_ampersand_allow_other_keys;
    ls_symbol_t *sym_ampersand_whole;
    ls_symbol_t *sym_ampersand_environment;
    ls_symbol_t *sym_otherwise;
    ls_symbol_t *sym_t;
    ls_symbol_t *sym_do;
    ls_symbol_t *sym_dostar;
    ls_symbol_t *sym_return;
    ls_symbol_t *sym_setf;
    ls_symbol_t *sym_prog;
    ls_symbol_t *sym_progstar;
    ls_symbol_t *sym_dotimes;
    ls_symbol_t *sym_dolist;
    ls_symbol_t *sym_car;
    ls_symbol_t *sym_cdr;
    ls_symbol_t *sym_first;
    ls_symbol_t *sym_rest;
    ls_symbol_t *sym_nth;
    ls_symbol_t *sym_aref;
    ls_symbol_t *sym_gethash;
    ls_symbol_t *sym_slot_value;
    ls_symbol_t *sym_symbol_value;
    ls_symbol_t *sym_symbol_function;

    /* Integration config. */
    char    *cc_path;              /* path to the host C compiler */
    int      verbose;
    int      jit_enabled;
    int      jit_threshold;

    /* JIT arena (executable memory). */
    void    *jit_arena;
    size_t   jit_arena_cap;
    size_t   jit_arena_used;
};

/* ------- Value constructors/accessors (inlined in lscore.c) ------- */
ls_value_t ls_nil_v(void);
ls_value_t ls_t_v(void);

#define LS_TAG(v)       ((v).tag)
#define LS_IS(v, t)     ((v).tag == (t))

ls_value_t ls_make_obj(ls_state_t *L, uint16_t tag, uint32_t size);

/* ------- Public-private accessors for internals ------- */
ls_cons_t       *ls_cons_p      (ls_value_t v);
ls_symbol_t     *ls_symbol_p    (ls_value_t v);
ls_string_t     *ls_string_p    (ls_value_t v);
ls_vector_t     *ls_vector_p    (ls_value_t v);
ls_hashtable_t  *ls_hash_p      (ls_value_t v);
ls_function_t   *ls_function_p  (ls_value_t v);
ls_bytecode_fn_t*ls_bytecode_p  (ls_value_t v);
ls_native_t     *ls_native_p    (ls_value_t v);
ls_package_t    *ls_package_p   (ls_value_t v);
ls_stream_t     *ls_stream_p    (ls_value_t v);
ls_class_t      *ls_class_p     (ls_value_t v);
ls_instance_t   *ls_instance_p  (ls_value_t v);

/* Wrap a heap object pointer into a tagged value. */
ls_value_t ls_wrap(uint16_t tag, void *p);

/* ------- Internal reader/printer/eval entrypoints ------- */
ls_value_t ls_read_from_string(ls_state_t *L, const char *src, const char **end);
int        ls_print_value(ls_state_t *L, ls_value_t v, ls_stream_t *s, int escape);
ls_value_t ls_eval(ls_state_t *L, ls_value_t form, ls_env_t *env);
ls_value_t ls_apply(ls_state_t *L, ls_value_t fn, int nargs, ls_value_t *args);
ls_value_t ls_funcall(ls_state_t *L, ls_value_t fn, int nargs, ...);
ls_value_t ls_progn(ls_state_t *L, ls_value_t forms, ls_env_t *env);
ls_value_t ls_macroexpand(ls_state_t *L, ls_value_t form, ls_env_t *env);

/* ------- Error helpers ------- */
void ls_error(ls_state_t *L, const char *fmt, ...);
void ls_type_error(ls_state_t *L, const char *expected, ls_value_t got);
void ls_arity_error(ls_state_t *L, const char *name, int got, int min, int max);
void ls_undefined_function_error(ls_state_t *L, ls_symbol_t *s);
void ls_unbound_variable_error(ls_state_t *L, ls_symbol_t *s);
void ls_export_symbol(ls_state_t *L, ls_package_t *pkg, ls_symbol_t *sym);

/* ------- GC entry points ------- */
void ls_gc_collect(ls_state_t *L);
void ls_gc_push_root(ls_state_t *L, ls_value_t *slot);
void ls_gc_pop_root(ls_state_t *L);

/* ------- Utility ------- */
uint32_t ls_hash_string(const char *s, size_t n);
uint32_t ls_hash_value(ls_value_t v, ls_hash_test_t t);
int      ls_value_equal(ls_value_t a, ls_value_t b, ls_hash_test_t t);
size_t   ls_list_length(ls_value_t list);
ls_value_t ls_list_nth(ls_value_t list, size_t n);
ls_value_t ls_list_reverse(ls_state_t *L, ls_value_t list);
ls_value_t ls_list_append(ls_state_t *L, ls_value_t a, ls_value_t b);

/* ------- Symbol interning / package operations ------- */
ls_value_t ls_intern_sym(ls_state_t *L, ls_package_t *pkg, const char *name, size_t namelen, int *is_external);
ls_package_t *ls_find_package(ls_state_t *L, const char *name, size_t namelen);
ls_package_t *ls_ensure_package(ls_state_t *L, const char *name);

/* ------- Hashtable ------- */
ls_hashtable_t *ls_hash_new(ls_state_t *L, ls_hash_test_t test, size_t cap_hint);
ls_value_t      ls_hash_get(ls_hashtable_t *h, ls_value_t k);
int             ls_hash_get_sv(ls_hashtable_t *h, ls_value_t k, ls_value_t *out);
void            ls_hash_put(ls_state_t *L, ls_hashtable_t *h, ls_value_t k, ls_value_t v);
int             ls_hash_remove(ls_hashtable_t *h, ls_value_t k);

/* Vector helpers. */
ls_vector_t *ls_vec_new(ls_state_t *L, size_t cap, int adjustable);
void         ls_vec_push(ls_state_t *L, ls_vector_t *v, ls_value_t x);

/* Standard list primitives -- inlined where possible. */
#define LS_CAR(v)  (((ls_cons_t*)(v).u.ptr)->car)
#define LS_CDR(v)  (((ls_cons_t*)(v).u.ptr)->cdr)
#define LS_CONSP(v) ((v).tag == LS_T_CONS)
#define LS_NILP(v)  ((v).tag == LS_T_NIL)

#endif /* LS_CORE_H */
