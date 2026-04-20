/* litesrpent.h -- Public API for Litesrpent, a Common Lisp in C.
 *
 * Litesrpent is a Common Lisp implementation with:
 *   - REPL / tree-walking interpreter
 *   - Bytecode compiler + VM (for "bytecode mode" / VM testing)
 *   - AOT compilation via transpile-to-C then invoke host C compiler
 *   - Hand-written x86-64 JIT for hot functions
 *   - FFI to .dll / .exe / .bin via LoadLibrary / GetProcAddress
 *   - Win32 GUI bindings (CreateWindowEx, message loop)
 *   - OpenGL bindings via opengl32.dll + wglGetProcAddress
 *   - Vulkan bindings via vulkan-1.dll
 *   - ELF writer for cross-producing Linux executables from Windows
 *   - CLOS, condition system, packages, format, loop
 *
 * This header is the only public-facing header.  All internal headers
 * live under include/ls*.h and should not be included directly by
 * downstream code that embeds Litesrpent.
 */
#ifndef LITESRPENT_H
#define LITESRPENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LITESRPENT_VERSION_MAJOR 0
#define LITESRPENT_VERSION_MINOR 1
#define LITESRPENT_VERSION_PATCH 0
#define LITESRPENT_VERSION_STRING "0.1.0"

/* Opaque state.  One ls_state_t corresponds to one Lisp image. */
typedef struct ls_state ls_state_t;

/* Public value handle.  This is a 16-byte tagged union; callers
 * should treat it as opaque and use the accessors below.  Passing
 * ls_value_t around by value is cheap and safe. */
typedef struct ls_value {
    uint32_t tag;
    uint32_t flags;      /* reserved for GC marks etc. */
    union {
        int64_t fixnum;
        double  flonum;
        uint32_t character;
        void    *ptr;
    } u;
} ls_value_t;

/* -------- Lifecycle -------- */
ls_state_t *ls_new(void);
void ls_free(ls_state_t *L);

/* -------- Top-level evaluation -------- */
int ls_eval_string(ls_state_t *L, const char *src, ls_value_t *out);
int ls_eval_file(ls_state_t *L, const char *path, ls_value_t *out);
int ls_load_file(ls_state_t *L, const char *path);

/* REPL convenience: runs an interactive read-eval-print loop against
 * stdin/stdout until EOF or (quit). */
int ls_repl(ls_state_t *L);

/* -------- Compile modes --------
 * ls_compile_to_bytecode writes a .lsb bytecode file that can be
 * executed by `litesrpent --bytecode file.lsb`.
 *
 * ls_compile_to_exe transpiles to C and invokes the host C compiler
 * to produce a native .exe on Windows (or .elf on Linux, or an
 * emulated ELF on Windows when target is "elf").
 *
 * ls_jit_compile is called implicitly when a bytecode function's
 * call count exceeds the JIT threshold, but can be called manually
 * to force JIT compilation of a given function.
 */
typedef enum ls_target {
    LS_TARGET_NATIVE_EXE,   /* native .exe on Windows, ELF on Linux */
    LS_TARGET_PE32PLUS,     /* force Windows PE32+ output */
    LS_TARGET_ELF64,        /* force ELF64 output (emulated on Windows) */
    LS_TARGET_BYTECODE      /* write .lsb bytecode image */
} ls_target_t;

int ls_compile_file(ls_state_t *L,
                    const char *src_path,
                    const char *out_path,
                    ls_target_t target);

int ls_compile_string(ls_state_t *L,
                      const char *source,
                      const char *out_path,
                      ls_target_t target);

int ls_jit_function(ls_state_t *L, ls_value_t fn);

/* -------- Value accessors -------- */
int        ls_is_nil     (ls_value_t v);
int        ls_is_t       (ls_value_t v);
int        ls_is_fixnum  (ls_value_t v);
int        ls_is_flonum  (ls_value_t v);
int        ls_is_number  (ls_value_t v);
int        ls_is_string  (ls_value_t v);
int        ls_is_symbol  (ls_value_t v);
int        ls_is_cons    (ls_value_t v);
int        ls_is_list    (ls_value_t v);
int        ls_is_vector  (ls_value_t v);
int        ls_is_hash    (ls_value_t v);
int        ls_is_fn      (ls_value_t v);

int64_t     ls_to_fixnum (ls_value_t v);
double      ls_to_flonum (ls_value_t v);
const char *ls_to_string (ls_value_t v, size_t *len_out);

ls_value_t ls_nil        (void);
ls_value_t ls_t          (void);
ls_value_t ls_make_fixnum(int64_t x);
ls_value_t ls_make_flonum(double x);
ls_value_t ls_make_char  (uint32_t cp);
ls_value_t ls_make_string(ls_state_t *L, const char *s, size_t n);
ls_value_t ls_make_symbol(ls_state_t *L, const char *name);
ls_value_t ls_cons       (ls_state_t *L, ls_value_t car, ls_value_t cdr);
ls_value_t ls_car        (ls_value_t pair);
ls_value_t ls_cdr        (ls_value_t pair);

/* -------- Global / package operations -------- */
ls_value_t ls_intern     (ls_state_t *L, const char *pkg, const char *name);
ls_value_t ls_getvar     (ls_state_t *L, ls_value_t sym);
void       ls_setvar     (ls_state_t *L, ls_value_t sym, ls_value_t val);

/* -------- Native callbacks -------- */
typedef ls_value_t (*ls_native_fn)(ls_state_t *L, int nargs, ls_value_t *args);
void ls_defun(ls_state_t *L, const char *pkg, const char *name,
              ls_native_fn fn, int min_args, int max_args);

/* -------- Errors / conditions -------- */
const char *ls_last_error(ls_state_t *L);
int ls_signal(ls_state_t *L, const char *type, const char *fmt, ...);

/* -------- FFI handle -------- */
ls_value_t ls_ffi_open   (ls_state_t *L, const char *path);
ls_value_t ls_ffi_sym    (ls_state_t *L, ls_value_t lib, const char *name);
int        ls_ffi_close  (ls_state_t *L, ls_value_t lib);

#ifdef __cplusplus
}
#endif
#endif /* LITESRPENT_H */
