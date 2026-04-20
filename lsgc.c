/* lsgc.c -- mark-and-sweep garbage collector.
 *
 * The GC walks every live ls_value_t starting from a small set of
 * roots (state pointers, escape frames, dyn frames), marks each
 * reachable heap object, then sweeps the all-objects list to free
 * anything not marked.  It is stop-the-world, precise on the heap,
 * and conservative only with respect to C local variables (which
 * we do NOT scan -- instead we require call sites that need GC
 * safety to use ls_gc_push_root).
 *
 * For Litesrpent's use case (a Lisp REPL + compiler) this is plenty. */

#include "lscore.h"

#define LS_MAX_ROOT_SLOTS 256

static ls_value_t **g_root_slots;
static size_t       g_root_count;
static size_t       g_root_cap;

void ls_gc_push_root(ls_state_t *L, ls_value_t *slot) {
    (void)L;
    if (g_root_count == g_root_cap) {
        size_t nc = g_root_cap ? g_root_cap * 2 : 64;
        g_root_slots = (ls_value_t **)realloc(g_root_slots, nc * sizeof(*g_root_slots));
        g_root_cap = nc;
    }
    g_root_slots[g_root_count++] = slot;
}
void ls_gc_pop_root(ls_state_t *L) { (void)L; if (g_root_count) g_root_count--; }

/* Gray-stack based marker. */
static void gray_push(ls_gc_t *g, ls_obj_header_t *h) {
    if (!h || (h->flags & LS_GC_MARK)) return;
    h->flags |= LS_GC_MARK;
    if (g->gray_count == g->gray_cap) {
        size_t nc = g->gray_cap ? g->gray_cap * 2 : 256;
        g->gray_stack = (ls_value_t *)realloc(g->gray_stack, nc * sizeof(ls_value_t));
        g->gray_cap = nc;
    }
    ls_value_t v; memset(&v, 0, sizeof v);
    v.tag = h->tag;
    v.u.ptr = h;
    g->gray_stack[g->gray_count++] = v;
}

static void mark_value(ls_gc_t *g, ls_value_t v) {
    switch (v.tag) {
    case LS_T_NIL: case LS_T_T: case LS_T_FIXNUM:
    case LS_T_FLONUM: case LS_T_CHAR: case LS_T_SPECIAL:
        return;
    default:
        if (v.u.ptr) gray_push(g, (ls_obj_header_t *)v.u.ptr);
    }
}

static void trace_object(ls_gc_t *g, ls_obj_header_t *h) {
    switch (h->tag) {
    case LS_T_CONS: {
        ls_cons_t *c = (ls_cons_t *)h;
        mark_value(g, c->car); mark_value(g, c->cdr);
        break;
    }
    case LS_T_SYMBOL: {
        ls_symbol_t *s = (ls_symbol_t *)h;
        if (s->name)    gray_push(g, &s->name->h);
        if (s->package) gray_push(g, &s->package->h);
        mark_value(g, s->value);
        mark_value(g, s->function);
        mark_value(g, s->plist);
        break;
    }
    case LS_T_STRING: break;
    case LS_T_VECTOR: {
        ls_vector_t *v = (ls_vector_t *)h;
        for (size_t i = 0; i < v->len; i++) mark_value(g, v->data[i]);
        break;
    }
    case LS_T_HASHTABLE: {
        ls_hashtable_t *ht = (ls_hashtable_t *)h;
        for (size_t i = 0; i < ht->cap; i++) {
            if (ht->entries[i].hash) {
                mark_value(g, ht->entries[i].key);
                mark_value(g, ht->entries[i].val);
            }
        }
        break;
    }
    case LS_T_FUNCTION: {
        ls_function_t *f = (ls_function_t *)h;
        mark_value(g, f->lambda_list);
        mark_value(g, f->body);
        mark_value(g, f->docstring);
        if (f->closure) gray_push(g, &f->closure->h);
        if (f->name) gray_push(g, &f->name->h);
        break;
    }
    case LS_T_BYTECODE: {
        ls_bytecode_fn_t *f = (ls_bytecode_fn_t *)h;
        if (f->proto) gray_push(g, &f->proto->h);
        for (uint32_t i = 0; i < f->nupvals; i++) mark_value(g, f->upvals[i]);
        if (f->name) gray_push(g, &f->name->h);
        break;
    }
    case LS_T_PACKAGE: {
        ls_package_t *p = (ls_package_t *)h;
        if (p->name) gray_push(g, &p->name->h);
        if (p->internal) gray_push(g, &p->internal->h);
        if (p->external) gray_push(g, &p->external->h);
        mark_value(g, p->use_list);
        mark_value(g, p->nicknames);
        break;
    }
    case LS_T_STREAM: break;
    case LS_T_FOREIGN: break;
    case LS_T_FOREIGN_LIB: {
        ls_foreign_lib_t *lib = (ls_foreign_lib_t *)h;
        if (lib->path) gray_push(g, &lib->path->h);
        break;
    }
    case LS_T_FOREIGN_FN: {
        ls_foreign_fn_t *fn = (ls_foreign_fn_t *)h;
        if (fn->lib) gray_push(g, &fn->lib->h);
        if (fn->name) gray_push(g, &fn->name->h);
        break;
    }
    case LS_T_CLASS: {
        ls_class_t *c = (ls_class_t *)h;
        if (c->name) gray_push(g, &c->name->h);
        mark_value(g, c->direct_supers);
        mark_value(g, c->direct_slots);
        mark_value(g, c->precedence_list);
        mark_value(g, c->effective_slots);
        mark_value(g, c->metaclass);
        if (c->slot_index) gray_push(g, &c->slot_index->h);
        break;
    }
    case LS_T_INSTANCE: {
        ls_instance_t *in = (ls_instance_t *)h;
        if (in->class_) gray_push(g, &in->class_->h);
        if (in->slots)
            for (uint32_t i = 0; i < in->class_->n_slots; i++)
                mark_value(g, in->slots[i]);
        break;
    }
    case LS_T_GENERIC: {
        ls_generic_t *gf = (ls_generic_t *)h;
        if (gf->name) gray_push(g, &gf->name->h);
        mark_value(g, gf->lambda_list);
        mark_value(g, gf->methods);
        mark_value(g, gf->combination);
        break;
    }
    case LS_T_METHOD: {
        ls_method_t *m = (ls_method_t *)h;
        mark_value(g, m->specializers);
        mark_value(g, m->qualifiers);
        mark_value(g, m->lambda);
        mark_value(g, m->generic);
        break;
    }
    case LS_T_CONDITION: {
        ls_condition_t *c = (ls_condition_t *)h;
        if (c->class_) gray_push(g, &c->class_->h);
        mark_value(g, c->initargs);
        mark_value(g, c->format_control);
        mark_value(g, c->format_arguments);
        break;
    }
    default: break;
    }
}

static void mark_roots(ls_state_t *L) {
    ls_gc_t *g = &L->gc;
    if (L->genv)    gray_push(g, &L->genv->h);
    if (L->packages) gray_push(g, &L->packages->h);
    if (L->pkg_cl)       gray_push(g, &L->pkg_cl->h);
    if (L->pkg_cl_user)  gray_push(g, &L->pkg_cl_user->h);
    if (L->pkg_keyword)  gray_push(g, &L->pkg_keyword->h);
    if (L->pkg_system)   gray_push(g, &L->pkg_system->h);
    if (L->current_package) gray_push(g, &L->current_package->h);
    if (L->symbol_cache) gray_push(g, &L->symbol_cache->h);
    if (L->stdin_)  gray_push(g, &L->stdin_->h);
    if (L->stdout_) gray_push(g, &L->stdout_->h);
    if (L->stderr_) gray_push(g, &L->stderr_->h);
    /* cached symbols */
#define ROOT_SYM(f) if (L->f) gray_push(g, &L->f->h)
    ROOT_SYM(sym_quote); ROOT_SYM(sym_quasiquote); ROOT_SYM(sym_unquote);
    ROOT_SYM(sym_unquote_splicing); ROOT_SYM(sym_function); ROOT_SYM(sym_lambda);
    ROOT_SYM(sym_if); ROOT_SYM(sym_let); ROOT_SYM(sym_letstar); ROOT_SYM(sym_progn);
    ROOT_SYM(sym_setq); ROOT_SYM(sym_defun); ROOT_SYM(sym_defmacro);
    ROOT_SYM(sym_defvar); ROOT_SYM(sym_defparameter); ROOT_SYM(sym_defconstant);
    ROOT_SYM(sym_defgeneric); ROOT_SYM(sym_defmethod); ROOT_SYM(sym_defclass);
    ROOT_SYM(sym_block); ROOT_SYM(sym_return_from); ROOT_SYM(sym_tagbody); ROOT_SYM(sym_go);
    ROOT_SYM(sym_catch); ROOT_SYM(sym_throw); ROOT_SYM(sym_unwind_protect);
    ROOT_SYM(sym_handler_case); ROOT_SYM(sym_handler_bind);
    ROOT_SYM(sym_restart_case); ROOT_SYM(sym_restart_bind);
    ROOT_SYM(sym_multiple_value_bind); ROOT_SYM(sym_multiple_value_call);
    ROOT_SYM(sym_multiple_value_prog1); ROOT_SYM(sym_the); ROOT_SYM(sym_declare);
    ROOT_SYM(sym_eval_when); ROOT_SYM(sym_flet); ROOT_SYM(sym_labels);
    ROOT_SYM(sym_macrolet); ROOT_SYM(sym_symbol_macrolet);
    ROOT_SYM(sym_load_time_value); ROOT_SYM(sym_locally);
    ROOT_SYM(sym_and); ROOT_SYM(sym_or); ROOT_SYM(sym_when); ROOT_SYM(sym_unless);
    ROOT_SYM(sym_cond); ROOT_SYM(sym_case); ROOT_SYM(sym_typecase);
    ROOT_SYM(sym_ampersand_rest); ROOT_SYM(sym_ampersand_optional);
    ROOT_SYM(sym_ampersand_key); ROOT_SYM(sym_ampersand_aux);
    ROOT_SYM(sym_ampersand_body); ROOT_SYM(sym_ampersand_allow_other_keys);
    ROOT_SYM(sym_ampersand_whole); ROOT_SYM(sym_ampersand_environment);
    ROOT_SYM(sym_otherwise); ROOT_SYM(sym_t);
#undef ROOT_SYM

    /* classes */
#define ROOT_CL(f) if (L->f) gray_push(g, &L->f->h)
    ROOT_CL(class_t); ROOT_CL(class_standard_object); ROOT_CL(class_cons);
    ROOT_CL(class_symbol); ROOT_CL(class_string); ROOT_CL(class_number);
    ROOT_CL(class_integer); ROOT_CL(class_fixnum); ROOT_CL(class_float);
    ROOT_CL(class_vector); ROOT_CL(class_hashtable); ROOT_CL(class_function);
    ROOT_CL(class_condition); ROOT_CL(class_error); ROOT_CL(class_simple_error);
    ROOT_CL(class_type_error); ROOT_CL(class_arith_error);
    ROOT_CL(class_undefined_function); ROOT_CL(class_unbound_variable);
#undef ROOT_CL

    /* escape/handler/dynamic bindings */
    for (ls_dynframe_t *d = L->dyn_top; d; d = d->next) {
        if (d->sym) gray_push(g, &d->sym->h);
        mark_value(g, d->old_val);
    }
    for (ls_handler_t *h = L->hnd_top; h; h = h->next) {
        if (h->type) gray_push(g, &h->type->h);
        mark_value(g, h->fn);
    }
    for (ls_restart_frame_t *r = L->restart_top; r; r = r->next) {
        if (r->restart) gray_push(g, &r->restart->h);
    }
    for (ls_escape_t *e = L->esc_top; e; e = e->next) {
        mark_value(g, e->tag);
        mark_value(g, e->value);
    }
    /* multiple values */
    for (int i = 0; i < L->mv.n; i++) mark_value(g, L->mv.v[i]);

    /* user-pushed roots */
    for (size_t i = 0; i < g_root_count; i++) mark_value(g, *g_root_slots[i]);
}

static void sweep(ls_state_t *L) {
    ls_gc_t *g = &L->gc;
    ls_obj_header_t **pp = &g->all_objects;
    while (*pp) {
        ls_obj_header_t *h = *pp;
        if ((h->flags & LS_GC_MARK) || (h->flags & LS_GC_PINNED)) {
            h->flags &= ~LS_GC_MARK;
            pp = &h->next;
        } else {
            *pp = h->next;
            /* free any owned internal buffers */
            switch (h->tag) {
            case LS_T_STRING:    free(((ls_string_t *)h)->chars); break;
            case LS_T_VECTOR:    free(((ls_vector_t *)h)->data); break;
            case LS_T_HASHTABLE: free(((ls_hashtable_t *)h)->entries); break;
            case LS_T_INSTANCE:  free(((ls_instance_t *)h)->slots); break;
            case LS_T_ARRAY:     free(((ls_array_t *)h)->dims); free(((ls_array_t *)h)->data); break;
            case LS_T_STREAM: {
                ls_stream_t *s = (ls_stream_t *)h;
                if (s->owns_fp && s->fp) fclose(s->fp);
                free(s->buffer);
                break;
            }
            case LS_T_FOREIGN: {
                ls_foreign_t *f = (ls_foreign_t *)h;
                if (f->owned && f->ptr) free(f->ptr);
                break;
            }
            default: break;
            }
            g->bytes_allocated -= h->size;
            free(h);
        }
    }
}

void ls_gc_collect(ls_state_t *L) {
    if (L->gc.disabled) return;
    L->gc.gray_count = 0;
    mark_roots(L);
    while (L->gc.gray_count) {
        ls_value_t v = L->gc.gray_stack[--L->gc.gray_count];
        trace_object(&L->gc, (ls_obj_header_t *)v.u.ptr);
    }
    sweep(L);
    L->gc.next_gc = L->gc.bytes_allocated * 2 + (1u << 20);
}
