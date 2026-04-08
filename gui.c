/*
 * gui.c — Win32 GUI front-end for the SQLite clone engine.
 *
 * Compile with:
 *   make gui
 * or manually:
 *   gcc -std=c99 -Wall -mwindows -o sqlite_clone.exe db.c gui.c app.res -lcomdlg32
 *
 * Window layout
 * ─────────────
 *   ┌─[Menu: File]──────────────────────────────────────┐
 *   │  output area (read-only, multiline, scrollable)    │
 *   │                                                    │
 *   │                                                    │
 *   ├────────────────────────────────────────────────────┤
 *   │ db > [  input field                          ] Run │
 *   └────────────────────────────────────────────────────┘
 *
 * The "File → Open / New Database" dialog lets the user pick or
 * create a .db file.  The window title shows the current filename.
 *
 * Pressing Enter in the input field (or clicking Run) dispatches the
 * command to the engine and appends the result to the output area.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

/* ------------------------------------------------------------------ */
/* Resource / control IDs                                               */
/* ------------------------------------------------------------------ */

#define IDC_OUTPUT      101
#define IDC_INPUT       102
#define IDC_RUN_BTN     103

#define IDM_FILE_OPEN   201
#define IDM_FILE_NEW    202
#define IDM_FILE_CLOSE  203
#define IDM_FILE_EXIT   204
#define IDM_HELP_ABOUT  301

#define APP_ICON        1   /* icon resource ID defined in app.rc */

/* Height of the input bar at the bottom of the window (pixels) */
#define INPUT_BAR_HEIGHT 36
#define RUN_BTN_WIDTH    60
#define PROMPT_WIDTH     40  /* width of the "db > " static label */

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

static HWND   g_hwnd        = NULL;   /* main window                  */
static HWND   g_output      = NULL;   /* read-only multiline edit     */
static HWND   g_input       = NULL;   /* single-line input edit       */
static HWND   g_run_btn     = NULL;   /* "Run" button                 */
static HWND   g_prompt_lbl  = NULL;   /* static "db > " label         */

static Table* g_table       = NULL;   /* NULL when no db is open      */
static char   g_db_path[MAX_PATH] = {0};

static WNDPROC g_orig_input_proc = NULL;  /* original Edit proc (subclassed) */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Convert a narrow (ANSI/UTF-8) string to a wide string.
 * Caller must free() the returned buffer.
 */
static WCHAR* to_wide(const char* s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR* w = (WCHAR*)malloc((size_t)n * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* Convert a wide string to narrow UTF-8.  Caller must free(). */
static char* to_narrow(const WCHAR* w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char* s = (char*)malloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* Append text to the read-only output Edit control. */
static void output_append(const char* text) {
    if (!text || !*text) return;
    WCHAR* w = to_wide(text);
    /* Move caret to end, then insert */
    LRESULT len = SendMessage(g_output, WM_GETTEXTLENGTH, 0, 0);
    SendMessage(g_output, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessage(g_output, EM_REPLACESEL, FALSE, (LPARAM)w);
    /* Scroll to bottom */
    SendMessage(g_output, WM_VSCROLL, SB_BOTTOM, 0);
    free(w);
}

/* Append a formatted line (narrow) to the output control. */
static void output_printf(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    output_append(buf);
}

/* Update the window title to reflect the current database path. */
static void update_title(void) {
    if (g_db_path[0]) {
        char title[MAX_PATH + 64];
        /* Extract just the filename part for a cleaner title */
        const char* name = strrchr(g_db_path, '\\');
        if (!name) name = strrchr(g_db_path, '/');
        name = name ? name + 1 : g_db_path;
        snprintf(title, sizeof(title), "SQLite Clone — %s", name);
        WCHAR* w = to_wide(title);
        SetWindowTextW(g_hwnd, w);
        free(w);
    } else {
        SetWindowTextW(g_hwnd, L"SQLite Clone");
    }
}

/* Enable / disable controls that require an open database. */
static void update_ui_state(void) {
    BOOL open = (g_table != NULL);
    EnableWindow(g_input,   open);
    EnableWindow(g_run_btn, open);
}

/* ------------------------------------------------------------------ */
/* Database open / close                                                */
/* ------------------------------------------------------------------ */

static void do_close_db(void) {
    if (g_table) {
        db_close(g_table);
        g_table    = NULL;
        g_db_path[0] = '\0';
        output_append("\r\n[Database closed.]\r\n");
    }
    update_title();
    update_ui_state();
}

static void do_open_db(const char* path) {
    /* Close any currently open database first */
    if (g_table) do_close_db();

    db_output_reset();
    Table* t = db_open(path);
    if (!t) {
        /* Engine wrote an error into db_output_buf */
        output_printf("[Error] %s\r\n",
                      db_output_len > 0 ? db_output_buf : "Could not open database.");
        return;
    }

    g_table = t;
    strncpy(g_db_path, path, MAX_PATH - 1);
    g_db_path[MAX_PATH - 1] = '\0';

    update_title();
    update_ui_state();

    /* Welcome banner */
    output_append("SQLite Clone\r\n");
    output_printf("Opened: %s\r\n", g_db_path);
    output_append("Commands: insert <id> <user> <email>  |  select  |  .tables  |  .btree  |  .exit\r\n");
    output_append("─────────────────────────────────────────────────────────────────────────────────\r\n");
}

/* ------------------------------------------------------------------ */
/* File dialogs                                                         */
/* ------------------------------------------------------------------ */

static void show_open_dialog(BOOL create_new) {
    WCHAR path_w[MAX_PATH] = {0};

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = g_hwnd;
    ofn.lpstrFilter     = L"SQLite Clone DB (*.db)\0*.db\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile       = path_w;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrDefExt     = L"db";
    ofn.Flags           = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (create_new) {
        ofn.lpstrTitle  = L"New Database";
        ofn.Flags      |= OFN_OVERWRITEPROMPT;
        if (!GetSaveFileNameW(&ofn)) return;
    } else {
        ofn.lpstrTitle  = L"Open Database";
        ofn.Flags      |= OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
    }

    char* path_a = to_narrow(path_w);
    do_open_db(path_a);
    free(path_a);
}

/* ------------------------------------------------------------------ */
/* Command dispatch                                                     */
/* ------------------------------------------------------------------ */

static void dispatch_command(void) {
    if (!g_table) {
        output_append("[No database open — use File → Open or New.]\r\n");
        return;
    }

    /* Read the input field */
    int len = GetWindowTextLengthA(g_input);
    if (len <= 0) return;

    char* cmd = (char*)malloc((size_t)len + 1);
    GetWindowTextA(g_input, cmd, len + 1);

    /* Echo the command in the output area */
    output_printf("db > %s\r\n", cmd);

    /* Clear the input field */
    SetWindowTextA(g_input, "");

    /* Run through the engine */
    MetaCommandResult res = db_run_command(g_table, cmd);
    free(cmd);

    /* Show whatever the engine produced */
    if (db_output_len > 0) {
        /* The engine uses \n; convert to \r\n for the Edit control */
        char* out = (char*)malloc((size_t)db_output_len * 2 + 1);
        int j = 0;
        for (int i = 0; i < db_output_len; i++) {
            if (db_output_buf[i] == '\n') {
                out[j++] = '\r';
            }
            out[j++] = db_output_buf[i];
        }
        out[j] = '\0';
        output_append(out);
        free(out);
    }

    if (res == META_COMMAND_EXIT) {
        output_append("\r\n[Closing database and exiting...]\r\n");
        db_close(g_table);
        g_table = NULL;
        DestroyWindow(g_hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* Input field subclass — intercept Enter key                          */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK input_subclass_proc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        dispatch_command();
        return 0;
    }
    return CallWindowProc(g_orig_input_proc, hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Layout helper — called on WM_CREATE and WM_SIZE                     */
/* ------------------------------------------------------------------ */

static void layout_controls(int cx, int cy) {
    int input_y     = cy - INPUT_BAR_HEIGHT;
    int output_h    = input_y;

    /* Output area fills the top portion */
    SetWindowPos(g_output, NULL,
                 0, 0, cx, output_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    /* Separator line between output and input bar is just the gap */

    /* "db > " label on the left of the input bar */
    SetWindowPos(g_prompt_lbl, NULL,
                 4, input_y + (INPUT_BAR_HEIGHT - 20) / 2,
                 PROMPT_WIDTH, 20,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    /* Run button on the right */
    SetWindowPos(g_run_btn, NULL,
                 cx - RUN_BTN_WIDTH - 4,
                 input_y + (INPUT_BAR_HEIGHT - 24) / 2,
                 RUN_BTN_WIDTH, 24,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    /* Input edit between label and button */
    int input_x = 4 + PROMPT_WIDTH + 4;
    int input_w = cx - input_x - RUN_BTN_WIDTH - 12;
    SetWindowPos(g_input, NULL,
                 input_x,
                 input_y + (INPUT_BAR_HEIGHT - 24) / 2,
                 input_w, 24,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        HFONT mono = CreateFont(
            16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
            L"Consolas");

        /* Output area — read-only, multiline, vertical scroll */
        g_output = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd,
            (HMENU)(UINT_PTR)IDC_OUTPUT,
            GetModuleHandle(NULL), NULL);
        SendMessage(g_output, WM_SETFONT, (WPARAM)mono, TRUE);
        /* Raise character limit for the output edit */
        SendMessage(g_output, EM_SETLIMITTEXT, (WPARAM)0x7FFFFF, 0);

        /* "db > " prompt label */
        g_prompt_lbl = CreateWindowW(
            L"STATIC", L"db >",
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            0, 0, 0, 0, hwnd,
            (HMENU)(UINT_PTR)0,
            GetModuleHandle(NULL), NULL);
        SendMessage(g_prompt_lbl, WM_SETFONT, (WPARAM)mono, TRUE);

        /* Input edit */
        g_input = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd,
            (HMENU)(UINT_PTR)IDC_INPUT,
            GetModuleHandle(NULL), NULL);
        SendMessage(g_input, WM_SETFONT, (WPARAM)mono, TRUE);

        /* Subclass the input edit to catch Enter */
        g_orig_input_proc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(
            g_input, GWLP_WNDPROC, (LONG_PTR)input_subclass_proc);

        /* Run button */
        g_run_btn = CreateWindowW(
            L"BUTTON", L"Run",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd,
            (HMENU)(UINT_PTR)IDC_RUN_BTN,
            GetModuleHandle(NULL), NULL);

        update_ui_state();

        /* Show the "Open DB" dialog immediately if no db path yet */
        PostMessage(hwnd, WM_COMMAND, IDM_FILE_OPEN, 0);
        return 0;
    }

    case WM_SIZE:
        layout_controls(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDM_FILE_OPEN:
            show_open_dialog(FALSE);
            break;

        case IDM_FILE_NEW:
            show_open_dialog(TRUE);
            break;

        case IDM_FILE_CLOSE:
            do_close_db();
            break;

        case IDM_FILE_EXIT:
            if (g_table) db_close(g_table);
            g_table = NULL;
            DestroyWindow(hwnd);
            break;

        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"SQLite Clone  v1.0\r\n\r\n"
                L"A hand-rolled B-tree database engine\r\n"
                L"with a Win32 GUI front-end.\r\n\r\n"
                L"Commands:\r\n"
                L"  insert <id> <username> <email>\r\n"
                L"  select\r\n"
                L"  .tables\r\n"
                L"  .btree\r\n"
                L"  .exit",
                L"About SQLite Clone",
                MB_OK | MB_ICONINFORMATION);
            break;

        case IDC_RUN_BTN:
            dispatch_command();
            /* Return focus to the input field */
            SetFocus(g_input);
            break;
        }
        return 0;

    case WM_SETFOCUS:
        if (g_input) SetFocus(g_input);
        return 0;

    case WM_CLOSE:
        if (g_table) {
            db_close(g_table);
            g_table = NULL;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* Build the menu bar                                                   */
/* ------------------------------------------------------------------ */

static HMENU create_menu(void) {
    HMENU menu_bar = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_OPEN,  L"&Open Database...\tCtrl+O");
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_NEW,   L"&New Database...\tCtrl+N");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_CLOSE, L"&Close Database");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_EXIT,  L"E&xit\tAlt+F4");

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_HELP_ABOUT, L"&About...");

    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)file_menu, L"&File");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)help_menu, L"&Help");

    return menu_bar;
}

/* ------------------------------------------------------------------ */
/* WinMain                                                              */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    /* Register window class */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(APP_ICON));
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = L"SqliteCloneWnd";
    wc.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(APP_ICON));

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"RegisterClassEx failed.", L"Error", MB_ICONERROR);
        return 1;
    }

    /* If a .db path was supplied on the command line, use it */
    if (lpCmdLine && lpCmdLine[0] != '\0') {
        /* Strip surrounding quotes if present */
        char path[MAX_PATH] = {0};
        if (lpCmdLine[0] == '"') {
            snprintf(path, MAX_PATH, "%s", lpCmdLine + 1);
            char* q = strrchr(path, '"');
            if (q) *q = '\0';
        } else {
            snprintf(path, MAX_PATH, "%s", lpCmdLine);
        }
        snprintf(g_db_path, MAX_PATH, "%s", path);
    }

    /* Create the main window */
    g_hwnd = CreateWindowExW(
        0,
        L"SqliteCloneWnd",
        L"SQLite Clone",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        860, 560,
        NULL, create_menu(), hInstance, NULL);

    if (!g_hwnd) {
        MessageBoxW(NULL, L"CreateWindow failed.", L"Error", MB_ICONERROR);
        return 1;
    }

    /* If a path was given on the CLI, open it directly
       (WM_CREATE already posts IDM_FILE_OPEN, so we cancel that by
       opening the db before the message loop starts). */
    if (g_db_path[0]) {
        /* We do the open synchronously here; the posted IDM_FILE_OPEN
           will be a no-op because g_table will already be non-NULL. */
        do_open_db(g_db_path);
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    /* Message loop */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
