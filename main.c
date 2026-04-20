/* main.c -- Litesrpent entry point.
 *
 * Usage:
 *   litesrpent                    -- start REPL
 *   litesrpent file.lisp          -- load and execute file
 *   litesrpent --compile file.lisp -o out.exe [--target native|pe|elf|bytecode]
 *   litesrpent --bytecode file.lsb -- run bytecode image
 *   litesrpent --eval "(expr)"    -- evaluate expression
 *   litesrpent --version          -- print version
 */
#include "lscore.h"
#include "lseval.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Forward declarations for init functions in each module. */
extern void ls_register_builtins(ls_state_t *L);
extern void ls_init_clos(ls_state_t *L);
extern void ls_init_vm(ls_state_t *L);
extern void ls_init_aot(ls_state_t *L);
extern void ls_init_jit(ls_state_t *L);
extern void ls_init_ffi(ls_state_t *L);
extern void ls_init_gui(ls_state_t *L);
extern void ls_init_gl(ls_state_t *L);
extern void ls_init_vk(ls_state_t *L);
extern void ls_init_elf(ls_state_t *L);

/* ---- State initialization ---- */

static ls_stream_t *make_file_stream(ls_state_t *L, FILE *fp, int dir) {
    ls_value_t v = ls_make_obj(L, LS_T_STREAM, sizeof(ls_stream_t));
    ls_stream_t *s = (ls_stream_t *)v.u.ptr;
    s->fp = fp;
    s->direction = dir;
    s->owns_fp = 0;
    s->ungot_char = -1;
    return s;
}

static void intern_cached_symbols(ls_state_t *L) {
#define ISYM(field, pkg, name) do { \
        L->field = (ls_symbol_t*)ls_intern(L, pkg, name).u.ptr; \
        ls_export_symbol(L, L->field->package, L->field); \
    } while(0)
    ISYM(sym_quote,       "COMMON-LISP", "QUOTE");
    ISYM(sym_quasiquote,  "COMMON-LISP", "QUASIQUOTE");
    ISYM(sym_unquote,     "COMMON-LISP", "UNQUOTE");
    ISYM(sym_unquote_splicing, "COMMON-LISP", "UNQUOTE-SPLICING");
    ISYM(sym_function,    "COMMON-LISP", "FUNCTION");
    ISYM(sym_lambda,      "COMMON-LISP", "LAMBDA");
    ISYM(sym_if,          "COMMON-LISP", "IF");
    ISYM(sym_let,         "COMMON-LISP", "LET");
    ISYM(sym_letstar,     "COMMON-LISP", "LET*");
    ISYM(sym_progn,       "COMMON-LISP", "PROGN");
    ISYM(sym_setq,        "COMMON-LISP", "SETQ");
    ISYM(sym_defun,       "COMMON-LISP", "DEFUN");
    ISYM(sym_defmacro,    "COMMON-LISP", "DEFMACRO");
    ISYM(sym_defvar,      "COMMON-LISP", "DEFVAR");
    ISYM(sym_defparameter,"COMMON-LISP", "DEFPARAMETER");
    ISYM(sym_defconstant, "COMMON-LISP", "DEFCONSTANT");
    ISYM(sym_defgeneric,  "COMMON-LISP", "DEFGENERIC");
    ISYM(sym_defmethod,   "COMMON-LISP", "DEFMETHOD");
    ISYM(sym_defclass,    "COMMON-LISP", "DEFCLASS");
    ISYM(sym_block,       "COMMON-LISP", "BLOCK");
    ISYM(sym_return_from, "COMMON-LISP", "RETURN-FROM");
    ISYM(sym_tagbody,     "COMMON-LISP", "TAGBODY");
    ISYM(sym_go,          "COMMON-LISP", "GO");
    ISYM(sym_catch,       "COMMON-LISP", "CATCH");
    ISYM(sym_throw,       "COMMON-LISP", "THROW");
    ISYM(sym_unwind_protect, "COMMON-LISP", "UNWIND-PROTECT");
    ISYM(sym_handler_case,"COMMON-LISP", "HANDLER-CASE");
    ISYM(sym_handler_bind,"COMMON-LISP", "HANDLER-BIND");
    ISYM(sym_restart_case, "COMMON-LISP", "RESTART-CASE");
    ISYM(sym_restart_bind, "COMMON-LISP", "RESTART-BIND");
    ISYM(sym_multiple_value_bind, "COMMON-LISP", "MULTIPLE-VALUE-BIND");
    ISYM(sym_multiple_value_call, "COMMON-LISP", "MULTIPLE-VALUE-CALL");
    ISYM(sym_multiple_value_prog1, "COMMON-LISP", "MULTIPLE-VALUE-PROG1");
    ISYM(sym_the,         "COMMON-LISP", "THE");
    ISYM(sym_declare,     "COMMON-LISP", "DECLARE");
    ISYM(sym_eval_when,   "COMMON-LISP", "EVAL-WHEN");
    ISYM(sym_flet,        "COMMON-LISP", "FLET");
    ISYM(sym_labels,      "COMMON-LISP", "LABELS");
    ISYM(sym_macrolet,    "COMMON-LISP", "MACROLET");
    ISYM(sym_symbol_macrolet, "COMMON-LISP", "SYMBOL-MACROLET");
    ISYM(sym_load_time_value, "COMMON-LISP", "LOAD-TIME-VALUE");
    ISYM(sym_locally,     "COMMON-LISP", "LOCALLY");
    ISYM(sym_and,         "COMMON-LISP", "AND");
    ISYM(sym_or,          "COMMON-LISP", "OR");
    ISYM(sym_when,        "COMMON-LISP", "WHEN");
    ISYM(sym_unless,      "COMMON-LISP", "UNLESS");
    ISYM(sym_cond,        "COMMON-LISP", "COND");
    ISYM(sym_case,        "COMMON-LISP", "CASE");
    ISYM(sym_typecase,    "COMMON-LISP", "TYPECASE");
    ISYM(sym_ampersand_rest,     "COMMON-LISP", "&REST");
    ISYM(sym_ampersand_optional, "COMMON-LISP", "&OPTIONAL");
    ISYM(sym_ampersand_key,      "COMMON-LISP", "&KEY");
    ISYM(sym_ampersand_aux,      "COMMON-LISP", "&AUX");
    ISYM(sym_ampersand_body,     "COMMON-LISP", "&BODY");
    ISYM(sym_ampersand_allow_other_keys, "COMMON-LISP", "&ALLOW-OTHER-KEYS");
    ISYM(sym_ampersand_whole,    "COMMON-LISP", "&WHOLE");
    ISYM(sym_ampersand_environment, "COMMON-LISP", "&ENVIRONMENT");
    ISYM(sym_otherwise,   "COMMON-LISP", "OTHERWISE");
    ISYM(sym_t,           "COMMON-LISP", "T");
    ISYM(sym_do,          "COMMON-LISP", "DO");
    ISYM(sym_dostar,      "COMMON-LISP", "DO*");
    ISYM(sym_return,      "COMMON-LISP", "RETURN");
    ISYM(sym_setf,        "COMMON-LISP", "SETF");
    ISYM(sym_prog,        "COMMON-LISP", "PROG");
    ISYM(sym_progstar,    "COMMON-LISP", "PROG*");
    ISYM(sym_dotimes,     "COMMON-LISP", "DOTIMES");
    ISYM(sym_dolist,      "COMMON-LISP", "DOLIST");
    ISYM(sym_car,         "COMMON-LISP", "CAR");
    ISYM(sym_cdr,         "COMMON-LISP", "CDR");
    ISYM(sym_first,       "COMMON-LISP", "FIRST");
    ISYM(sym_rest,        "COMMON-LISP", "REST");
    ISYM(sym_nth,         "COMMON-LISP", "NTH");
    ISYM(sym_aref,        "COMMON-LISP", "AREF");
    ISYM(sym_gethash,     "COMMON-LISP", "GETHASH");
    ISYM(sym_slot_value,  "COMMON-LISP", "SLOT-VALUE");
    ISYM(sym_symbol_value,"COMMON-LISP", "SYMBOL-VALUE");
    ISYM(sym_symbol_function,"COMMON-LISP", "SYMBOL-FUNCTION");
#undef ISYM
    /* Set T's value to itself. */
    L->sym_t->value = ls_t_v();
    L->sym_t->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_CONSTANT;
}

ls_state_t *ls_new(void) {
    ls_state_t *L = (ls_state_t *)calloc(1, sizeof(ls_state_t));
    if (!L) return NULL;
    L->gc.next_gc = 1 << 20;

    /* Create core packages. */
    L->packages = ls_hash_new(L, LS_HASH_EQUAL, 32);
    L->pkg_cl      = ls_ensure_package(L, "COMMON-LISP");
    L->pkg_cl_user = ls_ensure_package(L, "COMMON-LISP-USER");
    L->pkg_keyword = ls_ensure_package(L, "KEYWORD");
    L->pkg_system  = ls_ensure_package(L, "LITESRPENT-SYSTEM");
    ls_ensure_package(L, "LITESRPENT-GL");
    ls_ensure_package(L, "LITESRPENT-VK");
    ls_ensure_package(L, "LITESRPENT-GUI");

    /* Set up USE-list for CL-USER. */
    L->pkg_cl_user->use_list = ls_cons(L, ls_wrap(LS_T_PACKAGE, L->pkg_cl), ls_nil_v());
    /* LITESRPENT-SYSTEM also uses CL */
    L->pkg_system->use_list = ls_cons(L, ls_wrap(LS_T_PACKAGE, L->pkg_cl), ls_nil_v());

    L->current_package = L->pkg_cl_user;
    L->genv = ls_env_new(L, NULL);
    L->symbol_cache = ls_hash_new(L, LS_HASH_EQUAL, 256);

    /* Standard streams. */
    L->stdin_  = make_file_stream(L, stdin, 1);
    L->stdout_ = make_file_stream(L, stdout, 2);
    L->stderr_ = make_file_stream(L, stderr, 2);

    /* Multiple values. */
    L->mv.n = 0;

    /* Cached symbols. */
    intern_cached_symbols(L);

    /* Config. */
    L->jit_enabled = 1;
    L->jit_threshold = 100;
    L->verbose = 0;

    /* Register all built-in functions. */
    ls_register_builtins(L);
    ls_init_clos(L);
    ls_init_vm(L);
    ls_init_aot(L);
    ls_init_jit(L);
    ls_init_ffi(L);
    ls_init_gui(L);
    ls_init_gl(L);
    ls_init_vk(L);
    ls_init_elf(L);

    /* Set *package* etc. */
    ls_value_t pkg_sym = ls_intern(L, "COMMON-LISP", "*PACKAGE*");
    ls_symbol_t *ps = (ls_symbol_t *)pkg_sym.u.ptr;
    ps->value = ls_wrap(LS_T_PACKAGE, L->pkg_cl_user);
    ps->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_SPECIAL;

    /* *standard-input/output/error* */
    ls_value_t sin_sym = ls_intern(L, "COMMON-LISP", "*STANDARD-INPUT*");
    ((ls_symbol_t*)sin_sym.u.ptr)->value = ls_wrap(LS_T_STREAM, L->stdin_);
    ((ls_symbol_t*)sin_sym.u.ptr)->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_SPECIAL;
    ls_value_t sout_sym = ls_intern(L, "COMMON-LISP", "*STANDARD-OUTPUT*");
    ((ls_symbol_t*)sout_sym.u.ptr)->value = ls_wrap(LS_T_STREAM, L->stdout_);
    ((ls_symbol_t*)sout_sym.u.ptr)->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_SPECIAL;

    /* *features* */
    ls_value_t feat_sym = ls_intern(L, "COMMON-LISP", "*FEATURES*");
    ls_value_t flist = ls_nil_v();
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "LITESRPENT"), flist);
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "COMMON-LISP"), flist);
#ifdef _WIN32
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "WIN32"), flist);
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "WINDOWS"), flist);
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "X86-64"), flist);
#else
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "LINUX"), flist);
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "UNIX"), flist);
    flist = ls_cons(L, ls_intern(L, "KEYWORD", "X86-64"), flist);
#endif
    ((ls_symbol_t*)feat_sym.u.ptr)->value = flist;
    ((ls_symbol_t*)feat_sym.u.ptr)->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_SPECIAL;

    return L;
}

void ls_free(ls_state_t *L) {
    if (!L) return;
    /* Free all heap objects. */
    ls_obj_header_t *h = L->gc.all_objects;
    while (h) {
        ls_obj_header_t *next = h->next;
        switch (h->tag) {
        case LS_T_STRING:    free(((ls_string_t *)h)->chars); break;
        case LS_T_VECTOR:    free(((ls_vector_t *)h)->data); break;
        case LS_T_HASHTABLE: free(((ls_hashtable_t *)h)->entries); break;
        default: break;
        }
        free(h);
        h = next;
    }
    if (L->jit_arena) {
#ifdef _WIN32
        VirtualFree(L->jit_arena, 0, MEM_RELEASE);
#else
        munmap(L->jit_arena, L->jit_arena_cap);
#endif
    }
    free(L->cc_path);
    free(L);
}

/* ---- File evaluation ---- */

int ls_eval_file(ls_state_t *L, const char *path, ls_value_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) { ls_error(L, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, len, f);
    buf[len] = 0;
    fclose(f);

    int ret = ls_eval_string(L, buf, out);
    free(buf);
    return ret;
}

int ls_eval_string(ls_state_t *L, const char *src, ls_value_t *out) {
    const char *p = src;
    ls_value_t last = ls_nil_v();
    ls_escape_t esc; memset(&esc, 0, sizeof esc);
    esc.kind = 99; /* top-level error catcher */
    esc.next = L->esc_top;
    L->esc_top = &esc;

    if (setjmp(esc.buf) != 0) {
        L->esc_top = esc.next;
        fprintf(stderr, "Error: %s\n", L->err_buf);
        if (out) *out = ls_nil_v();
        return -1;
    }

    while (*p) {
        /* Skip whitespace and comments. */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (!*p) break;
        if (*p == ';') { while (*p && *p != '\n') p++; continue; }
        const char *end = NULL;
        ls_value_t form = ls_read_from_string(L, p, &end);
        if (!end || end == p) break;
        p = end;
        last = ls_eval(L, form, L->genv);
    }

    L->esc_top = esc.next;
    if (out) *out = last;
    return 0;
}

int ls_load_file(ls_state_t *L, const char *path) {
    return ls_eval_file(L, path, NULL);
}

/* ---- REPL ---- */

/* Recompute paren depth / string state from scratch over the whole buffer.
 * Returns depth (positive = unclosed opens). Sets *in_str_out to current
 * string state. Properly handles ;-comments, \-escapes inside strings, and
 * #\( character literals. */
static int repl_depth(const char *buf, size_t len, int *in_str_out) {
    int depth = 0, in_str = 0;
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (in_str) {
            if (c == '\\' && i + 1 < len) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == ';') {
            /* skip to end of line */
            while (i < len && buf[i] != '\n') i++;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        /* #\X character literal -- skip the next character even if it's a paren */
        if (c == '#' && i + 2 < len && buf[i+1] == '\\') { i += 2; continue; }
        if (c == '(') depth++;
        else if (c == ')') depth--;
    }
    if (in_str_out) *in_str_out = in_str;
    return depth;
}

/* Read a single line of input, supporting Shift+Enter for embedded newlines
 * on Win32 console.  Falls back to plain fgets otherwise.  Returns 0 on EOF. */
#ifdef _WIN32
static int repl_read_line_win32(char *buf, size_t cap) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    /* Detect interactive console; use raw mode only when attached. */
    if (!GetConsoleMode(in, &mode)) {
        if (!fgets(buf, (int)cap, stdin)) return 0;
        return 1;
    }
    /* Switch to raw input so we can intercept Shift+Enter. */
    DWORD orig = mode;
    SetConsoleMode(in, 0);
    size_t p = 0;
    int finished = 0;
    while (!finished) {
        INPUT_RECORD ir;
        DWORD nread = 0;
        if (!ReadConsoleInputW(in, &ir, 1, &nread) || nread == 0) {
            SetConsoleMode(in, orig);
            return 0;
        }
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        WCHAR wc = ir.Event.KeyEvent.uChar.UnicodeChar;
        DWORD ctrl = ir.Event.KeyEvent.dwControlKeyState;
        int shift = (ctrl & SHIFT_PRESSED) != 0;

        if (vk == VK_RETURN) {
            if (shift) {
                /* Shift+Enter: insert literal newline, keep reading. */
                if (p + 1 < cap) { buf[p++] = '\n'; }
                fputs("\n  ", stdout); fflush(stdout);
                continue;
            }
            /* Plain Enter -- finish line. */
            fputs("\n", stdout); fflush(stdout);
            finished = 1;
            break;
        }
        if (vk == VK_BACK) {
            if (p > 0) {
                p--;
                /* erase from terminal */
                fputs("\b \b", stdout); fflush(stdout);
            }
            continue;
        }
        if (wc == 3) {  /* Ctrl-C */
            SetConsoleMode(in, orig);
            return 0;
        }
        if (wc == 4) {  /* Ctrl-D == EOF */
            if (p == 0) { SetConsoleMode(in, orig); return 0; }
            continue;
        }
        if (wc >= 32 && wc < 127) {
            if (p + 1 < cap) { buf[p++] = (char)wc; fputc((char)wc, stdout); fflush(stdout); }
        } else if (wc == '\t') {
            if (p + 1 < cap) { buf[p++] = '\t'; fputc('\t', stdout); fflush(stdout); }
        } else if (wc > 127) {
            /* Encode as UTF-8 (basic 2/3-byte). */
            char utf[4]; int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf, sizeof utf, NULL, NULL);
            for (int k = 0; k < n && p + 1 < cap; k++) buf[p++] = utf[k];
            fwrite(utf, 1, n, stdout); fflush(stdout);
        }
    }
    if (p + 1 < cap) buf[p++] = '\n';
    buf[p] = 0;
    SetConsoleMode(in, orig);
    return 1;
}
#endif

static int repl_read_line(char *buf, size_t cap) {
#ifdef _WIN32
    return repl_read_line_win32(buf, cap);
#else
    return fgets(buf, (int)cap, stdin) != NULL;
#endif
}

/* Try to interpret a non-paren input as a file path: if the file exists,
 * load it.  Returns 1 if handled, 0 if not. */
static int repl_try_load_path(ls_state_t *L, const char *line) {
    /* Trim whitespace */
    while (*line && (*line == ' ' || *line == '\t')) line++;
    if (!*line) return 0;
    if (line[0] == '(' || line[0] == '\'' || line[0] == ';' ||
        line[0] == '`' || line[0] == ',' || line[0] == ':' ||
        line[0] == '\"' || line[0] == '#') return 0;
    /* Strip trailing whitespace */
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t' ||
                     line[n-1] == '\n' || line[n-1] == '\r')) n--;
    if (n == 0) return 0;
    char path[1024];
    if (n >= sizeof path) return 0;
    memcpy(path, line, n); path[n] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    printf("; loading %s\n", path);
    int rc = ls_eval_file(L, path, NULL);
    if (rc != 0) printf("; load failed\n");
    return 1;
}

/* Mode switch for evaluation strategy. */
typedef enum { REPL_MODE_INTERP = 0, REPL_MODE_BYTECODE = 1, REPL_MODE_AOT = 2 } repl_mode_t;
static const char *repl_mode_name(repl_mode_t m) {
    switch (m) {
    case REPL_MODE_BYTECODE: return "BYTECODE";
    case REPL_MODE_AOT:      return "AOT";
    default:                 return "INTERP";
    }
}

/* Detect mode-switch directives like (bytecode), (aot), (interp), (mode).
 * If the form is one of these, perform the switch and return 1. */
static int repl_handle_mode_directive(ls_state_t *L, ls_value_t form, repl_mode_t *mode) {
    (void)L;
    if (form.tag != LS_T_CONS) return 0;
    ls_value_t hd = ls_car(form);
    if (hd.tag != LS_T_SYMBOL) return 0;
    if (ls_cdr(form).tag != LS_T_NIL) return 0;
    const char *nm = ((ls_symbol_t *)hd.u.ptr)->name->chars;
    if (strcmp(nm, "BYTECODE") == 0) {
        *mode = REPL_MODE_BYTECODE;
        printf("; switched to BYTECODE VM mode\n");
        return 1;
    }
    if (strcmp(nm, "AOT") == 0) {
        *mode = REPL_MODE_AOT;
        printf("; switched to AOT C-transpilation mode\n");
        return 1;
    }
    if (strcmp(nm, "INTERP") == 0 || strcmp(nm, "INTERPRET") == 0 ||
        strcmp(nm, "INTERPRETER") == 0) {
        *mode = REPL_MODE_INTERP;
        printf("; switched to INTERPRETER mode\n");
        return 1;
    }
    if (strcmp(nm, "MODE") == 0) {
        printf("; current mode: %s\n", repl_mode_name(*mode));
        return 1;
    }
    return 0;
}

/* Forward decls for VM/AOT entry points. */
extern ls_value_t ls_vm_compile_and_run(ls_state_t *L, ls_value_t form);
extern ls_value_t ls_aot_compile_and_run(ls_state_t *L, ls_value_t form);

static ls_value_t repl_eval_form(ls_state_t *L, ls_value_t form, repl_mode_t mode) {
    if (mode == REPL_MODE_BYTECODE) {
        return ls_vm_compile_and_run(L, form);
    }
    if (mode == REPL_MODE_AOT) {
        return ls_aot_compile_and_run(L, form);
    }
    return ls_eval(L, form, L->genv);
}

int ls_repl(ls_state_t *L) {
    printf("Litesrpent %s -- a Common Lisp implementation\n", LITESRPENT_VERSION_STRING);
    printf("  Interpreter | Bytecode VM | AOT | JIT | FFI | Win32 GUI | OpenGL | Vulkan\n");
    printf("  Type (quit) to exit, (bytecode), (aot), (interp) to switch mode.\n");
    printf("  Lines outside parens are treated as file paths to load.\n");
    printf("  Shift+Enter inserts a newline; trailing \\ also continues.\n\n");

    repl_mode_t mode = REPL_MODE_INTERP;
    char line[16384];
    for (;;) {
        const char *pkg_name = "CL";
        if (L->current_package && L->current_package->name) {
            const char *full = L->current_package->name->chars;
            if (strcmp(full, "COMMON-LISP") == 0) pkg_name = "CL";
            else if (strcmp(full, "COMMON-LISP-USER") == 0) pkg_name = "CL";
            else if (strcmp(full, "KEYWORD") == 0) pkg_name = "KW";
            else pkg_name = full;
        }
        const char *mode_tag = (mode == REPL_MODE_BYTECODE) ? "[bc]" :
                               (mode == REPL_MODE_AOT)      ? "[aot]" : "";
        printf("litesrpent-%s%s:> ", pkg_name, mode_tag);
        fflush(stdout);
        if (!repl_read_line(line, sizeof line)) break;

        /* Strip trailing CR/LF (not embedded ones from Shift+Enter). */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) continue;

        /* Trailing \ requests explicit continuation. */
        int explicit_continue = (len > 0 && line[len-1] == '\\');
        if (explicit_continue) line[--len] = 0;

        /* Compute paren state across the entire buffer (handles embedded \n). */
        int in_str = 0;
        int depth = repl_depth(line, len, &in_str);

        /* Multi-line continuation loop -- always recompute over whole buffer
         * so that adding ')' on its own line correctly closes the form. */
        while (depth > 0 || in_str || explicit_continue) {
            printf("litesrpent-%s%s..  ", pkg_name, mode_tag);
            fflush(stdout);
            char cont[8192];
            if (!repl_read_line(cont, sizeof cont)) { depth = 0; break; }
            size_t cl = strlen(cont);
            while (cl > 0 && (cont[cl-1] == '\n' || cont[cl-1] == '\r')) cont[--cl] = 0;
            int this_continue = (cl > 0 && cont[cl-1] == '\\');
            if (this_continue) cont[--cl] = 0;
            if (len + cl + 2 < sizeof line) {
                line[len++] = '\n';
                memcpy(line + len, cont, cl + 1);
                len += cl;
            }
            /* recompute over whole buffer so previous bookkeeping mistakes
             * don't get baked in -- this fixes the "blank line keeps me
             * trapped" bug. */
            depth = repl_depth(line, len, &in_str);
            explicit_continue = this_continue;
        }
        if (depth < 0) depth = 0;  /* tolerate stray ')' */

        /* If the input doesn't begin with a paren or quote, treat as file. */
        const char *trim = line;
        while (*trim && (*trim == ' ' || *trim == '\t' || *trim == '\n')) trim++;
        if (*trim && *trim != '(' && *trim != '\'' && *trim != '`' &&
            *trim != ';' && *trim != '\"' && *trim != '#' && *trim != ':' &&
            *trim != ',') {
            if (repl_try_load_path(L, trim)) continue;
            /* Otherwise, fall through to eval as symbol/atom. */
        }

        /* Evaluate. */
        ls_value_t result = ls_nil_v();
        ls_escape_t esc; memset(&esc, 0, sizeof esc);
        esc.kind = 99;
        esc.next = L->esc_top;
        L->esc_top = &esc;

        if (setjmp(esc.buf) != 0) {
            L->esc_top = esc.next;
            fprintf(stderr, "Error: %s\n", L->err_buf);
            continue;
        }

        const char *p = line;
        int printed_any = 0;
        while (*p) {
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (!*p) break;
            const char *end = NULL;
            ls_value_t form = ls_read_from_string(L, p, &end);
            if (!end || end == p) break;
            p = end;
            /* Mode-switch directives are handled at the REPL level. */
            if (repl_handle_mode_directive(L, form, &mode)) {
                printed_any = 1;
                result = ls_nil_v();
                continue;
            }
            result = repl_eval_form(L, form, mode);
            printed_any = 1;
        }
        L->esc_top = esc.next;

        if (printed_any) {
            ls_print_value(L, result, L->stdout_, 1);
            printf("\n");
        }

        if (L->gc.bytes_allocated > L->gc.next_gc) ls_gc_collect(L);
    }
    return 0;
}

/* ---- Compile mode ---- */

int ls_compile_file(ls_state_t *L, const char *src_path, const char *out_path, ls_target_t target) {
    (void)target;
    /* Delegate to AOT: defined in lsaot.c */
    extern int ls_aot_compile_file(ls_state_t *L, const char *src, const char *out, int tgt);
    return ls_aot_compile_file(L, src_path, out_path, (int)target);
}

int ls_compile_string(ls_state_t *L, const char *source, const char *out_path, ls_target_t target) {
    /* Write source to temp file, then compile. */
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp.lisp", out_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fputs(source, f);
    fclose(f);
    int r = ls_compile_file(L, tmp, out_path, target);
    remove(tmp);
    return r;
}

/* ---- Main ---- */

static void print_usage(void) {
    printf("Litesrpent %s -- Common Lisp implementation in C\n\n", LITESRPENT_VERSION_STRING);
    printf("Usage:\n");
    printf("  litesrpent                         Start REPL\n");
    printf("  litesrpent <file.lisp>             Load and execute file\n");
    printf("  litesrpent --eval \"(expr)\"         Evaluate expression\n");
    printf("  litesrpent --compile <file> -o <out> [--target native|pe|elf|bytecode]\n");
    printf("  litesrpent --bytecode <file.lsb>   Run bytecode image\n");
    printf("  litesrpent --version               Print version\n");
    printf("  litesrpent --verbose               Enable verbose output\n");
}

int main(int argc, char **argv) {
    ls_state_t *L = ls_new();
    if (!L) { fprintf(stderr, "Failed to initialize Litesrpent\n"); return 1; }

    /* Load standard library */
    {
        char stdlib_path[512];
        #ifdef _WIN32
        char exe_dir[512];
        GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
        char *sl = strrchr(exe_dir, '\\');
        if (sl) *sl = 0;
        snprintf(stdlib_path, sizeof stdlib_path, "%s\\..\\stdlib\\core.lisp", exe_dir);
        #else
        snprintf(stdlib_path, sizeof stdlib_path, "stdlib/core.lisp");
        #endif
        FILE *f = fopen(stdlib_path, "r");
        if (f) { fclose(f); ls_eval_file(L, stdlib_path, NULL); }
        else {
            f = fopen("stdlib/core.lisp", "r");
            if (f) { fclose(f); ls_eval_file(L, "stdlib/core.lisp", NULL); }
        }
    }

    /* Find compiler path. */
#ifdef _WIN32
    /* Check for bundled MinGW first */
    {
        char path[512];
        /* Get directory of this executable */
        GetModuleFileNameA(NULL, path, sizeof path);
        char *last_slash = strrchr(path, '\\');
        if (last_slash) *last_slash = 0;
        char gcc_path[512];
        snprintf(gcc_path, sizeof gcc_path, "%s\\..\\third_party\\mingw64\\bin\\gcc.exe", path);
        FILE *test = fopen(gcc_path, "rb");
        if (test) { fclose(test); L->cc_path = strdup(gcc_path); }
        else L->cc_path = strdup("gcc");
    }
#else
    L->cc_path = strdup("gcc");
#endif

    if (argc < 2) {
        ls_repl(L);
        ls_free(L);
        return 0;
    }

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("Litesrpent %s\n", LITESRPENT_VERSION_STRING);
            ls_free(L);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            ls_free(L);
            return 0;
        }
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            L->verbose = 1;
            i++; continue;
        }
        if (strcmp(argv[i], "--eval") == 0 || strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--eval needs an argument\n"); return 1; }
            ls_value_t result;
            ls_eval_string(L, argv[i+1], &result);
            ls_print_value(L, result, L->stdout_, 1);
            printf("\n");
            ls_free(L);
            return 0;
        }
        if (strcmp(argv[i], "--compile") == 0 || strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--compile needs source file\n"); return 1; }
            const char *src = argv[++i];
            const char *out = "a.exe";
            ls_target_t target = LS_TARGET_NATIVE_EXE;
            i++;
            while (i < argc) {
                if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out = argv[++i]; i++; }
                else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                    i++;
                    if (strcmp(argv[i], "pe") == 0) target = LS_TARGET_PE32PLUS;
                    else if (strcmp(argv[i], "elf") == 0) target = LS_TARGET_ELF64;
                    else if (strcmp(argv[i], "bytecode") == 0) target = LS_TARGET_BYTECODE;
                    i++;
                } else break;
            }
            int r = ls_compile_file(L, src, out, target);
            if (r == 0) printf("Compiled %s -> %s\n", src, out);
            else fprintf(stderr, "Compilation failed\n");
            ls_free(L);
            return r;
        }
        /* Default: treat as filename to execute. */
        {
            ls_value_t result;
            int r = ls_eval_file(L, argv[i], &result);
            if (r != 0) {
                fprintf(stderr, "Error loading %s\n", argv[i]);
                ls_free(L);
                return 1;
            }
            i++;
        }
    }

    ls_free(L);
    return 0;
}
