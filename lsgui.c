/* lsgui.c -- Win32 GUI bindings for Litesrpent.
 *
 * Provides Lisp-level window creation, child controls, message pump,
 * and callback dispatch.  Uses the Win32 API directly (linked via
 * -luser32 -lgdi32 -lkernel32).
 *
 * Registered by ls_init_gui(). */
#include "lscore.h"
#include "lseval.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ============================================================
 *  Internal state
 * ============================================================ */
static const char *LS_WND_CLASS  = "LitesrpentWin";
static int         ls_gui_inited = 0;

/* Per-window callback storage.  We keep a small table mapping HWND ->
 * (ls_state_t*, ls_value_t callback).  Stored as a flat array since
 * most programs will have < 64 windows. */
#define LS_MAX_GUI_WINDOWS 256

typedef struct {
    HWND        hwnd;
    ls_state_t *L;
    ls_value_t  callback;    /* Lisp function or NIL */
} ls_gui_entry_t;

static ls_gui_entry_t gui_table[LS_MAX_GUI_WINDOWS];
static int            gui_table_count = 0;

static ls_gui_entry_t *gui_find(HWND hwnd) {
    for (int i = 0; i < gui_table_count; i++)
        if (gui_table[i].hwnd == hwnd) return &gui_table[i];
    return NULL;
}

static void gui_register(HWND hwnd, ls_state_t *L) {
    if (gui_table_count >= LS_MAX_GUI_WINDOWS) return;
    ls_gui_entry_t *e = &gui_table[gui_table_count++];
    e->hwnd     = hwnd;
    e->L        = L;
    e->callback = ls_nil_v();
}

static void gui_unregister(HWND hwnd) {
    for (int i = 0; i < gui_table_count; i++) {
        if (gui_table[i].hwnd == hwnd) {
            gui_table[i] = gui_table[--gui_table_count];
            return;
        }
    }
}

/* ============================================================
 *  WndProc
 *
 *  Dispatches WM_COMMAND, WM_CLOSE, WM_DESTROY, WM_PAINT, WM_SIZE,
 *  and WM_KEYDOWN to the Lisp callback if one is set on the window.
 *
 *  The callback receives (msg wparam lparam) and should return T
 *  to indicate that the message was handled, or NIL to allow the
 *  default processing.
 * ============================================================ */
static LRESULT CALLBACK ls_wnd_proc(HWND hwnd, UINT msg,
                                     WPARAM wparam, LPARAM lparam)
{
    ls_gui_entry_t *e = gui_find(hwnd);

    if (e && e->callback.tag != LS_T_NIL) {
        ls_value_t cbargs[3];
        cbargs[0] = ls_make_fixnum((int64_t)msg);
        cbargs[1] = ls_make_fixnum((int64_t)wparam);
        cbargs[2] = ls_make_fixnum((int64_t)lparam);
        ls_value_t result = ls_apply(e->L, e->callback, 3, cbargs);
        if (result.tag != LS_T_NIL)
            return 0; /* handled */
    }

    switch (msg) {
    case WM_DESTROY:
        if (e) gui_unregister(hwnd);
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}

/* ============================================================
 *  One-time window class registration
 * ============================================================ */
static void ensure_gui_init(void) {
    if (ls_gui_inited) return;
    ls_gui_inited = 1;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = ls_wnd_proc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = 0;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hIcon         = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = LS_WND_CLASS;
    wc.hIconSm      = LoadIconA(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);
}

/* ============================================================
 *  (gui-create-window title x y w h &optional style) -> fixnum(hwnd)
 * ============================================================ */
static ls_value_t bi_gui_create_window(ls_state_t *L, int nargs,
                                        ls_value_t *args)
{
    if (nargs < 5) {
        ls_error(L, "gui-create-window: need (title x y w h)");
        return ls_nil_v();
    }
    ensure_gui_init();
    const char *title = "Litesrpent";
    if (args[0].tag == LS_T_STRING)
        title = ((ls_string_t *)args[0].u.ptr)->chars;
    int x = (args[1].tag == LS_T_FIXNUM) ? (int)args[1].u.fixnum : CW_USEDEFAULT;
    int y = (args[2].tag == LS_T_FIXNUM) ? (int)args[2].u.fixnum : CW_USEDEFAULT;
    int w = (args[3].tag == LS_T_FIXNUM) ? (int)args[3].u.fixnum : 640;
    int h = (args[4].tag == LS_T_FIXNUM) ? (int)args[4].u.fixnum : 480;

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (nargs >= 6 && args[5].tag == LS_T_FIXNUM)
        style = (DWORD)args[5].u.fixnum;

    HWND hwnd = CreateWindowExA(
        0, LS_WND_CLASS, title, style,
        x, y, w, h,
        NULL, NULL, GetModuleHandleA(NULL), NULL
    );
    if (!hwnd) {
        ls_error(L, "gui-create-window: CreateWindowEx failed (err %lu)",
                 GetLastError());
        return ls_nil_v();
    }
    gui_register(hwnd, L);
    return ls_make_fixnum((int64_t)(intptr_t)hwnd);
}

/* ============================================================
 *  (gui-show-window hwnd &optional cmd)
 * ============================================================ */
static ls_value_t bi_gui_show_window(ls_state_t *L, int nargs,
                                      ls_value_t *args)
{
    (void)L;
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "gui-show-window: expected hwnd");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    int cmd = SW_SHOW;
    if (nargs >= 2 && args[1].tag == LS_T_FIXNUM)
        cmd = (int)args[1].u.fixnum;
    ShowWindow(hwnd, cmd);
    UpdateWindow(hwnd);
    return ls_t_v();
}

/* ============================================================
 *  (gui-destroy-window hwnd)
 * ============================================================ */
static ls_value_t bi_gui_destroy_window(ls_state_t *L, int nargs,
                                         ls_value_t *args)
{
    (void)L;
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "gui-destroy-window: expected hwnd");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    DestroyWindow(hwnd);
    return ls_t_v();
}

/* ============================================================
 *  (gui-message-box text title &optional flags) -> fixnum
 * ============================================================ */
static ls_value_t bi_gui_message_box(ls_state_t *L, int nargs,
                                      ls_value_t *args)
{
    (void)L;
    const char *text  = (nargs >= 1 && args[0].tag == LS_T_STRING)
                         ? ((ls_string_t *)args[0].u.ptr)->chars : "";
    const char *title = (nargs >= 2 && args[1].tag == LS_T_STRING)
                         ? ((ls_string_t *)args[1].u.ptr)->chars : "Litesrpent";
    UINT flags = MB_OK;
    if (nargs >= 3 && args[2].tag == LS_T_FIXNUM)
        flags = (UINT)args[2].u.fixnum;
    int result = MessageBoxA(NULL, text, title, flags);
    return ls_make_fixnum(result);
}

/* ============================================================
 *  (gui-pump-messages &optional block?) -> T if WM_QUIT, NIL otherwise
 *
 *  If block? is true (default), runs until WM_QUIT.
 *  If block? is NIL, processes all pending messages then returns.
 * ============================================================ */
static ls_value_t bi_gui_pump_messages(ls_state_t *L, int nargs,
                                        ls_value_t *args)
{
    (void)L;
    int blocking = 1;
    if (nargs >= 1 && args[0].tag == LS_T_NIL)
        blocking = 0;

    MSG msg;
    if (blocking) {
        while (GetMessageA(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return ls_t_v(); /* WM_QUIT received */
    } else {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return ls_t_v();
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return ls_nil_v();
    }
}

/* ============================================================
 *  (gui-set-text hwnd text)
 * ============================================================ */
static ls_value_t bi_gui_set_text(ls_state_t *L, int nargs,
                                   ls_value_t *args)
{
    if (nargs < 2) {
        ls_error(L, "gui-set-text: need (hwnd text)");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    const char *text = "";
    if (args[1].tag == LS_T_STRING)
        text = ((ls_string_t *)args[1].u.ptr)->chars;
    SetWindowTextA(hwnd, text);
    return ls_t_v();
}

/* ============================================================
 *  (gui-get-text hwnd) -> string
 * ============================================================ */
static ls_value_t bi_gui_get_text(ls_state_t *L, int nargs,
                                   ls_value_t *args)
{
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "gui-get-text: expected hwnd");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    int len = GetWindowTextLengthA(hwnd);
    if (len <= 0) return ls_make_string(L, "", 0);
    char *buf = (char *)malloc((size_t)(len + 1));
    if (!buf) return ls_make_string(L, "", 0);
    GetWindowTextA(hwnd, buf, len + 1);
    ls_value_t result = ls_make_string(L, buf, (size_t)len);
    free(buf);
    return result;
}

/* ============================================================
 *  (gui-create-button parent text x y w h id) -> fixnum(hwnd)
 * ============================================================ */
static ls_value_t bi_gui_create_button(ls_state_t *L, int nargs,
                                        ls_value_t *args)
{
    if (nargs < 7) {
        ls_error(L, "gui-create-button: need (parent text x y w h id)");
        return ls_nil_v();
    }
    HWND parent = (HWND)(intptr_t)args[0].u.fixnum;
    const char *text = (args[1].tag == LS_T_STRING)
                        ? ((ls_string_t *)args[1].u.ptr)->chars : "Button";
    int x  = (int)args[2].u.fixnum;
    int y  = (int)args[3].u.fixnum;
    int w  = (int)args[4].u.fixnum;
    int h  = (int)args[5].u.fixnum;
    int id = (int)args[6].u.fixnum;

    HWND btn = CreateWindowExA(
        0, "BUTTON", text,
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        x, y, w, h,
        parent, (HMENU)(intptr_t)id,
        GetModuleHandleA(NULL), NULL
    );
    if (!btn) {
        ls_error(L, "gui-create-button: CreateWindowEx failed");
        return ls_nil_v();
    }
    return ls_make_fixnum((int64_t)(intptr_t)btn);
}

/* ============================================================
 *  (gui-create-edit parent text x y w h &optional style) -> fixnum
 * ============================================================ */
static ls_value_t bi_gui_create_edit(ls_state_t *L, int nargs,
                                      ls_value_t *args)
{
    if (nargs < 6) {
        ls_error(L, "gui-create-edit: need (parent text x y w h)");
        return ls_nil_v();
    }
    HWND parent = (HWND)(intptr_t)args[0].u.fixnum;
    const char *text = (args[1].tag == LS_T_STRING)
                        ? ((ls_string_t *)args[1].u.ptr)->chars : "";
    int x = (int)args[2].u.fixnum;
    int y = (int)args[3].u.fixnum;
    int w = (int)args[4].u.fixnum;
    int h = (int)args[5].u.fixnum;

    DWORD style = WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL;
    if (nargs >= 7 && args[6].tag == LS_T_FIXNUM)
        style = (DWORD)args[6].u.fixnum;

    HWND edit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", text, style,
        x, y, w, h,
        parent, NULL,
        GetModuleHandleA(NULL), NULL
    );
    if (!edit) {
        ls_error(L, "gui-create-edit: CreateWindowEx failed");
        return ls_nil_v();
    }
    return ls_make_fixnum((int64_t)(intptr_t)edit);
}

/* ============================================================
 *  (gui-create-static parent text x y w h) -> fixnum
 * ============================================================ */
static ls_value_t bi_gui_create_static(ls_state_t *L, int nargs,
                                        ls_value_t *args)
{
    if (nargs < 6) {
        ls_error(L, "gui-create-static: need (parent text x y w h)");
        return ls_nil_v();
    }
    HWND parent = (HWND)(intptr_t)args[0].u.fixnum;
    const char *text = (args[1].tag == LS_T_STRING)
                        ? ((ls_string_t *)args[1].u.ptr)->chars : "";
    int x = (int)args[2].u.fixnum;
    int y = (int)args[3].u.fixnum;
    int w = (int)args[4].u.fixnum;
    int h = (int)args[5].u.fixnum;

    HWND st = CreateWindowExA(
        0, "STATIC", text,
        WS_VISIBLE | WS_CHILD | SS_LEFT,
        x, y, w, h,
        parent, NULL,
        GetModuleHandleA(NULL), NULL
    );
    if (!st) {
        ls_error(L, "gui-create-static: CreateWindowEx failed");
        return ls_nil_v();
    }
    return ls_make_fixnum((int64_t)(intptr_t)st);
}

/* ============================================================
 *  (gui-set-callback hwnd fn)
 * ============================================================ */
static ls_value_t bi_gui_set_callback(ls_state_t *L, int nargs,
                                       ls_value_t *args)
{
    if (nargs < 2) {
        ls_error(L, "gui-set-callback: need (hwnd fn)");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    ls_gui_entry_t *e = gui_find(hwnd);
    if (!e) {
        /* Register this hwnd on the fly (e.g. child controls). */
        gui_register(hwnd, L);
        e = gui_find(hwnd);
    }
    if (e) e->callback = args[1];
    return ls_t_v();
}

/* ============================================================
 *  (gui-invalidate hwnd &optional erase?)
 * ============================================================ */
static ls_value_t bi_gui_invalidate(ls_state_t *L, int nargs,
                                     ls_value_t *args)
{
    (void)L;
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "gui-invalidate: expected hwnd");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    BOOL erase = TRUE;
    if (nargs >= 2 && args[1].tag == LS_T_NIL) erase = FALSE;
    InvalidateRect(hwnd, NULL, erase);
    return ls_t_v();
}

/* ============================================================
 *  (gui-get-dc hwnd) -> fixnum(hdc)
 * ============================================================ */
static ls_value_t bi_gui_get_dc(ls_state_t *L, int nargs,
                                 ls_value_t *args)
{
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "gui-get-dc: expected hwnd");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    HDC hdc = GetDC(hwnd);
    return ls_make_fixnum((int64_t)(intptr_t)hdc);
}

/* ============================================================
 *  (gui-release-dc hwnd hdc)
 * ============================================================ */
static ls_value_t bi_gui_release_dc(ls_state_t *L, int nargs,
                                     ls_value_t *args)
{
    if (nargs < 2) {
        ls_error(L, "gui-release-dc: need (hwnd hdc)");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    HDC hdc   = (HDC)(intptr_t)args[1].u.fixnum;
    ReleaseDC(hwnd, hdc);
    return ls_t_v();
}

/* ============================================================
 *  (gui-set-pixel hdc x y color)
 * ============================================================ */
static ls_value_t bi_gui_set_pixel(ls_state_t *L, int nargs,
                                    ls_value_t *args)
{
    if (nargs < 4) {
        ls_error(L, "gui-set-pixel: need (hdc x y color)");
        return ls_nil_v();
    }
    HDC hdc = (HDC)(intptr_t)args[0].u.fixnum;
    int x = (int)args[1].u.fixnum;
    int y = (int)args[2].u.fixnum;
    COLORREF c = (COLORREF)args[3].u.fixnum;
    SetPixel(hdc, x, y, c);
    return ls_t_v();
}

/* ============================================================
 *  (gui-move-window hwnd x y w h &optional repaint?)
 * ============================================================ */
static ls_value_t bi_gui_move_window(ls_state_t *L, int nargs,
                                      ls_value_t *args)
{
    if (nargs < 5) {
        ls_error(L, "gui-move-window: need (hwnd x y w h)");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    int x = (int)args[1].u.fixnum;
    int y = (int)args[2].u.fixnum;
    int w = (int)args[3].u.fixnum;
    int h = (int)args[4].u.fixnum;
    BOOL repaint = TRUE;
    if (nargs >= 6 && args[5].tag == LS_T_NIL) repaint = FALSE;
    MoveWindow(hwnd, x, y, w, h, repaint);
    return ls_t_v();
}

/* ============================================================
 *  (gui-enable-window hwnd enable?)
 * ============================================================ */
static ls_value_t bi_gui_enable_window(ls_state_t *L, int nargs,
                                        ls_value_t *args)
{
    if (nargs < 2) {
        ls_error(L, "gui-enable-window: need (hwnd enable?)");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    BOOL enable = (args[1].tag != LS_T_NIL) ? TRUE : FALSE;
    EnableWindow(hwnd, enable);
    return ls_t_v();
}

/* ============================================================
 *  (gui-set-timer hwnd id ms) -> fixnum
 * ============================================================ */
static ls_value_t bi_gui_set_timer(ls_state_t *L, int nargs,
                                    ls_value_t *args)
{
    if (nargs < 3) {
        ls_error(L, "gui-set-timer: need (hwnd id ms)");
        return ls_nil_v();
    }
    HWND hwnd    = (HWND)(intptr_t)args[0].u.fixnum;
    UINT_PTR id  = (UINT_PTR)args[1].u.fixnum;
    UINT ms      = (UINT)args[2].u.fixnum;
    UINT_PTR ret = SetTimer(hwnd, id, ms, NULL);
    return ls_make_fixnum((int64_t)ret);
}

/* ============================================================
 *  (gui-kill-timer hwnd id)
 * ============================================================ */
static ls_value_t bi_gui_kill_timer(ls_state_t *L, int nargs,
                                     ls_value_t *args)
{
    if (nargs < 2) {
        ls_error(L, "gui-kill-timer: need (hwnd id)");
        return ls_nil_v();
    }
    HWND hwnd   = (HWND)(intptr_t)args[0].u.fixnum;
    UINT_PTR id = (UINT_PTR)args[1].u.fixnum;
    KillTimer(hwnd, id);
    return ls_t_v();
}

/* ============================================================
 *  (gui-get-client-rect hwnd) -> (list x y w h)
 * ============================================================ */
static ls_value_t bi_gui_get_client_rect(ls_state_t *L, int nargs,
                                          ls_value_t *args)
{
    if (nargs < 1 || args[0].tag != LS_T_FIXNUM) {
        ls_error(L, "gui-get-client-rect: expected hwnd");
        return ls_nil_v();
    }
    HWND hwnd = (HWND)(intptr_t)args[0].u.fixnum;
    RECT r;
    GetClientRect(hwnd, &r);
    ls_value_t tail = ls_nil_v();
    tail = ls_cons(L, ls_make_fixnum(r.bottom - r.top), tail);
    tail = ls_cons(L, ls_make_fixnum(r.right - r.left), tail);
    tail = ls_cons(L, ls_make_fixnum(r.top), tail);
    tail = ls_cons(L, ls_make_fixnum(r.left), tail);
    return tail;
}

/* ============================================================
 *  WM_ constants exported as Lisp symbols
 * ============================================================ */
static void gui_export_constants(ls_state_t *L) {
    struct { const char *name; int64_t val; } consts[] = {
        {"WM-CREATE",   0x0001}, {"WM-DESTROY",  0x0002},
        {"WM-CLOSE",    0x0010}, {"WM-PAINT",    0x000F},
        {"WM-SIZE",     0x0005}, {"WM-COMMAND",  0x0111},
        {"WM-TIMER",    0x0113}, {"WM-KEYDOWN",  0x0100},
        {"WM-KEYUP",    0x0101}, {"WM-CHAR",     0x0102},
        {"WM-LBUTTONDOWN", 0x0201}, {"WM-RBUTTONDOWN", 0x0204},
        {"WM-MOUSEMOVE", 0x0200}, {"WM-LBUTTONUP", 0x0202},
        {"SW-SHOW", SW_SHOW}, {"SW-HIDE", SW_HIDE},
        {"SW-MAXIMIZE", SW_MAXIMIZE}, {"SW-MINIMIZE", SW_MINIMIZE},
        {"MB-OK", MB_OK}, {"MB-YESNO", MB_YESNO},
        {"MB-OKCANCEL", MB_OKCANCEL},
        {"MB-ICONERROR", MB_ICONERROR},
        {"MB-ICONWARNING", MB_ICONWARNING},
        {"MB-ICONINFORMATION", MB_ICONINFORMATION},
        {"IDOK", IDOK}, {"IDCANCEL", IDCANCEL},
        {"IDYES", IDYES}, {"IDNO", IDNO},
        {NULL, 0}
    };
    for (int i = 0; consts[i].name; i++) {
        ls_value_t sym = ls_intern(L, "LITESRPENT-SYSTEM", consts[i].name);
        ls_symbol_t *s = (ls_symbol_t *)sym.u.ptr;
        s->value = ls_make_fixnum(consts[i].val);
        s->sym_flags |= LS_SYM_HAS_VALUE | LS_SYM_CONSTANT;
    }
}

/* ============================================================
 *  Registration
 * ============================================================ */
void ls_init_gui(ls_state_t *L) {
#define DGUI(name, fn, min, max) ls_defun(L, "LITESRPENT-SYSTEM", name, fn, min, max)
    DGUI("GUI-CREATE-WINDOW",   bi_gui_create_window,   5, 6);
    DGUI("GUI-SHOW-WINDOW",     bi_gui_show_window,     1, 2);
    DGUI("GUI-DESTROY-WINDOW",  bi_gui_destroy_window,  1, 1);
    DGUI("GUI-MESSAGE-BOX",     bi_gui_message_box,     1, 3);
    DGUI("GUI-PUMP-MESSAGES",   bi_gui_pump_messages,   0, 1);
    DGUI("GUI-SET-TEXT",        bi_gui_set_text,        2, 2);
    DGUI("GUI-GET-TEXT",        bi_gui_get_text,        1, 1);
    DGUI("GUI-CREATE-BUTTON",   bi_gui_create_button,   7, 7);
    DGUI("GUI-CREATE-EDIT",     bi_gui_create_edit,     6, 7);
    DGUI("GUI-CREATE-STATIC",   bi_gui_create_static,   6, 6);
    DGUI("GUI-SET-CALLBACK",    bi_gui_set_callback,    2, 2);
    DGUI("GUI-INVALIDATE",      bi_gui_invalidate,      1, 2);
    DGUI("GUI-GET-DC",          bi_gui_get_dc,          1, 1);
    DGUI("GUI-RELEASE-DC",      bi_gui_release_dc,      2, 2);
    DGUI("GUI-SET-PIXEL",       bi_gui_set_pixel,       4, 4);
    DGUI("GUI-MOVE-WINDOW",     bi_gui_move_window,     5, 6);
    DGUI("GUI-ENABLE-WINDOW",   bi_gui_enable_window,   2, 2);
    DGUI("GUI-SET-TIMER",       bi_gui_set_timer,       3, 3);
    DGUI("GUI-KILL-TIMER",      bi_gui_kill_timer,      2, 2);
    DGUI("GUI-GET-CLIENT-RECT", bi_gui_get_client_rect, 1, 1);
#undef DGUI
    gui_export_constants(L);
}

#else  /* !_WIN32 -- stub for non-Windows builds */

void ls_init_gui(ls_state_t *L) {
    (void)L;
    /* No GUI support on non-Windows for now. */
}

#endif /* _WIN32 */
