/*
 * vm_display_idd.c -- Host-side IDD frame receiver + D3D11 renderer.
 *
 * Connects to the guest VM over AF_HYPERV sockets:
 *   :0002  Frame channel — receives frames, renders via D3D11 textured quad
 *   :0003  Input channel — forwards keyboard/mouse events to guest
 *   :0004  Audio channel — receives audio from guest, renders via WASAPI
 *
 * Clipboard sync (:0005/:0006) is handled by vm_clipboard.c.
 *
 * Pure C, compiled as C.
 */

#include <winsock2.h>
#include <windows.h>

#define COBJMACROS
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union in SDK headers */
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#pragma warning(pop)

#include <stdio.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "../core/protocol.h"
#include "vm_clipboard.h"
#include "vm_agent.h"
#include "hcs_vm.h"
#include "ui.h"
#include "resource.h"
#include "../core/display_protocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif


/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* These three are superseded by hcs_service_guid(os_type, port, ...) — kept
   for grep. Windows VMs reach byte-identical GUIDs via the helper; Linux
   VMs reach vsock-template GUIDs. */
static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Input channel service GUID — connects to agent for SendInput injection */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Audio capture channel — connects to guest audio helper (guest→host) */
static const GUID AUDIO_SERVICE_GUID =
    { 0xa5b0cafe, 0x0004, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Audio wire protocol (mirror of tools/agent/appsandbox-audio.c) ---- */

#define AUDIO_HEADER_MAGIC  0x31415341  /* 'ASA1' */

#pragma pack(push, 1)
typedef struct AudioHeader {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;      /* 1=PCM, 3=IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct AudioFrameHeader {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

/* Clipboard reader-apply message (posted by vm_clipboard.c to our wndproc) */
#define WM_CLIP_READER_APPLY (WM_APP + 11)

/* ---- Frame protocol constants ---- */

#define FRAME_MAGIC         ASB_DISPLAY_RAW_MAGIC
#define DEFAULT_WIDTH       ASB_DISPLAY_DEFAULT_WIDTH
#define DEFAULT_HEIGHT      ASB_DISPLAY_DEFAULT_HEIGHT
#define MAX_DIRTY_RECTS     ASB_DISPLAY_MAX_DIRTY_RECTS
#define MAX_FRAME_DATA_SIZE ASB_DISPLAY_MAX_FRAME_DATA_SIZE

/* ---- Window messages ---- */

#define WM_VM_DISPLAY_CLOSED    (WM_APP + 5)
#define WM_IDD_FRAME_READY      (WM_USER + 100)
#define WM_IDD_FOCUS            (WM_USER + 101)
#define WM_IDD_INPUT_READY      (WM_USER + 102)
#define WM_IDD_CURSOR_CHANGED   (WM_USER + 103)
#define WM_IDD_RESIZE_CAPABLE   (WM_USER + 104)
#define WM_IDD_ACTUAL_SIZE      (WM_USER + 105)
#define WM_IDD_RESIZE_RETRY     (WM_USER + 106)

typedef enum DisplayResizePhase {
    DISPLAY_RESIZE_PHASE_NONE = 0,
    DISPLAY_RESIZE_PHASE_WAIT_ACK,
    DISPLAY_RESIZE_PHASE_WAIT_FRAME,
} DisplayResizePhase;

/* Timer for Present cadence when no frames arrive */
#define IDT_PRESENT     2001
#define PRESENT_MS      16   /* ~60 fps */
#define IDT_FULLSCREEN_TOOLBAR 2002
#define IDT_DISPLAY_RESIZE 2003
#define DISPLAY_RESIZE_DEBOUNCE_MS 125
#define DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS \
    ASB_DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS
#define DISPLAY_RESIZE_RETRY_LIMIT ASB_DISPLAY_RESIZE_RETRY_LIMIT
#define IDM_ENTER_FULLSCREEN 0x1030
#define TOOLBAR_POLL_MS 100
#define TOOLBAR_HIDE_MS 700
#define TOOLBAR_HOTZONE_DIP 4
#define TOOLBAR_HEIGHT_DIP 44
#define TOOLBAR_WIDTH_DIP 176
#define TOOLBAR_EDGE_MARGIN_DIP 8

/* Debug log window */
#define IDC_LOG_LIST      3001
#define MAX_LOG_LINES     200
#define LOG_WINDOW_W      800
#define LOG_WINDOW_H      300

/* ---- HLSL shaders (inline strings) ---- */

static const char g_vs_hlsl[] =
    "struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    VS_OUT o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char g_ps_hlsl[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    return tex.Sample(samp, uv);\n"
    "}\n";

/* ---- Frame header from wire ---- */

#pragma pack(push, 1)
typedef AsbDisplayFrameHeader FrameHeader;

/* ---- Cursor header from wire (must match VDD_WIRE_CURSOR_HEADER) ---- */

#define CURSOR_MAGIC        0x52435341  /* "ASCR" little-endian */
#define MAX_CURSOR_SIZE     (256 * 256 * 4 * 2)  /* 2x for MASKED_COLOR double-height */

typedef struct CursorHeader {
    UINT32 magic;
    INT32  x;
    INT32  y;
    UINT32 visible;
    UINT32 shape_updated;
    UINT32 shape_id;
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 xhot;
    UINT32 yhot;
    UINT32 cursor_type;     /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32 shape_data_size;
} CursorHeader;
#pragma pack(pop)

/* ---- Display context ---- */

struct VmDisplayIdd {
    VmInstance  *vm;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    wchar_t      os_type[32];      /* copy of vm->os_type for picking the service GUID variant */
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    BOOL         fullscreen;
    WINDOWPLACEMENT windowed_placement;
    LONG_PTR     windowed_style;
    LONG_PTR     windowed_ex_style;
    HWND         fullscreen_toolbar;
    BOOL         fullscreen_toolbar_visible;
    BOOL         fullscreen_toolbar_pressed;
    BOOL         suppress_f11_up;
    ULONGLONG    toolbar_leave_tick;
    volatile BOOL open;
    volatile BOOL stop;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    ID3D11ShaderResourceView *frame_srv;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11SamplerState      *sampler;

    /* Frame buffer (CPU-side, updated by recv thread) */
    BYTE          *frame_buf;
    UINT           frame_width;
    UINT           frame_height;
    UINT           frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL  frame_dirty;
    RECT           pending_dirty[MAX_DIRTY_RECTS];
    UINT           pending_dirty_count;
    BOOL           pending_full_upload;

    /* Host-side upload counters. These are intentionally cheap counters and
     * are logged from the render thread at a low cadence. */
    ULONGLONG      host_full_uploads;
    ULONGLONG      host_partial_uploads;
    ULONGLONG      host_gpu_upload_bytes;
    ULONGLONG      host_stats_start_ms;
    ULONGLONG      host_stats_last_log_ms;
    ULONGLONG      frame_seq_gaps;
    ULONGLONG      last_frame_seq;
    BOOL           have_frame_seq;

    UINT           render_count;     /* number of renders (for one-shot logging) */
    volatile UINT  recv_count;       /* number of frames received over HvSocket */

    /* Host <-> guest display-control state. ACK is not completion: the
     * pending request is cleared only after an ASFR with matching dimensions. */
    SOCKET         frame_socket;
    SRWLOCK        frame_send_lock;
    HANDLE         control_send_event;
    HANDLE         control_send_thread;
    volatile BOOL  control_send_stop;
    SRWLOCK        control_queue_lock;
    AsbDisplayControl queued_control;
    BOOL           control_queued;
    volatile LONG  resize_capable;
    SRWLOCK        resize_lock;
    UINT           desired_width;
    UINT           desired_height;
    UINT           actual_width;
    UINT           actual_height;
    BOOL           actual_valid;
    BOOL           force_resize_sync;
    ULONGLONG      actual_generation;
    UINT           queued_resize_width;
    UINT           queued_resize_height;
    UINT           pending_resize_width;
    UINT           pending_resize_height;
    UINT32         resize_request_id;
    UINT32         pending_resize_id;
    ULONGLONG      pending_resize_generation;
    ULONGLONG      pending_resize_deadline;
    DisplayResizePhase pending_resize_phase;
    UINT           pending_resize_retry_count;
    BOOL           resize_queued;
    ULONGLONG      last_resize_send_tick;
    ULONGLONG      frame_connection_generation;
    ULONGLONG      queued_control_generation;

    /* One explicit UI request at a time. The configured resolution is kept
     * in VmInstance; this transient state is only cleared after ASFR (or a
     * terminal protocol failure). */
    HWND           runtime_notify_hwnd;
    BOOL           runtime_request_active;
    UINT           runtime_request_width;
    UINT           runtime_request_height;
    ULONGLONG      runtime_request_deadline;

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    SRWLOCK        input_lock;
    volatile LONG  keyboard_version;
    volatile LONG  mouse_version;
    volatile BOOL  frame_connected;
    BOOL           relative_mouse;
    BOOL           input_sizing;
    UINT           mouse_buttons;
    BOOL           raw_absolute_valid;
    HANDLE         raw_absolute_device;
    POINT          raw_absolute_position;
    BOOL           mouse_sync_pending;
    UINT32         mouse_sync_id;
    ULONGLONG      mouse_sync_deadline;
    POINT          mouse_sync_origin;
    SOCKET         mouse_sync_socket;
    InputPacket    mouse_reply;
    int            mouse_reply_size;
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */

    /* Keyboard hotkey handling */
    wchar_t        vhdx_path[MAX_PATH]; /* copy of vm->vhdx_path for settings file */
    volatile BOOL  transmit_hotkeys; /* TRUE = capture host hotkeys + send to guest */
    volatile BOOL  input_focused;    /* TRUE while our top-level window is active */
    HHOOK          kbd_hook;          /* WH_KEYBOARD_LL handle, NULL when not installed */
    /* Legacy keys use VK indices; physical keys use scan/E0 or 512 + function VK. */
    BYTE           held_down[768];
    InputPacket    held_keys[768];
    BYTE           hook_routes[768];
    BOOL           input_menu_active;

    /* Guest cursor */
    HCURSOR        guest_cursor;    /* current cursor created from guest bitmap */
    UINT32         cursor_shape_id; /* tracks which shape is current */
    BOOL           cursor_visible;  /* guest cursor visibility */

    /* Debug log window (separate top-level window) */
    HWND           log_hwnd;        /* top-level log window */
    HWND           log_list_hwnd;   /* listbox inside log window */
    HWND           render_hwnd;     /* child window for D3D11 rendering */

    /* Clipboard (extracted to vm_clipboard.c) */
    VmClipboard      clipboard;

    /* Audio playback channel (:0004 — guest→host render) */
    volatile SOCKET  audio_socket;
    HANDLE           audio_recv_thread;
    volatile BOOL    audio_muted;

    /* Threads */
    HANDLE         recv_thread;
    HANDLE         window_thread;
};

/* ---- Forward declarations ---- */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static DWORD WINAPI     idd_window_thread_proc(LPVOID param);
static DWORD WINAPI     idd_recv_thread_proc(LPVOID param);
static void idd_update_relative_mouse(VmDisplayIdd *d);
static void idd_resume_absolute_mouse(VmDisplayIdd *d, const InputPacket *reply);
static void idd_poll_mouse_position(VmDisplayIdd *d);
static LRESULT CALLBACK idd_fullscreen_toolbar_proc(HWND hwnd, UINT msg,
                                                     WPARAM wp, LPARAM lp);
static void idd_register_fullscreen_toolbar_class(HINSTANCE hInst);
static void idd_enter_fullscreen(VmDisplayIdd *d);
static void idd_exit_fullscreen(VmDisplayIdd *d);
static void idd_show_fullscreen_toolbar(VmDisplayIdd *d);
static void idd_schedule_resize(VmDisplayIdd *d, UINT width, UINT height,
                                BOOL immediate);
static void idd_flush_resize(VmDisplayIdd *d);
static void idd_check_resize_timeout(VmDisplayIdd *d);
static void idd_check_runtime_request_timeout(VmDisplayIdd *d);
static void idd_post_runtime_result(VmDisplayIdd *d, BOOL success,
                                    UINT width, UINT height);
static BOOL idd_send_display_control(VmDisplayIdd *d,
                                      const AsbDisplayControl *control,
                                      ULONGLONG generation);
static void idd_handle_resize_ack(VmDisplayIdd *d,
                                  const AsbDisplayControl *control,
                                  ULONGLONG connection_generation);
static BOOL idd_prepare_resize_send(VmDisplayIdd *d,
                                    const AsbDisplayControl *control,
                                    ULONGLONG generation);
static void idd_invalidate_resize_connection(VmDisplayIdd *d);
static DWORD WINAPI idd_control_send_thread_proc(LPVOID param);
static void window_to_vm_coords(HWND hwnd, int wx, int wy, UINT vm_w, UINT vm_h,
                                UINT *vx, UINT *vy);

/* ---- Window class ---- */

static const wchar_t *IDD_DISPLAY_CLASS = L"AppSandboxIddDisplay";
static const wchar_t *IDD_RENDER_CLASS  = L"AppSandboxIddRender";
static const wchar_t *IDD_LOG_CLASS     = L"AppSandboxIddLog";
static const wchar_t *IDD_TOOLBAR_CLASS = L"AppSandboxIddFullscreenToolbar";

/* System menu command IDs — must be < 0xF000 and have low 4 bits clear */
#define IDM_AUDIO_MUTE     0x1000
#define IDM_XMIT_HOTKEYS   0x1010
#define IDM_SHOW_LOG       0x1020
static BOOL g_idd_class_registered;
static WNDPROC g_orig_listbox_proc;
static SRWLOCK g_mouse_capture_lock = SRWLOCK_INIT;
static HWND g_mouse_capture_hwnd;

/* Listbox subclass — handles Ctrl+A / Ctrl+C */
static LRESULT CALLBACK idd_log_listbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (wp == 'C') {
            LRESULT sel_count = SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
            if (sel_count > 0) {
                int *indices = (int *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sel_count * sizeof(int));
                if (indices) {
                    LRESULT i;
                    SIZE_T total = 0;
                    wchar_t *text;
                    SendMessageW(hwnd, LB_GETSELITEMS, (WPARAM)sel_count, (LPARAM)indices);
                    for (i = 0; i < sel_count; i++)
                        total += (SIZE_T)SendMessageW(hwnd, LB_GETTEXTLEN, (WPARAM)indices[i], 0) + 2;
                    total++;
                    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(wchar_t));
                    if (text) {
                        wchar_t *p = text;
                        for (i = 0; i < sel_count; i++) {
                            LRESULT len = SendMessageW(hwnd, LB_GETTEXT, (WPARAM)indices[i], (LPARAM)p);
                            p += len;
                            *p++ = L'\r'; *p++ = L'\n';
                        }
                        if (OpenClipboard(hwnd)) {
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                            if (hMem) {
                                void *dst = GlobalLock(hMem);
                                if (dst) {
                                    memcpy(dst, text, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                                    GlobalUnlock(hMem);
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                } else {
                                    GlobalFree(hMem);
                                }
                            }
                            CloseClipboard();
                        }
                        HeapFree(GetProcessHeap(), 0, text);
                    }
                    HeapFree(GetProcessHeap(), 0, indices);
                }
            }
            return 0;
        }
        if (wp == 'A') {
            LRESULT cnt = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            SendMessageW(hwnd, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, (WORD)(cnt - 1)));
            return 0;
        }
    }
    return CallWindowProcW(g_orig_listbox_proc, hwnd, msg, wp, lp);
}

/* Log window proc — resizes listbox child to fill the window */
static LRESULT CALLBACK idd_log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH s_dark_brush = NULL;

    switch (msg) {
    case WM_SIZE: {
        HWND list = GetDlgItem(hwnd, IDC_LOG_LIST);
        if (list) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(list, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* Match AppSandbox #log-panel: --ctrl-bg #2d2d2d, text #ccc */
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(204, 204, 204));
        SetBkColor(hdc, RGB(45, 45, 45));
        if (!s_dark_brush) s_dark_brush = CreateSolidBrush(RGB(45, 45, 45));
        return (LRESULT)s_dark_brush;
    }
    case WM_CLOSE:
        /* Just hide — the main IDD window owns the lifecycle */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Render child window proc — forwards input + paint to parent for handling */
static LRESULT CALLBACK idd_render_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        /* Validate this window's update region; repaint once for expose/resize
           (rendering is otherwise push-driven by received frames). */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        SendMessageW(GetParent(hwnd), WM_IDD_FRAME_READY, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSELEAVE:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SETCURSOR:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Mouse-only host overlay.  This is intentionally not a standard BUTTON:
   clicking it must not move keyboard focus away from the IDD window while
   fullscreen input is being captured for the guest. */
static LRESULT CALLBACK idd_fullscreen_toolbar_proc(HWND hwnd, UINT msg,
                                                     WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        d = (VmDisplayIdd *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
    }

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;

    case WM_MOUSEMOVE:
        if (d) {
            d->toolbar_leave_tick = 0;
            idd_show_fullscreen_toolbar(d);
        }
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return 0;

    case WM_LBUTTONDOWN:
        if (d) {
            d->fullscreen_toolbar_pressed = TRUE;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (d) {
            POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
            RECT rc;
            GetClientRect(hwnd, &rc);
            d->fullscreen_toolbar_pressed = FALSE;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            if (PtInRect(&rc, pt) && d->fullscreen)
                idd_exit_fullscreen(d);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (d) {
            d->fullscreen_toolbar_pressed = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        /* Never let wheel input bubble from the host overlay to the guest. */
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HBRUSH bg, border;
        HDC hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        bg = CreateSolidBrush(d && d->fullscreen_toolbar_pressed
                                  ? RGB(64, 64, 68) : RGB(45, 45, 48));
        border = CreateSolidBrush(RGB(130, 130, 136));
        FillRect(hdc, &rc, bg);
        FrameRect(hdc, &rc, border);
        DeleteObject(bg);
        DeleteObject(border);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(240, 240, 242));
        DrawTextW(hdc, L"Exit Fullscreen", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_idd_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    if (g_idd_class_registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_wnd_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_DISPLAY_CLASS;
    RegisterClassExW(&wc);

    /* Render child — D3D11 swap chain targets this window.
       hCursor=NULL so WM_SETCURSOR can set the guest cursor. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_render_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = IDD_RENDER_CLASS;
    RegisterClassExW(&wc);

    /* Separate log window — dark background to match AppSandbox main window */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = idd_log_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = IDD_LOG_CLASS;
    RegisterClassExW(&wc);

    idd_register_fullscreen_toolbar_class(hInst);
    g_idd_class_registered = TRUE;
}



/* Mouse-only host overlay used by borderless fullscreen. */
static void idd_register_fullscreen_toolbar_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = idd_fullscreen_toolbar_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = IDD_TOOLBAR_CLASS;
    RegisterClassExW(&wc);
}


/* ---- Debug log panel ---- */

static void idd_log(VmDisplayIdd *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    LRESULT count;

    if (!d || !d->log_list_hwnd || d->stop) return;

    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);

    count = SendMessageW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf);

    /* Trim old entries */
    while (count > MAX_LOG_LINES) {
        SendMessageW(d->log_list_hwnd, LB_DELETESTRING, 0, 0);
        count--;
    }

    /* Scroll to bottom and force repaint even when not focused */
    SendMessageW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
    UpdateWindow(d->log_list_hwnd);
}

/* ---- Send input packet to guest ---- */

static BOOL send_input_locked(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    SOCKET s;
    int ret;

    s = d->input_socket;
    if (s == INVALID_SOCKET) return FALSE;

    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    /* Non-blocking send — drop packet if buffer full rather than stall the UI */
    ret = send(s, (const char *)&pkt, (int)sizeof(pkt), 0);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return FALSE;
        }
        idd_log(d, L"INPUT SEND ERR %d - flagging for reconnect.", err);
        /* Mark dead — recv thread owns the socket and will close + reconnect */
        d->input_socket = INVALID_SOCKET;
        PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
        return FALSE;
    }
    if (ret != (int)sizeof(pkt)) {
        /* Partial send on a stream socket: the guest reads fixed-size 20-byte
           InputPackets, so a truncated packet permanently misaligns the wire.
           Flag for reconnect so the channel resynchronises. */
        idd_log(d, L"INPUT SEND short (%d/%d) - flagging for reconnect.",
                ret, (int)sizeof(pkt));
        d->input_socket = INVALID_SOCKET;
        PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
        return FALSE;
    }

    return TRUE;
}

static void send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    AcquireSRWLockExclusive(&d->input_lock);
    send_input_locked(d, type, p1, p2, p3);
    ReleaseSRWLockExclusive(&d->input_lock);
}

static BOOL idd_clip_mouse(VmDisplayIdd *d)
{
    RECT rc;
    if (!GetClientRect(d->render_hwnd, &rc) || IsRectEmpty(&rc)) return FALSE;
    MapWindowPoints(d->render_hwnd, NULL, (POINT *)&rc, 2);
    return ClipCursor(&rc);
}

static void idd_cancel_mouse_sync(VmDisplayIdd *d)
{
    POINT point;
    if (!d->mouse_sync_pending) return;
    d->mouse_sync_pending = FALSE;
    if (GetForegroundWindow() == d->hwnd && GetCursorPos(&point) &&
        WindowFromPoint(point) == d->render_hwnd)
        SetCursor(!d->cursor_visible ? NULL :
                  d->guest_cursor ? d->guest_cursor : LoadCursorW(NULL, IDC_ARROW));
}

static UINT idd_dip_to_px(HWND hwnd, int dip)
{
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    UINT px;
    if (!dpi) dpi = 96;
    px = (UINT)MulDiv(dip, (int)dpi, 96);
    return px ? px : 1;
}

/* While the overlay is visible, keep the whole top strip out of relative
   capture.  This lets the pointer travel from the render surface into the
   toolbar even when the guest cursor is hidden. */
static BOOL idd_point_in_fullscreen_toolbar_region(VmDisplayIdd *d, POINT pt)
{
    RECT rc;
    int top;
    if (!d->fullscreen || !d->fullscreen_toolbar_visible || !d->hwnd)
        return FALSE;
    if (!GetClientRect(d->hwnd, &rc) || !ScreenToClient(d->hwnd, &pt))
        return FALSE;
    top = (int)idd_dip_to_px(d->hwnd, TOOLBAR_HEIGHT_DIP + TOOLBAR_EDGE_MARGIN_DIP);
    return pt.x >= 0 && pt.x < rc.right && pt.y >= 0 && pt.y <= top;
}

/* Temporarily release raw/relative capture without changing the user's
   relative-mouse preference.  Used when the host overlay or fullscreen
   transition needs the real cursor back. */
static void idd_suspend_relative_mouse_capture(VmDisplayIdd *d)
{
    AcquireSRWLockExclusive(&g_mouse_capture_lock);
    if (g_mouse_capture_hwnd == d->hwnd) {
        RAWINPUTDEVICE mouse = { 0x01, 0x02, RIDEV_REMOVE, NULL };
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        ClipCursor(NULL);
        g_mouse_capture_hwnd = NULL;
    }
    d->relative_mouse = FALSE;
    d->raw_absolute_valid = FALSE;
    ReleaseSRWLockExclusive(&g_mouse_capture_lock);
    idd_cancel_mouse_sync(d);
}

static void idd_update_relative_mouse(VmDisplayIdd *d)
{
    POINT pt = {0};
    BOOL active = !d->stop && d->frame_connected &&
                   d->mouse_version == INPUT_MOUSE_VERSION &&
                   d->input_socket != INVALID_SOCKET && d->input_focused &&
                   !d->input_menu_active && !d->input_sizing &&
                   GetForegroundWindow() == d->hwnd && !IsIconic(d->hwnd) &&
                   GetCursorPos(&pt) && WindowFromPoint(pt) == d->render_hwnd &&
                   !idd_point_in_fullscreen_toolbar_region(d, pt);
    BOOL capture = active && !d->cursor_visible;
    BOOL sync_absolute = FALSE;
    if (!active || capture) idd_cancel_mouse_sync(d);
    AcquireSRWLockExclusive(&g_mouse_capture_lock);
    if (capture == d->relative_mouse &&
        (!capture || g_mouse_capture_hwnd == d->hwnd)) {
        ReleaseSRWLockExclusive(&g_mouse_capture_lock);
        return;
    }
    if (capture) {
        RAWINPUTDEVICE mouse = { 0x01, 0x02, 0, d->hwnd };
        d->relative_mouse = FALSE;
        if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
            if (idd_clip_mouse(d)) {
                g_mouse_capture_hwnd = d->hwnd;
                d->relative_mouse = TRUE;
                d->mouse_in = TRUE;
                d->raw_absolute_valid = FALSE;
                SetCursor(NULL);
            } else {
                mouse.dwFlags = RIDEV_REMOVE;
                mouse.hwndTarget = NULL;
                RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            }
        }
    } else {
        if (g_mouse_capture_hwnd == d->hwnd) {
            sync_absolute = d->relative_mouse && active && d->cursor_visible;
            RAWINPUTDEVICE mouse = { 0x01, 0x02, RIDEV_REMOVE, NULL };
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
            ClipCursor(NULL);
            g_mouse_capture_hwnd = NULL;
        }
        d->relative_mouse = FALSE;
        d->raw_absolute_valid = FALSE;
    }
    ReleaseSRWLockExclusive(&g_mouse_capture_lock);
    if (sync_absolute && d->frame_width && d->frame_height) {
        if (d->mouse_version == INPUT_MOUSE_VERSION) {
            if (++d->mouse_sync_id == 0) ++d->mouse_sync_id;
            d->mouse_sync_origin = pt;
            d->mouse_sync_deadline = GetTickCount64() + 250;
            AcquireSRWLockExclusive(&d->input_lock);
            d->mouse_sync_socket = d->input_socket;
            d->mouse_sync_pending = send_input_locked(d, INPUT_MOUSE_POSITION_QUERY,
                                                      d->mouse_sync_id, 0, 0);
            ReleaseSRWLockExclusive(&d->input_lock);
            if (d->mouse_sync_pending) {
                SetCursor(NULL);
                return;
            }
        }
        idd_resume_absolute_mouse(d, NULL);
    }
}

static void idd_flush_mouse_buttons(VmDisplayIdd *d)
{
    for (UINT button = INPUT_BTN_LEFT; button <= INPUT_BTN_MIDDLE; button++) {
        if (d->mouse_buttons & (1u << button))
            send_input(d, INPUT_MOUSE_BUTTON, button, 0, 0);
    }
    d->mouse_buttons = 0;
    if (GetCapture() == d->render_hwnd) ReleaseCapture();
}

/* ==================================================================
 * Per-VM display settings (display_settings.json beside disk.vhdx)
 *
 * Mirrors the vm_state.json pattern in asb_core.c but is owned entirely
 * by the IDD display: the file is created lazily the first time a VM's
 * display opens, so both new and pre-existing VMs get one on demand.
 * ================================================================== */

static void idd_display_settings_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_chars)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_chars, L"%s\\display_settings.json", dir);
}

static void idd_display_settings_save(const wchar_t *vhdx_path, BOOL transmit_hotkeys)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"transmitKeyboardHotkeys\":%d}\n", transmit_hotkeys ? 1 : 0);
    fclose(f);
}

/* Read the persisted setting; if the file is absent, create it with the
   default (off) and return FALSE. Returns the transmit-hotkeys value. */
static BOOL idd_display_settings_load_or_create(const wchar_t *vhdx_path)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    BOOL transmit = FALSE;

    if (!vhdx_path || vhdx_path[0] == L'\0') return FALSE;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);

    if (_wfopen_s(&f, path, L"r") != 0 || !f) {
        /* Lazy creation: file doesn't exist yet (new or pre-existing VM). */
        idd_display_settings_save(vhdx_path, FALSE);
        return FALSE;
    }
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"transmitKeyboardHotkeys\":1"))
            transmit = TRUE;
    }
    fclose(f);
    return transmit;
}

/* ==================================================================
 * Keyboard hotkey capture
 * ================================================================== */

/* Host shortcuts withheld from the guest in Default mode.
   alt_down reflects GetKeyState(VK_MENU) in the window procedure. */
static BOOL idd_is_reserved_hotkey(DWORD vk, BOOL alt_down)
{
    switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SNAPSHOT:   /* PrintScreen */
    case VK_APPS:       /* Menu / context key */
        return TRUE;
    case VK_TAB:
        return alt_down;                                  /* Alt+Tab */
    case VK_ESCAPE:
        /* Alt+Esc / Ctrl+Esc. GetAsyncKeyState reads real-time physical state,
           which is reliable both in the wndproc and inside the low-level hook
           (where the synchronized GetKeyState value may not be updated yet). */
        return alt_down || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    default:
        return FALSE;
    }
}

static BOOL idd_capture_all_keys(const VmDisplayIdd *d)
{
    return d->fullscreen || d->transmit_hotkeys;
}

/* Forward a key event to the guest and track held state so a later focus
   change can release anything still down. Runs on the window thread only. */
static void idd_forward_key(VmDisplayIdd *d, DWORD vk, DWORD scan, BOOL ext, BOOL up)
{
    UINT32 type = INPUT_KEY, index = vk;
    InputPacket pkt;
    AcquireSRWLockExclusive(&d->input_lock);
    if (d->keyboard_version == INPUT_KEYBOARD_VERSION) {
        type = INPUT_KEY_PHYSICAL;
        if (vk == VK_PAUSE || vk == VK_CANCEL) { scan = 0; ext = FALSE; }
        /* Windows flags right Shift and Num Lock as extended without a physical E0 prefix. */
        if (scan == 0x36 || scan == 0x45) ext = FALSE;
        if (vk == VK_PACKET || scan > 255) goto done;
        if (scan == 0 && vk != VK_CANCEL && vk != VK_PAUSE &&
            vk != VK_SNAPSHOT && vk != VK_SLEEP &&
            !(vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2)) goto done;
        index = scan ? scan + (ext ? 256 : 0) : 512 + vk;
    }
    pkt.magic = INPUT_MAGIC;
    pkt.type = type;
    pkt.param1 = vk;
    pkt.param2 = scan;
    pkt.param3 = ext ? INPUT_KEY_EXTENDED : 0;
    if (type == INPUT_KEY_PHYSICAL &&
        index < ARRAYSIZE(d->held_down) && d->held_down[index])
        pkt = d->held_keys[index];
    if (type == INPUT_KEY_PHYSICAL && up && (scan == 0xF1 || scan == 0xF2) &&
        !d->held_down[index]) {
        /* Some Korean keyboard drivers report only the key release. */
        if (!send_input_locked(d, pkt.type, pkt.param1, pkt.param2, pkt.param3)) goto done;
        d->held_down[index] = 1;
        d->held_keys[index] = pkt;
    }
    if (up) pkt.param3 |= INPUT_KEY_UP;
    if (send_input_locked(d, pkt.type, pkt.param1, pkt.param2, pkt.param3) &&
        index < ARRAYSIZE(d->held_down)) {
        d->held_down[index] = !up;
        if (!up) d->held_keys[index] = pkt;
    }
done:
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* Send key-up for every key we believe is still held in the guest, then
   clear tracking. Called when our window loses activation, when Transmit
   mode is turned off, and on teardown. */
static void idd_flush_held_keys(VmDisplayIdd *d)
{
    AcquireSRWLockExclusive(&d->input_lock);
    for (int i = 0; i < ARRAYSIZE(d->held_down); i++) {
        if (d->held_down[i]) {
            const InputPacket *pkt = &d->held_keys[i];
            send_input_locked(d, pkt->type, pkt->param1, pkt->param2,
                              pkt->param3 | INPUT_KEY_UP);
            d->held_down[i] = 0;
        }
    }
    ReleaseSRWLockExclusive(&d->input_lock);
}

/* Thread-local owner: a WH_KEYBOARD_LL callback runs on the thread that
   installed it (our window thread), so this resolves each display's context
   without a global registry, keeping multiple displays independent. */
static __declspec(thread) VmDisplayIdd *t_hook_display;

enum { KEY_ROUTE_HOST = 1, KEY_ROUTE_GUEST, KEY_ROUTE_BOTH };

static BOOL idd_is_modifier(DWORD vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU);
}

static LRESULT CALLBACK idd_ll_keyboard_proc(int code, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = t_hook_display;
    if (code == HC_ACTION && d && !d->stop) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        BOOL ext = (k->flags & LLKHF_EXTENDED) != 0;
        BOOL focused = d->input_focused && GetForegroundWindow() == d->hwnd;
        if (focused && d->suppress_f11_up && up && k->vkCode == VK_F11) {
            d->suppress_f11_up = FALSE;
            return 1;
        }
        if (d->keyboard_version != INPUT_KEYBOARD_VERSION) {
            if (focused && idd_capture_all_keys(d) &&
                (d->fullscreen || k->vkCode != VK_F11)) {
                idd_forward_key(d, k->vkCode, k->scanCode, ext, up);
                return 1;
            }
        } else {
            DWORD scan = (k->vkCode == VK_PAUSE || k->vkCode == VK_CANCEL)
                         ? 0 : k->scanCode;
            if (scan == 0x36 || scan == 0x45) ext = FALSE;
            if (scan > 255 || k->vkCode > 255) return CallNextHookEx(NULL, code, wp, lp);
            UINT index = scan ? scan + (ext ? 256 : 0) : 512 + k->vkCode;
            BYTE route = d->hook_routes[index];
            BOOL pulse = k->vkCode == VK_PAUSE || k->vkCode == VK_CANCEL;
            if (!focused || d->input_menu_active) {
                d->hook_routes[index] = up || pulse ? 0 : KEY_ROUTE_HOST;
                return CallNextHookEx(NULL, code, wp, lp);
            }
            if (!route) {
                if (up && scan != 0xF1 && scan != 0xF2)
                    return CallNextHookEx(NULL, code, wp, lp);
                if (!d->fullscreen && k->vkCode == VK_F11) {
                    route = KEY_ROUTE_HOST;
                } else if (idd_capture_all_keys(d)) {
                    route = KEY_ROUTE_GUEST;
                } else if (idd_is_reserved_hotkey(k->vkCode, (k->flags & LLKHF_ALTDOWN) != 0) ||
                           (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                    route = KEY_ROUTE_HOST;
                } else {
                    route = idd_is_modifier(k->vkCode) ? KEY_ROUTE_BOTH : KEY_ROUTE_GUEST;
                }
            }
            /* Releases and repeats follow the key-down destination even if modifiers changed. */
            d->hook_routes[index] = up || pulse ? 0 : route;
            if (route != KEY_ROUTE_HOST)
                idd_forward_key(d, k->vkCode, k->scanCode, ext, up);
            if (route == KEY_ROUTE_GUEST) return 1;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void idd_install_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) return;
    ZeroMemory(d->hook_routes, sizeof(d->hook_routes));
    AcquireSRWLockShared(&d->input_lock);
    if (d->keyboard_version == INPUT_KEYBOARD_VERSION) {
        for (UINT i = 0; i < ARRAYSIZE(d->held_down); i++)
            if (d->held_down[i]) d->hook_routes[i] = KEY_ROUTE_BOTH;
    }
    ReleaseSRWLockShared(&d->input_lock);
    t_hook_display = d;
    d->kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, idd_ll_keyboard_proc,
                                     d->hInstance, 0);
    if (!d->kbd_hook)
        idd_log(d, L"Hotkey hook install failed (err %lu).", GetLastError());
    else
        idd_log(d, L"Hotkey capture enabled.");
}

static void idd_remove_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) {
        UnhookWindowsHookEx(d->kbd_hook);
        d->kbd_hook = NULL;
        idd_log(d, L"Hotkey capture disabled.");
    }
    t_hook_display = NULL;
}

static void idd_update_kbd_hook(VmDisplayIdd *d)
{
    if (idd_capture_all_keys(d) || d->keyboard_version == INPUT_KEYBOARD_VERSION)
        idd_install_kbd_hook(d);
    else
        idd_remove_kbd_hook(d);
}

/* Compute letterboxed/pillarboxed viewport within client rect */
static void compute_letterbox(UINT client_w, UINT client_h,
                              UINT frame_w, UINT frame_h,
                              float *out_x, float *out_y,
                              float *out_w, float *out_h)
{
    float scale_x, scale_y, scale;
    if (client_w == 0 || client_h == 0 || frame_w == 0 || frame_h == 0) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return;
    }
    scale_x = (float)client_w / (float)frame_w;
    scale_y = (float)client_h / (float)frame_h;
    scale = scale_x < scale_y ? scale_x : scale_y;
    *out_w = (float)frame_w * scale;
    *out_h = (float)frame_h * scale;
    *out_x = ((float)client_w - *out_w) * 0.5f;
    *out_y = ((float)client_h - *out_h) * 0.5f;
}

/* Map window client coordinates to VM framebuffer coordinates */
static void window_to_vm_coords(HWND hwnd, int wx, int wy,
                                 UINT vm_w, UINT vm_h,
                                 UINT *vx, UINT *vy)
{
    RECT rc;
    float vp_x, vp_y, vp_w, vp_h;
    float local_x, local_y;

    GetClientRect(hwnd, &rc);
    compute_letterbox((UINT)rc.right, (UINT)rc.bottom, vm_w, vm_h,
                      &vp_x, &vp_y, &vp_w, &vp_h);

    if (vp_w <= 0 || vp_h <= 0) {
        *vx = 0;
        *vy = 0;
        return;
    }

    local_x = ((float)wx - vp_x) / vp_w * (float)vm_w;
    local_y = ((float)wy - vp_y) / vp_h * (float)vm_h;

    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    *vx = (UINT)local_x;
    *vy = (UINT)local_y;
    if (*vx >= vm_w) *vx = vm_w - 1;
    if (*vy >= vm_h) *vy = vm_h - 1;
}

static void idd_resume_absolute_mouse(VmDisplayIdd *d, const InputPacket *reply)
{
    POINT point;
    UINT vx, vy;
    if (d->stop || !d->frame_connected || !d->input_focused ||
        d->input_menu_active || d->input_sizing || d->relative_mouse ||
        !d->cursor_visible || !d->frame_width || !d->frame_height ||
        GetForegroundWindow() != d->hwnd || IsIconic(d->hwnd) ||
        !GetCursorPos(&point) || WindowFromPoint(point) != d->render_hwnd) {
        idd_cancel_mouse_sync(d);
        return;
    }

    if (reply && reply->param1 < d->frame_width && reply->param2 < d->frame_height) {
        RECT rc;
        float x, y, width, height;
        LONG dx = point.x - d->mouse_sync_origin.x;
        LONG dy = point.y - d->mouse_sync_origin.y;
        if (GetClientRect(d->render_hwnd, &rc) && !IsRectEmpty(&rc)) {
            compute_letterbox(rc.right, rc.bottom, d->frame_width, d->frame_height,
                              &x, &y, &width, &height);
            POINT target = {
                (LONG)(x + (reply->param1 + 0.5f) * width / d->frame_width) + dx,
                (LONG)(y + (reply->param2 + 0.5f) * height / d->frame_height) + dy
            };
            if (target.x < (LONG)x) target.x = (LONG)x;
            if (target.y < (LONG)y) target.y = (LONG)y;
            if (target.x >= (LONG)(x + width)) target.x = (LONG)(x + width) - 1;
            if (target.y >= (LONG)(y + height)) target.y = (LONG)(y + height) - 1;
            if (ClientToScreen(d->render_hwnd, &target)) {
                SetCursorPos(target.x, target.y);
                GetCursorPos(&point);
            }
        }
    }
    d->mouse_sync_pending = FALSE;
    ScreenToClient(d->render_hwnd, &point);
    window_to_vm_coords(d->render_hwnd, point.x, point.y,
                        d->frame_width, d->frame_height, &vx, &vy);
    send_input(d, INPUT_MOUSE_MOVE, vx, vy, 0);
    SetCursor(d->guest_cursor ? d->guest_cursor : LoadCursorW(NULL, IDC_ARROW));
}

static void idd_poll_mouse_position(VmDisplayIdd *d)
{
    InputPacket reply;
    BOOL received = FALSE, disconnected = FALSE;
    if (!d->mouse_sync_pending) return;
    idd_update_relative_mouse(d);
    if (!d->mouse_sync_pending) return;
    AcquireSRWLockExclusive(&d->input_lock);
    if (d->input_socket != INVALID_SOCKET && d->input_socket == d->mouse_sync_socket) {
        for (int i = 0; i < 16; i++) {
            int n = recv(d->input_socket, (char *)&d->mouse_reply + d->mouse_reply_size,
                         (int)sizeof(InputPacket) - d->mouse_reply_size, 0);
            if (n <= 0) {
                disconnected = n == 0 || WSAGetLastError() != WSAEWOULDBLOCK;
                if (disconnected) {
                    d->input_socket = INVALID_SOCKET;
                    PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
                }
                break;
            }
            d->mouse_reply_size += n;
            if (d->mouse_reply_size != sizeof(InputPacket)) continue;
            d->mouse_reply_size = 0;
            if (d->mouse_reply.magic == INPUT_MAGIC &&
                d->mouse_reply.type == INPUT_MOUSE_POSITION_REPLY &&
                d->mouse_reply.param3 == d->mouse_sync_id) {
                reply = d->mouse_reply;
                received = TRUE;
                break;
            }
        }
    } else {
        disconnected = TRUE;
    }
    ReleaseSRWLockExclusive(&d->input_lock);
    if (disconnected)
        idd_cancel_mouse_sync(d);
    else if (received)
        idd_resume_absolute_mouse(d, &reply);
    else if (GetTickCount64() >= d->mouse_sync_deadline)
        idd_resume_absolute_mouse(d, NULL);
}

/* ---- Reliable recv: read exactly `len` bytes ---- */

static BOOL recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_hv_service(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family   = AF_HYPERV;
    addr.VmId     = *vm_runtime_id;
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with recv timeout */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

static BOOL idd_send_display_control(VmDisplayIdd *d,
                                      const AsbDisplayControl *control,
                                      ULONGLONG generation)
{
    SOCKET s;
    ULONGLONG current_generation;
    int sent = 0;
    BOOL ok = FALSE;

    if (!d || !control ||
        InterlockedCompareExchange(&d->resize_capable, 0, 0) == 0)
        return FALSE;

    /* Prepare before taking frame_send_lock so the lock order remains
     * resize_lock -> frame_send_lock, matching idd_flush_resize(). */
    if (control->type == ASB_DISPLAY_CONTROL_RESIZE_REQUEST &&
        !idd_prepare_resize_send(d, control, generation)) {
        idd_log(d, L"display_control resize state was superseded before send.");
        return FALSE;
    }

    AcquireSRWLockShared(&d->frame_send_lock);
    s = d->frame_socket;
    current_generation = d->frame_connection_generation;
    if (s != INVALID_SOCKET && generation != 0 &&
        generation == current_generation) {
        DWORD timeout = 200;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout,
                   sizeof(timeout));
        while (sent < (int)sizeof(*control)) {
            int n = send(s, (const char *)control + sent,
                         (int)sizeof(*control) - sent, 0);
            if (n <= 0) break;
            sent += n;
        }
        ok = sent == (int)sizeof(*control);
        if (!ok)
            shutdown(s, SD_BOTH);
    }
    ReleaseSRWLockShared(&d->frame_send_lock);
    if (!ok)
        idd_log(d, L"display_control send failed; frame channel will reconnect.");
    return ok;
}

static void idd_requeue_failed_resize(VmDisplayIdd *d,
                                      const AsbDisplayControl *control,
                                      ULONGLONG generation)
{
    ULONGLONG current_generation = 0;

    if (!d || !control || control->type != ASB_DISPLAY_CONTROL_RESIZE_REQUEST)
        return;

    AcquireSRWLockShared(&d->frame_send_lock);
    current_generation = d->frame_connection_generation;
    ReleaseSRWLockShared(&d->frame_send_lock);
    if (generation != 0 && generation != current_generation)
        return;

    AcquireSRWLockExclusive(&d->resize_lock);
    if (d->pending_resize_id == control->request_id &&
        (generation == 0 || d->pending_resize_generation == generation)) {
        d->pending_resize_width = 0;
        d->pending_resize_height = 0;
        d->pending_resize_generation = 0;
        d->pending_resize_deadline = 0;
        d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;
        d->resize_queued = TRUE;
        d->queued_resize_width = d->desired_width;
        d->queued_resize_height = d->desired_height;
        d->pending_resize_id = 0;
    }
    ReleaseSRWLockExclusive(&d->resize_lock);
}

/* A resize request, accepted target, and actual frame belong to one frame
 * connection.  Invalidate all connection-scoped state before the next HELLO;
 * desired_* is deliberately retained so HELLO can synchronize it again. */
static void idd_invalidate_resize_connection(VmDisplayIdd *d)
{
    BOOL fail_runtime = FALSE;
    if (!d) return;

    AcquireSRWLockExclusive(&d->resize_lock);
    fail_runtime = d->runtime_request_active;
    d->actual_valid = FALSE;
    d->force_resize_sync = FALSE;
    d->actual_generation = 0;
    d->pending_resize_width = 0;
    d->pending_resize_height = 0;
    d->pending_resize_id = 0;
    d->pending_resize_generation = 0;
    d->pending_resize_deadline = 0;
    d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;
    d->pending_resize_retry_count = 0;
    d->resize_queued = FALSE;
    d->queued_resize_width = 0;
    d->queued_resize_height = 0;
    ReleaseSRWLockExclusive(&d->resize_lock);

    AcquireSRWLockExclusive(&d->control_queue_lock);
    d->control_queued = FALSE;
    d->queued_control_generation = 0;
    ReleaseSRWLockExclusive(&d->control_queue_lock);

    /* A frame/control disconnect is terminal for the explicit UI request.
     * Reconnect may still synchronize the persisted target once HELLO
     * returns, but it must not revive a request already reported as failed. */
    if (fail_runtime)
        idd_post_runtime_result(d, FALSE, 0, 0);
}

static BOOL idd_queue_display_control(VmDisplayIdd *d,
                                       const AsbDisplayControl *control)
{
    ULONGLONG generation;
    SOCKET s;

    if (!d || !control || !d->control_send_event)
        return FALSE;

    AcquireSRWLockShared(&d->frame_send_lock);
    s = d->frame_socket;
    generation = d->frame_connection_generation;
    ReleaseSRWLockShared(&d->frame_send_lock);
    if (s == INVALID_SOCKET || generation == 0 ||
        InterlockedCompareExchange(&d->resize_capable, 0, 0) == 0)
        return FALSE;

    AcquireSRWLockExclusive(&d->control_queue_lock);
    d->queued_control = *control;
    d->queued_control_generation = generation;
    d->control_queued = TRUE;
    ReleaseSRWLockExclusive(&d->control_queue_lock);
    SetEvent(d->control_send_event);
    return TRUE;
}

static DWORD WINAPI idd_control_send_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;

    while (!d->control_send_stop) {
        AsbDisplayControl control;
        ULONGLONG generation = 0;
        BOOL have_control = FALSE;

        WaitForSingleObject(d->control_send_event, 1000);
        if (d->control_send_stop) break;
        AcquireSRWLockExclusive(&d->control_queue_lock);
        if (d->control_queued) {
            control = d->queued_control;
            generation = d->queued_control_generation;
            d->control_queued = FALSE;
            d->queued_control_generation = 0;
            have_control = TRUE;
        }
        ReleaseSRWLockExclusive(&d->control_queue_lock);
        if (!have_control) continue;

        if (!idd_send_display_control(d, &control, generation))
            idd_requeue_failed_resize(d, &control, generation);
        else if (control.type == ASB_DISPLAY_CONTROL_RESIZE_REQUEST) {
            idd_log(d, L"display_resize sent id=%u target=%ux%u "
                    L"phase=wait_ack", control.request_id,
                    control.width, control.height);
        }
    }
    return 0;
}

/* A request becomes eligible for ACK handling immediately before the first
 * control byte is written. The guest may ACK before the final send() call
 * returns, so this transition must happen before the write, not after it. */
static BOOL idd_prepare_resize_send(VmDisplayIdd *d,
                                    const AsbDisplayControl *control,
                                    ULONGLONG generation)
{
    ULONGLONG now;
    BOOL prepared = FALSE;

    if (!d || !control || control->type != ASB_DISPLAY_CONTROL_RESIZE_REQUEST)
        return TRUE;

    now = GetTickCount64();
    AcquireSRWLockExclusive(&d->resize_lock);
    if (d->pending_resize_id == control->request_id &&
        d->pending_resize_generation == generation &&
        d->pending_resize_phase == DISPLAY_RESIZE_PHASE_NONE) {
        d->pending_resize_phase = DISPLAY_RESIZE_PHASE_WAIT_ACK;
        d->pending_resize_deadline = now + ASB_DISPLAY_RESIZE_ACK_TIMEOUT_MS;
        prepared = TRUE;
    }
    ReleaseSRWLockExclusive(&d->resize_lock);
    return prepared;
}

static void idd_flush_resize(VmDisplayIdd *d)
{
    AsbDisplayControl control;
    UINT width, height;
    UINT32 request_id;
    ULONGLONG generation;
    ULONGLONG now;

    if (!d || InterlockedCompareExchange(&d->resize_capable, 0, 0) == 0)
        return;
    now = GetTickCount64();
    AcquireSRWLockExclusive(&d->resize_lock);
    if (!d->resize_queued || d->pending_resize_id != 0) {
        ReleaseSRWLockExclusive(&d->resize_lock);
        return;
    }
    width = d->queued_resize_width;
    height = d->queued_resize_height;
    request_id = (UINT32)InterlockedIncrement((volatile LONG *)&d->resize_request_id);
    d->resize_queued = FALSE;
    d->pending_resize_width = width;
    d->pending_resize_height = height;
    d->pending_resize_id = request_id;
    AcquireSRWLockShared(&d->frame_send_lock);
    generation = d->frame_connection_generation;
    ReleaseSRWLockShared(&d->frame_send_lock);
    d->pending_resize_generation = generation;
    d->pending_resize_deadline = 0;
    d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;
    d->force_resize_sync = FALSE;
    d->last_resize_send_tick = now;
    ReleaseSRWLockExclusive(&d->resize_lock);

    ZeroMemory(&control, sizeof(control));
    control.magic = ASB_DISPLAY_CONTROL_MAGIC;
    control.version = ASB_DISPLAY_CONTROL_VERSION;
    control.type = ASB_DISPLAY_CONTROL_RESIZE_REQUEST;
    control.request_id = request_id;
    control.width = width;
    control.height = height;
    control.refresh_hz = ASB_DISPLAY_CONTROL_DEFAULT_REFRESH;
    if (!idd_queue_display_control(d, &control)) {
        idd_requeue_failed_resize(d, &control, generation);
        idd_log(d, L"display_control sender unavailable; resize queued.");
        return;
    }
    if (d->hwnd)
        SetTimer(d->hwnd, IDT_DISPLAY_RESIZE,
                 DISPLAY_RESIZE_DEBOUNCE_MS, NULL);
}

static void idd_post_runtime_result(VmDisplayIdd *d, BOOL success,
                                    UINT width, UINT height)
{
    VmDisplayRuntimeResult *result;
    HWND notify;

    if (!d) return;
    AcquireSRWLockExclusive(&d->resize_lock);
    if (!d->runtime_request_active) {
        ReleaseSRWLockExclusive(&d->resize_lock);
        return;
    }
    if (!width) width = d->runtime_request_width;
    if (!height) height = d->runtime_request_height;
    d->runtime_request_active = FALSE;
    d->runtime_request_deadline = 0;
    d->pending_resize_width = 0;
    d->pending_resize_height = 0;
    d->pending_resize_id = 0;
    d->pending_resize_generation = 0;
    d->pending_resize_deadline = 0;
    d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;
    d->pending_resize_retry_count = 0;
    d->resize_queued = FALSE;
    d->queued_resize_width = 0;
    notify = d->runtime_notify_hwnd;
    if (!success && d->vm) {
        uint32_t configured_width = d->vm->display_width;
        uint32_t configured_height = d->vm->display_height;
        if (!asb_display_is_preset(configured_width, configured_height)) {
            configured_width = DEFAULT_WIDTH;
            configured_height = DEFAULT_HEIGHT;
        }
        /* A failed explicit request must not become the reconnect target. */
        d->desired_width = (UINT)configured_width;
        d->desired_height = (UINT)configured_height;
    }
    ReleaseSRWLockExclusive(&d->resize_lock);

    AcquireSRWLockExclusive(&d->control_queue_lock);
    d->control_queued = FALSE;
    d->queued_control_generation = 0;
    ReleaseSRWLockExclusive(&d->control_queue_lock);

    result = (VmDisplayRuntimeResult *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*result));
    if (!result) return;
    result->vm = d->vm;
    result->width = width;
    result->height = height;
    result->success = success;
    if (!notify || !PostMessageW(notify, WM_VM_DISPLAY_RUNTIME_RESULT,
                                 0, (LPARAM)result))
        HeapFree(GetProcessHeap(), 0, result);
}

static void idd_check_runtime_request_timeout(VmDisplayIdd *d)
{
    ULONGLONG now;
    ULONGLONG deadline = 0;
    BOOL expired = FALSE;
    UINT width = 0, height = 0;

    if (!d) return;
    now = GetTickCount64();
    AcquireSRWLockShared(&d->resize_lock);
    if (d->runtime_request_active && d->runtime_request_deadline &&
        now >= d->runtime_request_deadline) {
        expired = TRUE;
        deadline = d->runtime_request_deadline;
        width = d->runtime_request_width;
        height = d->runtime_request_height;
    }
    ReleaseSRWLockShared(&d->resize_lock);
    if (expired) {
        idd_log(d, L"display_resize request_timeout target=%ux%u deadline=%llu",
                width, height, deadline);
        idd_post_runtime_result(d, FALSE, width, height);
    }
}

static void idd_schedule_resize(VmDisplayIdd *d, UINT width, UINT height,
                                BOOL immediate)
{
    uint32_t normalized_width = width;
    uint32_t normalized_height = height;
    BOOL queue;
    ULONGLONG now;

    if (!d || width == 0 || height == 0) return;
    asb_display_normalize_resolution(&normalized_width, &normalized_height);

    AcquireSRWLockExclusive(&d->resize_lock);
    if (d->desired_width != normalized_width ||
        d->desired_height != normalized_height)
        d->pending_resize_retry_count = 0;
    d->desired_width = (UINT)normalized_width;
    d->desired_height = (UINT)normalized_height;
    queue = d->force_resize_sync || !d->actual_valid ||
            d->actual_width != d->desired_width ||
            d->actual_height != d->desired_height;
    if (queue) {
        d->queued_resize_width = d->desired_width;
        d->queued_resize_height = d->desired_height;
        d->resize_queued = TRUE;
    } else
        d->resize_queued = FALSE;
    ReleaseSRWLockExclusive(&d->resize_lock);

    if (!queue || InterlockedCompareExchange(&d->resize_capable, 0, 0) == 0) {
        AcquireSRWLockShared(&d->resize_lock);
        if (!d->pending_resize_id && d->hwnd)
            KillTimer(d->hwnd, IDT_DISPLAY_RESIZE);
        ReleaseSRWLockShared(&d->resize_lock);
        return;
    }
    now = GetTickCount64();
    if (immediate || now - d->last_resize_send_tick >= DISPLAY_RESIZE_DEBOUNCE_MS) {
        idd_flush_resize(d);
    } else if (d->hwnd) {
        SetTimer(d->hwnd, IDT_DISPLAY_RESIZE, DISPLAY_RESIZE_DEBOUNCE_MS, NULL);
    }
}

static void idd_handle_resize_ack(VmDisplayIdd *d,
                                  const AsbDisplayControl *control,
                                  ULONGLONG connection_generation)
{
    BOOL accepted;
    BOOL retry = FALSE;
    BOOL busy = FALSE;
    BOOL abandon = FALSE;
    BOOL disable_resize = FALSE;
    UINT32 pending_id;
    UINT retry_count = 0;
    ULONGLONG now;
    UINT failed_width = 0, failed_height = 0;

    if (!d || !control)
        return;

    if (control->request_id == 0 ||
        control->status_or_flags > ASB_DISPLAY_CONTROL_STATUS_BUSY) {
        idd_log(d, L"display_control invalid resize ack id=%u status=%u.",
                control->request_id, control->status_or_flags);
        return;
    }

    accepted = control->status_or_flags ==
        ASB_DISPLAY_CONTROL_STATUS_ACCEPTED;
    if (accepted &&
        (control->width < ASB_DISPLAY_CONTROL_MIN_WIDTH ||
         control->height < ASB_DISPLAY_CONTROL_MIN_HEIGHT ||
         control->width > ASB_DISPLAY_CONTROL_MAX_WIDTH ||
         control->height > ASB_DISPLAY_CONTROL_MAX_HEIGHT ||
         control->refresh_hz < 24 || control->refresh_hz > 240)) {
        idd_log(d, L"display_control invalid resize ack id=%u "
                L"target=%ux%u refresh=%u.", control->request_id,
                control->width, control->height, control->refresh_hz);
        return;
    }

    AcquireSRWLockExclusive(&d->resize_lock);
    pending_id = d->pending_resize_id;
    if (pending_id == 0 || pending_id != control->request_id ||
        d->pending_resize_generation != connection_generation ||
        d->pending_resize_phase != DISPLAY_RESIZE_PHASE_WAIT_ACK) {
        ReleaseSRWLockExclusive(&d->resize_lock);
        idd_log(d, L"display_resize stale_ack id=%u pending=%u",
                control->request_id, pending_id);
        return;
    }

    if (accepted) {
        /* ACK is acceptance only. Completion still requires an ASFR with
         * this normalized target. */
        d->pending_resize_width = control->width;
        d->pending_resize_height = control->height;
        now = GetTickCount64();
        d->pending_resize_phase = DISPLAY_RESIZE_PHASE_WAIT_FRAME;
        d->pending_resize_deadline =
            now + DISPLAY_RESIZE_COMPLETION_TIMEOUT_MS;
        ReleaseSRWLockExclusive(&d->resize_lock);
        if (d->hwnd)
            SetTimer(d->hwnd, IDT_DISPLAY_RESIZE,
                     DISPLAY_RESIZE_DEBOUNCE_MS, NULL);
        idd_log(d, L"display_resize ack id=%u status=accepted target=%ux%u",
                control->request_id, control->width, control->height);
        return;
    }

    retry_count = d->pending_resize_retry_count;
    failed_width = d->runtime_request_width ? d->runtime_request_width
                                            : d->desired_width;
    failed_height = d->runtime_request_height ? d->runtime_request_height
                                               : d->desired_height;
    d->pending_resize_width = 0;
    d->pending_resize_height = 0;
    d->pending_resize_id = 0;
    d->pending_resize_generation = 0;
    d->pending_resize_deadline = 0;
    d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;

    if ((control->status_or_flags == ASB_DISPLAY_CONTROL_STATUS_MODE_FAILED ||
         control->status_or_flags == ASB_DISPLAY_CONTROL_STATUS_BUSY) &&
        retry_count < DISPLAY_RESIZE_RETRY_LIMIT &&
        InterlockedCompareExchange(&d->resize_capable, 0, 0) != 0) {
        busy = control->status_or_flags == ASB_DISPLAY_CONTROL_STATUS_BUSY;
        /* BUSY means the guest's previous asynchronous modeset is still
         * running. Keep retry budget for actual timeout/failure cases and
         * poll again later instead of burning all retries in milliseconds. */
        d->pending_resize_retry_count = busy ? retry_count : retry_count + 1;
        d->queued_resize_width = d->desired_width;
        d->queued_resize_height = d->desired_height;
        d->resize_queued = TRUE;
        retry = TRUE;
    } else {
        d->pending_resize_retry_count = 0;
        d->resize_queued = FALSE;
        /* Any non-accepted terminal status completes the UI request. */
        abandon = TRUE;
        disable_resize = control->status_or_flags ==
            ASB_DISPLAY_CONTROL_STATUS_UNSUPPORTED;
    }
    ReleaseSRWLockExclusive(&d->resize_lock);

    if (disable_resize)
        InterlockedExchange(&d->resize_capable, 0);
    idd_log(d, L"display_resize ack_failed id=%u status=%u",
            control->request_id, control->status_or_flags);
    if (retry) {
        idd_log(d, L"display_resize retry id=%u target=%ux%u",
                control->request_id, d->desired_width, d->desired_height);
        if (d->hwnd) {
            if (busy)
                SetTimer(d->hwnd, IDT_DISPLAY_RESIZE, 250, NULL);
            else
                PostMessageW(d->hwnd, WM_IDD_RESIZE_RETRY, 0, 0);
        }
    } else if (abandon) {
        idd_log(d, L"display_resize abandoned target=%ux%u actual=%ux%u",
                d->desired_width, d->desired_height,
                d->actual_width, d->actual_height);
        idd_post_runtime_result(d, FALSE, failed_width, failed_height);
    }
}

static void idd_check_resize_timeout(VmDisplayIdd *d)
{
    ULONGLONG now;
    ULONGLONG deadline;
    UINT32 request_id;
    UINT retry_count;
    UINT requested_width, requested_height;
    DisplayResizePhase phase;
    UINT actual_width, actual_height;
    UINT failed_width, failed_height;
    BOOL retry = FALSE;
    BOOL abandon = FALSE;

    if (!d) return;
    now = GetTickCount64();

    idd_check_runtime_request_timeout(d);

    AcquireSRWLockExclusive(&d->resize_lock);
    if (!d->pending_resize_id ||
        d->pending_resize_phase == DISPLAY_RESIZE_PHASE_NONE ||
        !d->pending_resize_deadline ||
        now < d->pending_resize_deadline) {
        ReleaseSRWLockExclusive(&d->resize_lock);
        return;
    }

    request_id = d->pending_resize_id;
    deadline = d->pending_resize_deadline;
    retry_count = d->pending_resize_retry_count;
    requested_width = d->pending_resize_width;
    requested_height = d->pending_resize_height;
    failed_width = d->runtime_request_width ? d->runtime_request_width
                                            : d->desired_width;
    failed_height = d->runtime_request_height ? d->runtime_request_height
                                               : d->desired_height;
    phase = d->pending_resize_phase;
    actual_width = d->actual_width;
    actual_height = d->actual_height;
    d->pending_resize_id = 0;
    d->pending_resize_width = 0;
    d->pending_resize_height = 0;
    d->pending_resize_generation = 0;
    d->pending_resize_deadline = 0;
    d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;

    if (retry_count < DISPLAY_RESIZE_RETRY_LIMIT &&
        InterlockedCompareExchange(&d->resize_capable, 0, 0) != 0 &&
        (!d->actual_valid || actual_width != d->desired_width ||
         actual_height != d->desired_height)) {
        d->pending_resize_retry_count = retry_count + 1;
        d->queued_resize_width = d->desired_width;
        d->queued_resize_height = d->desired_height;
        d->resize_queued = TRUE;
        retry = TRUE;
    } else {
        d->pending_resize_retry_count = 0;
        d->resize_queued = FALSE;
        abandon = TRUE;
    }
    ReleaseSRWLockExclusive(&d->resize_lock);

    idd_log(d, phase == DISPLAY_RESIZE_PHASE_WAIT_ACK
               ? L"display_resize ack_timeout id=%u target=%ux%u actual=%ux%u "
                 L"retry=%u deadline=%llu"
               : L"display_resize frame_timeout id=%u target=%ux%u actual=%ux%u "
                 L"retry=%u deadline=%llu",
            request_id, requested_width, requested_height, actual_width,
            actual_height, retry_count, deadline);
    if (retry) {
        idd_log(d, L"display_resize retry id=%u target=%ux%u",
                request_id, d->desired_width, d->desired_height);
        idd_flush_resize(d);
    } else if (abandon) {
        idd_log(d, L"display_resize abandoned target=%ux%u actual=%ux%u",
                d->desired_width, d->desired_height,
                actual_width, actual_height);
        idd_post_runtime_result(d, FALSE, failed_width, failed_height);
    }
}

static SOCKET connect_input(VmDisplayIdd *d)
{
    static volatile LONG request_id;
    GUID svc;
    UINT32 ready = 0;
    InputPacket queries[] = {
        { INPUT_MAGIC, INPUT_KEYBOARD_QUERY, INPUT_KEYBOARD_VERSION, 0, 0 },
        { INPUT_MAGIC, INPUT_MOUSE_QUERY, INPUT_MOUSE_VERSION, 0, 0 }
    };
    InputPacket reply = {0};
    int got = 0, version = 1, mouse_version = 0;
    BOOL keyboard_reply = FALSE, mouse_reply = FALSE;
    ULONGLONG deadline;
    u_long nonblock = 1;
    DWORD zero_timeout = 0;
    hcs_service_guid(d->os_type, 3, &svc);
    SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
    if (s == INVALID_SOCKET) return s;
    if (!recv_exact(s, &ready, sizeof(ready)) || ready != INPUT_READY_MAGIC)
        goto failed;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
    if (ioctlsocket(s, FIONBIO, &nonblock) != 0) goto failed;
    queries[0].param2 = queries[1].param2 = (UINT32)InterlockedIncrement(&request_id);
    if (send(s, (const char *)queries, sizeof(queries), 0) != sizeof(queries))
        goto failed;
    /* Old helpers ignore the query. A late or incomplete reply cannot change
       the mode after the socket has been made available to the window thread. */
    deadline = GetTickCount64() + 500;
    while ((!keyboard_reply || !mouse_reply) && !d->stop) {
        fd_set read_set;
        struct timeval timeout;
        LONGLONG remaining = (LONGLONG)(deadline - GetTickCount64());
        if (remaining <= 0) break;
        timeout.tv_sec = 0;
        timeout.tv_usec = (long)remaining * 1000;
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);
        int result = select(0, &read_set, NULL, NULL, &timeout);
        if (result == SOCKET_ERROR) goto failed;
        if (result == 0) break;
        int n = recv(s, (char *)&reply + got, (int)sizeof(reply) - got, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n <= 0) goto failed;
        got += n;
        if (got == sizeof(reply)) {
            if (reply.magic == INPUT_MAGIC && reply.param2 == queries[0].param2 &&
                reply.param3 == 0) {
                if (reply.type == INPUT_KEYBOARD_REPLY) {
                    keyboard_reply = TRUE;
                    if (reply.param1 == INPUT_KEYBOARD_VERSION)
                        version = INPUT_KEYBOARD_VERSION;
                } else if (reply.type == INPUT_MOUSE_REPLY) {
                    mouse_reply = TRUE;
                    if (reply.param1 == INPUT_MOUSE_VERSION)
                        mouse_version = INPUT_MOUSE_VERSION;
                }
            }
            got = 0;
        }
    }
    if (d->stop) goto failed;
    AcquireSRWLockExclusive(&d->input_lock);
    ZeroMemory(d->held_down, sizeof(d->held_down));
    d->keyboard_version = version;
    d->mouse_version = mouse_version;
    d->mouse_reply = reply;
    d->mouse_reply_size = got;
    d->input_socket = s;
    ReleaseSRWLockExclusive(&d->input_lock);
    PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
    idd_log(d, L"Input connected + ready (keyboard v%d).", version);
    return s;
failed:
    closesocket(s);
    return INVALID_SOCKET;
}

/* ==================================================================
 * Audio playback — guest -> host
 * ================================================================== */

static const CLSID AUDIO_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID AUDIO_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID AUDIO_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID AUDIO_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

static DWORD WINAPI audio_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    HRESULT hr;
    BOOL com_ok = FALSE;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pAC = NULL;
    IAudioRenderClient *pRC = NULL;
    WAVEFORMATEX *renderfmt = NULL;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    UINT32 buf_frames = 0;
    UINT32 render_block_align = 0;
    AudioHeader hdr;
    BOOL stream_started = FALSE;
    BOOL session_logged = FALSE;
    int  header_misses = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        com_ok = TRUE;

    /* Outer retry loop — keep trying to connect to the guest capture helper */
    while (!d->stop) {
        GUID svc; hcs_service_guid(d->os_type, 4, &svc);
        SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
        if (s == INVALID_SOCKET) {
            int wait;
            for (wait = 0; wait < 2000 && !d->stop; wait += 200)
                Sleep(200);
            continue;
        }

        /* Blocking recv for the session */
        {
            DWORD no_timeout = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
        }
        d->audio_socket = s;

        /* Read one-shot AudioHeader.
           The guest closes without sending a header when the VAD endpoint
           isn't ready yet (e.g. before user login / audio stack init).
           Log only the first miss per run so we don't spam. */
        if (!recv_exact(s, &hdr, sizeof(hdr)) || hdr.magic != AUDIO_HEADER_MAGIC) {
            if (header_misses == 0)
                idd_log(d, L"Audio header bad/absent - guest not ready, retrying quietly.");
            header_misses++;
            goto session_cleanup;
        }

        idd_log(d, L"Audio connected (GUID :0004).");
        session_logged = TRUE;
        header_misses = 0;

        idd_log(d, L"Audio header: %lu Hz, %u ch, %u bits, tag=%u.",
                 hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);

        /* Set up WASAPI render on the default endpoint */
        hr = CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL,
                               CLSCTX_ALL, &AUDIO_IID_IMMDeviceEnumerator,
                               (void **)&pEnum);
        if (FAILED(hr)) { idd_log(d, L"Audio: CoCreateInstance failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDev);
        if (FAILED(hr)) { idd_log(d, L"Audio: GetDefaultAudioEndpoint failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDevice_Activate(pDev, &AUDIO_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC);
        if (FAILED(hr)) { idd_log(d, L"Audio: Activate failed 0x%08lX.", hr); goto session_cleanup; }

        /* Build the source format exactly matching what the guest is sending.
           We deliberately do NOT call GetMixFormat: we never use the host's
           mix format (Initialize runs with AUTOCONVERTPCM, so WASAPI converts
           our renderfmt to whatever the endpoint needs). On some endpoints
           (seen on AMD HD Audio) GetMixFormat returns
           AUDCLNT_E_UNSUPPORTED_FORMAT for the configured default format,
           which would needlessly kill the whole audio path. */
        renderfmt = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!renderfmt) goto session_cleanup;
        ZeroMemory(renderfmt, sizeof(WAVEFORMATEX));
        renderfmt->wFormatTag      = hdr.format_tag;
        renderfmt->nChannels       = hdr.channels;
        renderfmt->nSamplesPerSec  = hdr.sample_rate;
        renderfmt->wBitsPerSample  = hdr.bits_per_sample;
        renderfmt->nBlockAlign     = hdr.block_align ? hdr.block_align
                                     : (WORD)((hdr.channels * hdr.bits_per_sample) / 8);
        renderfmt->nAvgBytesPerSec = renderfmt->nSamplesPerSec * renderfmt->nBlockAlign;
        renderfmt->cbSize          = 0;
        render_block_align         = renderfmt->nBlockAlign;

        /* 40ms buffer. HV socket is effectively memcpy and the guest polls
           every 5ms, so scheduler jitter is the only thing we need to absorb. */
        hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000, 0, renderfmt, NULL);
        if (FAILED(hr)) {
            idd_log(d, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr);
            goto session_cleanup;
        }

        hr = IAudioClient_GetBufferSize(pAC, &buf_frames);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioRenderClient, (void **)&pRC);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_Start(pAC);
        if (FAILED(hr)) goto session_cleanup;
        stream_started = TRUE;

        idd_log(d, L"Audio render started: buf=%lu frames.", buf_frames);

        /* Receive loop */
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 padding;
            UINT32 free_frames;
            UINT32 frames_to_write;
            BYTE *dst;

            if (!recv_exact(s, &fh, sizeof(fh))) break;
            if (fh.bytes == 0 || fh.bytes > 4 * 1024 * 1024) break;

            if (fh.bytes > scratch_cap) {
                BYTE *nb = scratch
                    ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, fh.bytes)
                    : (BYTE *)HeapAlloc(GetProcessHeap(), 0, fh.bytes);
                if (!nb) break;
                scratch = nb;
                scratch_cap = fh.bytes;
            }
            if (!recv_exact(s, scratch, (int)fh.bytes)) break;

            /* Host-side mute: keep draining the socket but don't push to WASAPI */
            if (d->audio_muted) continue;

            frames_in_payload = fh.bytes / render_block_align;
            if (frames_in_payload == 0) continue;

            /* Push into WASAPI buffer; drop if no room (low-latency, no stalling) */
            hr = IAudioClient_GetCurrentPadding(pAC, &padding);
            if (FAILED(hr)) break;
            free_frames = buf_frames - padding;
            frames_to_write = frames_in_payload;
            if (frames_to_write > free_frames)
                frames_to_write = free_frames;
            if (frames_to_write == 0)
                continue;

            hr = IAudioRenderClient_GetBuffer(pRC, frames_to_write, &dst);
            if (FAILED(hr)) break;
            memcpy(dst, scratch, (size_t)frames_to_write * render_block_align);
            IAudioRenderClient_ReleaseBuffer(pRC, frames_to_write, 0);
        }

session_cleanup:
        if (stream_started && pAC) {
            IAudioClient_Stop(pAC);
            stream_started = FALSE;
        }
        if (pRC)    { IAudioRenderClient_Release(pRC);    pRC = NULL; }
        if (pAC)    { IAudioClient_Release(pAC);          pAC = NULL; }
        if (pDev)   { IMMDevice_Release(pDev);            pDev = NULL; }
        if (pEnum)  { IMMDeviceEnumerator_Release(pEnum); pEnum = NULL; }
        if (renderfmt) { CoTaskMemFree(renderfmt); renderfmt = NULL; }

        if (d->audio_socket != INVALID_SOCKET) {
            closesocket(d->audio_socket);
            d->audio_socket = INVALID_SOCKET;
        }
        if (session_logged) {
            idd_log(d, L"Audio session ended.");
            session_logged = FALSE;
        }

        if (d->stop) break;
        /* Back off when the guest keeps rejecting us (VAD not ready) */
        Sleep(header_misses > 3 ? 5000 : 500);
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (com_ok)  CoUninitialize();
    d->audio_recv_thread = NULL;
    return 0;
}


/* ==================================================================
 * D3D11 initialization and teardown
 * ================================================================== */

static BOOL d3d_compile_shader(const char *hlsl, const char *entry,
                               const char *target, ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), NULL, NULL, NULL,
                            entry, target, 0, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            ui_log(L"Shader compile error: %S",
                   (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        return FALSE;
    }
    if (errors) errors->lpVtbl->Release(errors);
    return TRUE;
}

static BOOL checked_raw_frame_layout(UINT width, UINT height, UINT stride,
                                     SIZE_T *bytes_out)
{
    ULONGLONG bytes;
    if (!width || !height || width > ASB_DISPLAY_RAW_MAX_WIDTH ||
        height > ASB_DISPLAY_RAW_MAX_HEIGHT || width > UINT_MAX / 4 ||
        stride < width * 4)
        return FALSE;
    bytes = (ULONGLONG)stride * height;
    if (bytes > ASB_DISPLAY_MAX_FRAME_DATA_SIZE || bytes > (ULONGLONG)SIZE_MAX)
        return FALSE;
    if (bytes_out) *bytes_out = (SIZE_T)bytes;
    return TRUE;
}

static void pending_mark_full_locked(VmDisplayIdd *d)
{
    /* Full upload dominates every queued partial. Clearing the accumulator is
     * intentional: the CPU backing buffer already contains all prior dirty
     * regions, so replaying them after the full upload cannot add coverage. */
    d->pending_full_upload = TRUE;
    d->pending_dirty_count = 0;
    d->frame_dirty = TRUE;
}

static void pending_add_rect_locked(VmDisplayIdd *d, RECT rect)
{
    UINT i;

    if (d->pending_full_upload) return;
    if (rect.left < 0) rect.left = 0;
    if (rect.top < 0) rect.top = 0;
    if (rect.right > (LONG)d->frame_width) rect.right = (LONG)d->frame_width;
    if (rect.bottom > (LONG)d->frame_height) rect.bottom = (LONG)d->frame_height;
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;

    /* Merge overlapping or touching regions. A bounding-box union is safe:
     * the CPU backing buffer already contains the newest pixels everywhere
     * inside the union, so uploading a little extra never loses an update. */
    for (;;) {
        BOOL merged = FALSE;
        for (i = 0; i < d->pending_dirty_count; i++) {
            RECT *old = &d->pending_dirty[i];
            if (rect.right < old->left || old->right < rect.left ||
                rect.bottom < old->top || old->bottom < rect.top)
                continue;
            if (rect.left > old->left) rect.left = old->left;
            if (rect.top > old->top) rect.top = old->top;
            if (rect.right < old->right) rect.right = old->right;
            if (rect.bottom < old->bottom) rect.bottom = old->bottom;
            d->pending_dirty[i] = d->pending_dirty[--d->pending_dirty_count];
            merged = TRUE;
            break;
        }
        if (!merged) break;
    }
    if (d->pending_dirty_count >= MAX_DIRTY_RECTS) {
        pending_mark_full_locked(d);
        return;
    }
    d->pending_dirty[d->pending_dirty_count++] = rect;
    d->frame_dirty = TRUE;
}

static void pending_add_wire_rects_locked(VmDisplayIdd *d,
                                          const AsbDisplayRect *rects,
                                          UINT count)
{
    UINT i;
    for (i = 0; i < count; i++) {
        RECT rect;
        rect.left = rects[i].left;
        rect.top = rects[i].top;
        rect.right = rects[i].right;
        rect.bottom = rects[i].bottom;
        pending_add_rect_locked(d, rect);
        if (d->pending_full_upload) return;
    }
}

static void maybe_log_host_stats(VmDisplayIdd *d)
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG elapsed;
    double mib_per_sec;
    SOCKET frame_socket;

    AcquireSRWLockShared(&d->frame_send_lock);
    frame_socket = d->frame_socket;
    ReleaseSRWLockShared(&d->frame_send_lock);
    if (frame_socket == INVALID_SOCKET) return;

    if (!d->host_stats_start_ms) d->host_stats_start_ms = now;
    if (d->host_stats_last_log_ms && now - d->host_stats_last_log_ms < 5000)
        return;
    d->host_stats_last_log_ms = now;
    elapsed = now - d->host_stats_start_ms;
    mib_per_sec = elapsed
        ? (double)d->host_gpu_upload_bytes * 1000.0 /
          ((double)elapsed * 1024.0 * 1024.0) : 0.0;
    idd_log(d, L"display_stats scope=host resolution=%ux%u logical_refresh_hz=60 "
            L"host_full_uploads=%llu host_partial_uploads=%llu "
            L"host_gpu_upload_bytes=%llu host_gpu_upload_mib_per_sec=%.3f "
            L"recv_fps=%.2f present_fps=%.2f frame_seq_gaps=%llu",
            d->frame_width, d->frame_height,
            d->host_full_uploads, d->host_partial_uploads,
            d->host_gpu_upload_bytes, mib_per_sec,
            elapsed ? (double)d->recv_count * 1000.0 / elapsed : 0.0,
            elapsed ? (double)d->render_count * 1000.0 / elapsed : 0.0,
            d->frame_seq_gaps);
}

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    HRESULT hr;

    /* Create device and swap chain */
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = DEFAULT_WIDTH;
    scd.BufferDesc.Height                  = DEFAULT_HEIGHT;
    scd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = d->render_hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION,
        &scd, &d->swap_chain, &d->device, &feature_level, &d->ctx);

    if (FAILED(hr)) {
        ui_log(L"IDD: D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        return FALSE;
    }

    /* Create render target view from back buffer */
    {
        ID3D11Texture2D *back_buf = NULL;
        hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                       &IID_ID3D11Texture2D, (void **)&back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: GetBuffer failed (0x%08X)", hr);
            return FALSE;
        }
        hr = d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: CreateRenderTargetView failed (0x%08X)", hr);
            return FALSE;
        }
    }

    /* Create frame texture. Raw frames are copied through UpdateSubresource;
     * the default-usage resource is required for efficient partial uploads. */
    ZeroMemory(&td, sizeof(td));
    td.Width              = d->frame_width;
    td.Height             = d->frame_height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DEFAULT;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;

    hr = d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &d->frame_tex);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateTexture2D failed (0x%08X)", hr);
        return FALSE;
    }

    /* Shader resource view for the frame texture */
    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    hr = d->device->lpVtbl->CreateShaderResourceView(d->device,
            (ID3D11Resource *)d->frame_tex, &srv_desc, &d->frame_srv);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateShaderResourceView failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create vertex shader */
    if (!d3d_compile_shader(g_vs_hlsl, "main", "vs_4_0", &vs_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreateVertexShader(d->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob),
            NULL, &d->vs);
    vs_blob->lpVtbl->Release(vs_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateVertexShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create pixel shader */
    if (!d3d_compile_shader(g_ps_hlsl, "main", "ps_4_0", &ps_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreatePixelShader(d->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob),
            NULL, &d->ps);
    ps_blob->lpVtbl->Release(ps_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreatePixelShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Sampler state (linear filtering) */
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = d->device->lpVtbl->CreateSamplerState(d->device, &sd, &d->sampler);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateSamplerState failed (0x%08X)", hr);
        return FALSE;
    }

    d->pending_full_upload = TRUE;
    d->frame_dirty = TRUE;
    return TRUE;
}

static void d3d_resize_swap_chain(VmDisplayIdd *d)
{
    RECT rc;
    HRESULT hr;
    ID3D11Texture2D *back_buf = NULL;

    if (!d->swap_chain) return;

    /* Release old render target */
    if (d->rtv) {
        d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 0, NULL, NULL);
        d->rtv->lpVtbl->Release(d->rtv);
        d->rtv = NULL;
    }

    GetClientRect(d->render_hwnd, &rc);
    idd_log(d, L"Resize: render_hwnd client=%dx%d, frame=%ux%u",
            rc.right, rc.bottom, d->frame_width, d->frame_height);
    if (rc.right == 0 || rc.bottom == 0) return;

    hr = d->swap_chain->lpVtbl->ResizeBuffers(d->swap_chain, 0,
            (UINT)rc.right, (UINT)rc.bottom,
            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        ui_log(L"IDD: ResizeBuffers failed (0x%08X)", hr);
        return;
    }

    hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                   &IID_ID3D11Texture2D, (void **)&back_buf);
    if (SUCCEEDED(hr)) {
        d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
    }
}

static BOOL d3d_ensure_raw_frame_texture(VmDisplayIdd *d)
{
    D3D11_TEXTURE2D_DESC current, desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC view;
    HRESULT hr;

    if (d->frame_tex) {
        ID3D11Texture2D_GetDesc(d->frame_tex, &current);
        if (current.Width == d->frame_width &&
            current.Height == d->frame_height &&
            current.Usage == D3D11_USAGE_DEFAULT)
            return TRUE;
    }

    if (d->frame_srv) {
        d->frame_srv->lpVtbl->Release(d->frame_srv);
        d->frame_srv = NULL;
    }
    if (d->frame_tex) {
        d->frame_tex->lpVtbl->Release(d->frame_tex);
        d->frame_tex = NULL;
    }

    ZeroMemory(&desc, sizeof(desc));
    desc.Width = d->frame_width;
    desc.Height = d->frame_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = d->device->lpVtbl->CreateTexture2D(d->device, &desc, NULL, &d->frame_tex);
    if (FAILED(hr)) return FALSE;

    ZeroMemory(&view, sizeof(view));
    view.Format = desc.Format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    hr = d->device->lpVtbl->CreateShaderResourceView(
        d->device, (ID3D11Resource *)d->frame_tex, &view, &d->frame_srv);
    if (FAILED(hr)) {
        d->frame_tex->lpVtbl->Release(d->frame_tex);
        d->frame_tex = NULL;
        return FALSE;
    }
    pending_mark_full_locked(d);
    return TRUE;
}

static void d3d_render_frame(VmDisplayIdd *d)
{
    D3D11_VIEWPORT vp;
    RECT rc;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    BOOL frame_uploaded = FALSE;

    if (!d->device || !d->ctx || !d->swap_chain || !d->rtv)
        return;

    /* Upload frame data to GPU texture if dirty */
    if (d->frame_dirty) {
        EnterCriticalSection(&d->frame_cs);
        if (d3d_ensure_raw_frame_texture(d)) {
            if (d->pending_full_upload) {
                d->ctx->lpVtbl->UpdateSubresource(
                    d->ctx, (ID3D11Resource *)d->frame_tex, 0, NULL,
                    d->frame_buf, d->frame_stride,
                    d->frame_stride * d->frame_height);
                d->host_full_uploads++;
                d->host_gpu_upload_bytes +=
                    (ULONGLONG)d->frame_width * d->frame_height * 4;
            } else {
                UINT i;
                for (i = 0; i < d->pending_dirty_count; i++) {
                    RECT *r = &d->pending_dirty[i];
                    D3D11_BOX box;
                    UINT width = (UINT)(r->right - r->left);
                    UINT height = (UINT)(r->bottom - r->top);
                    const BYTE *src = d->frame_buf +
                        (SIZE_T)r->top * d->frame_stride + (SIZE_T)r->left * 4;
                    ZeroMemory(&box, sizeof(box));
                    box.left = (UINT)r->left;
                    box.top = (UINT)r->top;
                    box.right = (UINT)r->right;
                    box.bottom = (UINT)r->bottom;
                    box.back = 1;
                    d->ctx->lpVtbl->UpdateSubresource(
                        d->ctx, (ID3D11Resource *)d->frame_tex, 0, &box,
                        src, d->frame_stride, d->frame_stride * height);
                    d->host_gpu_upload_bytes += (ULONGLONG)width * height * 4;
                }
                if (d->pending_dirty_count)
                    d->host_partial_uploads++;
            }
            d->pending_full_upload = FALSE;
            d->pending_dirty_count = 0;
            d->frame_dirty = FALSE;
            frame_uploaded = TRUE;
        }
        LeaveCriticalSection(&d->frame_cs);
        maybe_log_host_stats(d);
    }

    /* Compute letterboxed viewport within client area */
    GetClientRect(d->render_hwnd, &rc);
    {
        float vp_x, vp_y, vp_w, vp_h;
        compute_letterbox((UINT)rc.right, (UINT)rc.bottom,
                          d->frame_width, d->frame_height,
                          &vp_x, &vp_y, &vp_w, &vp_h);
        ZeroMemory(&vp, sizeof(vp));
        vp.TopLeftX = vp_x;
        vp.TopLeftY = vp_y;
        vp.Width    = vp_w;
        vp.Height   = vp_h;
        vp.MaxDepth = 1.0f;
    }

    /* Refresh the title once per uploaded frame (~frame rate). */
    if (frame_uploaded && d->hwnd) {
        wchar_t title[256];
        swprintf_s(title, 256, L"%s%s Display %ux%u recv=%u",
                   d->audio_muted ? L"\U0001F507 " : L"",
                   d->vm_name, d->frame_width, d->frame_height, d->recv_count);
        SetWindowTextW(d->hwnd, title);
    }
    d->render_count++;

    d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
    d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &vp);
    d->ctx->lpVtbl->ClearRenderTargetView(d->ctx, d->rtv, clear_color);

    d->ctx->lpVtbl->IASetPrimitiveTopology(d->ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->lpVtbl->IASetInputLayout(d->ctx, NULL);

    d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
    d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps, NULL, 0);
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &d->frame_srv);
    d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);

    /* Draw fullscreen triangle (3 vertices, no vertex buffer) */
    d->ctx->lpVtbl->Draw(d->ctx, 3, 0);

    d->swap_chain->lpVtbl->Present(d->swap_chain, 0, 0);
}

static void d3d_cleanup(VmDisplayIdd *d)
{
    if (d->sampler)    { d->sampler->lpVtbl->Release(d->sampler);       d->sampler = NULL; }
    if (d->ps)         { d->ps->lpVtbl->Release(d->ps);                 d->ps = NULL; }
    if (d->vs)         { d->vs->lpVtbl->Release(d->vs);                 d->vs = NULL; }
    if (d->frame_srv)  { d->frame_srv->lpVtbl->Release(d->frame_srv);   d->frame_srv = NULL; }
    if (d->frame_tex)  { d->frame_tex->lpVtbl->Release(d->frame_tex);   d->frame_tex = NULL; }
    if (d->rtv)        { d->rtv->lpVtbl->Release(d->rtv);               d->rtv = NULL; }
    if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
    if (d->ctx)        { d->ctx->lpVtbl->Release(d->ctx);               d->ctx = NULL; }
    if (d->device)     { d->device->lpVtbl->Release(d->device);         d->device = NULL; }
}

/* ==================================================================
 * Guest cursor — create HCURSOR from received bitmap
 * ================================================================== */

/* Cursor types from IddCx IDDCX_CURSOR_SHAPE_TYPE */
#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

static HCURSOR create_cursor_from_bitmap(UINT width, UINT height,
                                          UINT xhot, UINT yhot,
                                          UINT cursor_type, UINT pitch,
                                          UINT shape_data_size,
                                          const BYTE *shape_data)
{
    HCURSOR result = NULL;
    BITMAPINFO bmi;
    HBITMAP hColor = NULL, hMask = NULL;
    ICONINFO ii;
    HDC hdc;

    if (width == 0 || height == 0 || width > 256 || height > 256)
        return NULL;

    /* shape_data holds exactly shape_data_size bytes; both copy paths read up
       to (height-1)*pitch + width*4 bytes. Require the stride to cover a row
       and the total to cover all rows, else a tiny buffer with large
       width/height/pitch would over-read. */
    if (pitch < width * 4 || (UINT64)pitch * height > (UINT64)shape_data_size)
        return NULL;

    hdc = GetDC(NULL);

    if (cursor_type == CURSOR_TYPE_MASKED_COLOR) {
        /* MASKED_COLOR (IddCx 1.10 / QueryHardwareCursor3): single 32bpp BGRA
           image where the alpha channel encodes the AND mask.
           Height is the ACTUAL cursor height (not doubled).
           Per pixel: A = AND mask (0xFF = transparent/XOR, 0x00 = opaque),
                      B,G,R = XOR color values.
           We extract A into a 1bpp monochrome hbmMask and BGR into hbmColor. */
        UINT mask_row_bytes = (width + 7) / 8;
        /* CreateBitmap consumes WORD-aligned rows, unlike a DWORD-aligned
           DIB. */
        UINT mask_pitch = (mask_row_bytes + 1) & ~1u;
        void *color_bits = NULL;
        BYTE *mask_buf;
        UINT row, col;

        /* Build 1bpp AND mask from alpha channel */
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      mask_pitch * height);
        if (!mask_buf) { ReleaseDC(NULL, hdc); return NULL; }

        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                BYTE alpha = src_row[col * 4 + 3];  /* A channel */
                if (alpha != 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        /* XOR color bitmap (32bpp, top-down) — copy full BGRA data */
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)width;
        bmi.bmiHeader.biHeight      = -(LONG)height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (hColor && color_bits) {
            UINT dst_pitch = width * 4;
            for (row = 0; row < height; row++) {
                const BYTE *src = shape_data + row * pitch;
                BYTE *dst = (BYTE *)color_bits + row * dst_pitch;
                for (col = 0; col < width; col++) {
                    dst[col * 4 + 0] = src[col * 4 + 0];  /* B */
                    dst[col * 4 + 1] = src[col * 4 + 1];  /* G */
                    dst[col * 4 + 2] = src[col * 4 + 2];  /* R */
                    dst[col * 4 + 3] = 0;                  /* A = 0, AND mask handles transparency */
                }
            }
        }

        ReleaseDC(NULL, hdc);

        if (!hColor || !hMask) {
            if (hColor) DeleteObject(hColor);
            if (hMask)  DeleteObject(hMask);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;

    } else {
        /* ALPHA (type 2): 32bpp BGRA with premultiplied alpha.
           Height is the real cursor height.

           The 32-bpp DIB section with BI_RGB doesn't tell Windows that
           the top byte is alpha — Windows treats it as 24-bit RGB +
           padding and, with an all-zero AND mask, renders EVERY pixel
           opaque. For cursors whose transparent regions are stored as
           (0,0,0,0) (e.g. ours from Linux), that paints a black square
           around the cursor. Windows-VDD cursors happen not to trigger
           this because their transparent regions have non-zero RGB.

           Fix: build the AND mask from the alpha channel — bit set
           (1) = transparent, bit cleared (0) = opaque. Threshold low
           (any alpha > 0 = opaque) preserves anti-aliased edges, the
           alpha channel then handles smooth blending of those edges.

           Use BITMAPV4HEADER with explicit alpha mask instead of
           BITMAPINFOHEADER+BI_RGB. The latter is ambiguous at 32-bpp:
           some Windows paths treat it as XRGB (alpha ignored, RGB
           rendered at full opacity → cursor body looks too bright).
           V4 with bV4AlphaMask = 0xFF000000 settles it. */
        void *color_bits = NULL;
        UINT row, col, dst_pitch;
        UINT mask_row_bytes, mask_pitch;
        BYTE *mask_buf;
        BITMAPV4HEADER bv4;

        ZeroMemory(&bv4, sizeof(bv4));
        bv4.bV4Size          = sizeof(BITMAPV4HEADER);
        bv4.bV4Width         = (LONG)width;
        bv4.bV4Height        = -(LONG)height;
        bv4.bV4Planes        = 1;
        bv4.bV4BitCount      = 32;
        bv4.bV4V4Compression = BI_BITFIELDS;
        bv4.bV4RedMask       = 0x00FF0000;
        bv4.bV4GreenMask     = 0x0000FF00;
        bv4.bV4BlueMask      = 0x000000FF;
        bv4.bV4AlphaMask     = 0xFF000000;

        hColor = CreateDIBSection(hdc, (BITMAPINFO *)&bv4,
                                  DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (!hColor || !color_bits) {
            ReleaseDC(NULL, hdc);
            return NULL;
        }

        dst_pitch = width * 4;
        for (row = 0; row < height; row++) {
            memcpy((BYTE *)color_bits + row * dst_pitch,
                   shape_data + row * pitch,
                   dst_pitch);
        }

        /* AND mask, 1bpp, WORD-aligned for CreateBitmap. Bit set = transparent. */
        mask_row_bytes = (width + 7) / 8;
        mask_pitch     = (mask_row_bytes + 1) & ~1u;
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (size_t)mask_pitch * height);
        if (!mask_buf) {
            DeleteObject(hColor);
            ReleaseDC(NULL, hdc);
            return NULL;
        }
        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                if (src_row[col * 4 + 3] == 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        ReleaseDC(NULL, hdc);

        if (!hMask) {
            DeleteObject(hColor);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
    }

    result = (HCURSOR)CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);

    return result;
}

/* ==================================================================
 * Recv thread - connects to VM, receives frames, updates frame_buf
 * ================================================================== */

static void clip_log_callback(const wchar_t *msg, void *user_data)
{
    VmDisplayIdd *d = (VmDisplayIdd *)user_data;
    idd_log(d, L"%s", msg);
}

static DWORD WINAPI idd_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;
    SIZE_T recv_capacity = (SIZE_T)DEFAULT_WIDTH * DEFAULT_HEIGHT * 4;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Allocate receive buffer for frame pixel data */
    recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, recv_capacity);
    if (!recv_buf) {
        ui_log(L"IDD recv: failed to allocate receive buffer");
        return 1;
    }

    /* Tell the agent to respawn input helper in console session */
    if (!d->stop && d->vm && d->vm->agent_online) {
        idd_log(d, L"Sending idd_connect to agent...");
        vm_agent_send(d->vm, "idd_connect", NULL, 0, 5000);
    }

    /* Input socket lives independently of the frame channel — survives
       frame disconnections so the user can still send input to wake
       the VM screen if the display path goes inactive. */
    {
        SOCKET input_s = INVALID_SOCKET;

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;
        ULONGLONG connection_generation;

        /* Ensure input channel is connected (independent of frame channel) */
        if (input_s == INVALID_SOCKET)
            input_s = connect_input(d);

        /* Clipboard module (handles :0005 + :0006 internally) */
        if (!d->clipboard) {
            d->clipboard = vm_clipboard_create(&d->runtime_id, d->os_type,
                                               d->hwnd, clip_log_callback, d);
            if (d->clipboard)
                idd_log(d, L"Clipboard module created.");
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        {
            GUID svc; hcs_service_guid(d->os_type, 2, &svc);
            s = connect_to_hv_service(&d->runtime_id, &svc, 3000);
        }
        if (s == INVALID_SOCKET) {
            int wait;
            idd_log(d, L"Connection failed, retrying in 3s.");
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        idd_log(d, L"Frame channel connected.");
        AcquireSRWLockExclusive(&d->frame_send_lock);
        d->frame_socket = s;
        d->frame_connection_generation++;
        connection_generation = d->frame_connection_generation;
        InterlockedExchange(&d->resize_capable, 0);
        ReleaseSRWLockExclusive(&d->frame_send_lock);
        /* Start a fresh host display-stats epoch for every frame-channel
         * connection. Frame sequence numbers are connection-scoped, so the
         * rate counters and gap count must not span reconnects either. */
        d->host_stats_start_ms = GetTickCount64();
        d->host_stats_last_log_ms = 0;
        d->host_full_uploads = 0;
        d->host_partial_uploads = 0;
        d->host_gpu_upload_bytes = 0;
        d->recv_count = 0;
        d->render_count = 0;
        d->frame_seq_gaps = 0;
        d->cursor_visible = TRUE;
        /* frame_seq is scoped to one Guest connection. A real sequence gap
         * is the only host-side evidence we can provide for a dropped dirty
         * update; never report a hard-coded zero. */
        d->have_frame_seq = FALSE;
        d->last_frame_seq = 0;
        PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        while (!d->stop) {
            AsbDisplayRect dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;
            SIZE_T wire_frame_bytes;
            ULONGLONG expected_data_size;
            BOOL frame_size_changed = FALSE;

            /* Peek at magic to determine message type */
            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;

                BOOL cursor_changed = d->cursor_visible != (chdr.visible != 0);
                d->cursor_visible = chdr.visible != 0;

                if (chdr.shape_updated && chdr.shape_data_size > 0) {
                    BYTE *cursor_buf;
                    if (chdr.shape_data_size > MAX_CURSOR_SIZE) {
                        idd_log(d, L"Cursor data too large (%u), reconnecting.",
                               chdr.shape_data_size);
                        break;
                    }
                    cursor_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                                    chdr.shape_data_size);
                    if (!cursor_buf) break;

                    if (!recv_exact(s, cursor_buf, (int)chdr.shape_data_size)) {
                        HeapFree(GetProcessHeap(), 0, cursor_buf);
                        break;
                    }

                    /* Create new cursor from bitmap */
                    {
                        HCURSOR new_cursor = create_cursor_from_bitmap(
                            chdr.width, chdr.height, chdr.xhot, chdr.yhot,
                            chdr.cursor_type, chdr.pitch,
                            chdr.shape_data_size, cursor_buf);
                        if (new_cursor) {
                            HCURSOR old = d->guest_cursor;
                            d->guest_cursor = new_cursor;
                            d->cursor_shape_id = chdr.shape_id;
                            if (old) DestroyCursor(old);
                            cursor_changed = TRUE;
                        }
                    }

                    HeapFree(GetProcessHeap(), 0, cursor_buf);
                }
                if (cursor_changed)
                    PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
                continue;  /* back to message loop */
            }

            if (magic == ASB_DISPLAY_CONTROL_MAGIC) {
                AsbDisplayControl control;
                control.magic = magic;
                if (!recv_exact(s, (BYTE *)&control + sizeof(UINT32),
                                sizeof(control) - sizeof(UINT32)))
                    break;
                if (control.version != ASB_DISPLAY_CONTROL_VERSION) {
                    idd_log(d, L"display_control unsupported version=%u.",
                            control.version);
                    continue;
                }
                if (control.type == ASB_DISPLAY_CONTROL_HELLO) {
                    BOOL capable = (control.status_or_flags &
                                    ASB_DISPLAY_CONTROL_FLAG_DYNAMIC_RESIZE) != 0;
                    InterlockedExchange(&d->resize_capable, capable ? 1 : 0);
                    if (capable) {
                        AcquireSRWLockExclusive(&d->resize_lock);
                        d->force_resize_sync = TRUE;
                        ReleaseSRWLockExclusive(&d->resize_lock);
                    }
                    idd_log(d, L"display_control hello version=%u flags=0x%08X "
                            L"dynamic_resize=%s.", control.version,
                            control.status_or_flags, capable ? L"yes" : L"no");
                    if (!capable)
                        idd_post_runtime_result(d, FALSE, 0, 0);
                    if (capable && d->hwnd)
                        PostMessageW(d->hwnd, WM_IDD_RESIZE_CAPABLE, 0, 0);
                } else if (control.type == ASB_DISPLAY_CONTROL_RESIZE_ACK) {
                    idd_handle_resize_ack(d, &control, connection_generation);
                } else {
                    idd_log(d, L"display_control unexpected type=%u.",
                            control.type);
                }
                continue;
            }

            if (magic != FRAME_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }

            /* Read rest of frame header (already read magic) */
            hdr.magic = magic;
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
                break;

            /* Sanity checks */
            if (!checked_raw_frame_layout(hdr.width, hdr.height, hdr.stride,
                                          &wire_frame_bytes)) {
                idd_log(d, L"Invalid frame dimensions %ux%u stride %u.",
                       hdr.width, hdr.height, hdr.stride);
                break;
            }

            if (!d->have_frame_seq) {
                d->have_frame_seq = TRUE;
            } else {
                ULONGLONG expected = d->last_frame_seq + 1;
                if (hdr.frame_seq != expected) {
                    ULONGLONG missing = (hdr.frame_seq > expected)
                        ? hdr.frame_seq - expected : 1;
                    d->frame_seq_gaps += missing;
                    idd_log(d, L"Frame sequence gap: expected=%llu got=%llu "
                            L"(frame_seq_gaps=%llu)",
                            expected, hdr.frame_seq, d->frame_seq_gaps);
                }
            }
            d->last_frame_seq = hdr.frame_seq;

            rect_count = hdr.dirty_rect_count;
            if (rect_count > MAX_DIRTY_RECTS) {
                idd_log(d, L"Too many dirty rects (%u), reconnecting.", rect_count);
                break;
            }

            /* Read dirty rects */
            if (rect_count > 0) {
                if (!recv_exact(s, dirty_rects,
                                (int)(rect_count * sizeof(AsbDisplayRect))))
                    break;
            }

            expected_data_size = rect_count ? 0 : wire_frame_bytes;
            for (i = 0; i < rect_count; i++) {
                ULONGLONG width, height;
                if (dirty_rects[i].left < 0 || dirty_rects[i].top < 0 ||
                    dirty_rects[i].right <= dirty_rects[i].left ||
                    dirty_rects[i].bottom <= dirty_rects[i].top ||
                    (UINT32)dirty_rects[i].right > hdr.width ||
                    (UINT32)dirty_rects[i].bottom > hdr.height) {
                    idd_log(d, L"Invalid dirty rect %d: (%d,%d)-(%d,%d).",
                            i, dirty_rects[i].left, dirty_rects[i].top,
                            dirty_rects[i].right, dirty_rects[i].bottom);
                    break;
                }
                width = (UINT32)(dirty_rects[i].right - dirty_rects[i].left);
                height = (UINT32)(dirty_rects[i].bottom - dirty_rects[i].top);
                expected_data_size += width * height * 4;
                if (expected_data_size > ASB_DISPLAY_MAX_FRAME_DATA_SIZE)
                    break;
            }
            if (i != rect_count || expected_data_size > UINT32_MAX)
                break;

            /* Read data_size */
            if (!recv_exact(s, &data_size, 4))
                break;

            if ((ULONGLONG)data_size != expected_data_size) {
                idd_log(d, L"Frame data size mismatch: got %u expected %llu.",
                        data_size, expected_data_size);
                break;
            }

            if ((SIZE_T)data_size > recv_capacity) {
                BYTE *larger = (BYTE *)HeapReAlloc(GetProcessHeap(), 0,
                                                   recv_buf, data_size);
                if (!larger) break;
                recv_buf = larger;
                recv_capacity = data_size;
            }

            /* Read pixel data */
            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            /* Reallocate frame_buf if resolution changed */
            if (hdr.width != d->frame_width || hdr.height != d->frame_height) {
                frame_size_changed = TRUE;
                UINT new_stride = hdr.width * 4;
                SIZE_T new_size;
                if (!checked_raw_frame_layout(hdr.width, hdr.height,
                                              new_stride, &new_size)) {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
                BYTE *new_buf   = (BYTE *)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY, new_size);
                if (new_buf) {
                    if (d->frame_buf)
                        HeapFree(GetProcessHeap(), 0, d->frame_buf);
                    d->frame_buf    = new_buf;
                    d->frame_width  = hdr.width;
                    d->frame_height = hdr.height;
                    d->frame_stride = new_stride;
                    idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                            hdr.width, hdr.height, hdr.stride);
                    pending_mark_full_locked(d);
                } else {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
            }

            /* Every ASFR is authoritative, including a frame whose dimensions
             * equal the previous frame. This is what completes a resize whose
             * first post-modeset frame did not change the layout metadata. */
            {
                UINT32 completed_id = 0;
                BOOL notify_actual = frame_size_changed;
                BOOL flush_queued = FALSE;
                AcquireSRWLockExclusive(&d->resize_lock);
                d->actual_width = hdr.width;
                d->actual_height = hdr.height;
                d->actual_valid = TRUE;
                d->actual_generation = connection_generation;
                if (d->pending_resize_id &&
                    d->pending_resize_generation == connection_generation &&
                    d->pending_resize_phase == DISPLAY_RESIZE_PHASE_WAIT_FRAME &&
                    d->pending_resize_width == hdr.width &&
                    d->pending_resize_height == hdr.height) {
                    completed_id = d->pending_resize_id;
                    d->pending_resize_id = 0;
                    d->pending_resize_width = 0;
                    d->pending_resize_height = 0;
                    d->pending_resize_generation = 0;
                    d->pending_resize_deadline = 0;
                    d->pending_resize_phase = DISPLAY_RESIZE_PHASE_NONE;
                    d->pending_resize_retry_count = 0;
                    notify_actual = TRUE;
                }
                if (d->resize_queued &&
                    d->queued_resize_width == hdr.width &&
                    d->queued_resize_height == hdr.height)
                    d->resize_queued = FALSE;
                flush_queued = d->resize_queued && d->pending_resize_id == 0;
                ReleaseSRWLockExclusive(&d->resize_lock);
                if (completed_id) {
                    idd_log(d, L"display_resize frame id=%u actual=%ux%u",
                            completed_id, hdr.width, hdr.height);
                    idd_log(d, L"display_resize complete id=%u actual=%ux%u",
                            completed_id, hdr.width, hdr.height);
                    idd_post_runtime_result(d, TRUE, hdr.width, hdr.height);
                }
                if (notify_actual && d->hwnd)
                    PostMessageW(d->hwnd, WM_IDD_ACTUAL_SIZE,
                                 (WPARAM)hdr.width, (LPARAM)hdr.height);
                if (flush_queued && d->hwnd)
                    PostMessageW(d->hwnd, WM_IDD_RESIZE_RETRY, 0, 0);
            }

            if (rect_count == 0) {
                /* Full frame update */
                UINT row;
                UINT copy_w = hdr.width * 4;
                BYTE *src = recv_buf;
                /* Bound source rows by the bytes actually received: width/height/
                   stride and data_size are independent guest-controlled fields, so
                   only data_size/stride rows of recv_buf hold valid pixels. */
                UINT src_rows = hdr.stride ? (UINT)(data_size / hdr.stride) : 0;
                if (copy_w > d->frame_stride) copy_w = d->frame_stride;
                if (copy_w > hdr.stride)      copy_w = hdr.stride;

                for (row = 0; row < hdr.height && row < d->frame_height &&
                              row < src_rows; row++) {
                    memcpy(d->frame_buf + row * d->frame_stride,
                           src + row * hdr.stride,
                           copy_w);
                }
            } else {
                /* Dirty rect updates — pixel data is per-rect rows concatenated */
                BYTE *src = recv_buf;
                for (i = 0; i < rect_count; i++) {
                    LONG left   = dirty_rects[i].left;
                    LONG top    = dirty_rects[i].top;
                    LONG right  = dirty_rects[i].right;
                    LONG bottom = dirty_rects[i].bottom;
                    UINT rect_w, rect_h, row;
                    UINT rect_row_bytes;

                    /* Clamp to frame bounds */
                    if (left < 0) left = 0;
                    if (top  < 0) top  = 0;
                    if (right  > (LONG)d->frame_width)  right  = (LONG)d->frame_width;
                    if (bottom > (LONG)d->frame_height) bottom = (LONG)d->frame_height;
                    if (left >= right || top >= bottom) continue;

                    rect_w = (UINT)(right - left);
                    rect_h = (UINT)(bottom - top);
                    rect_row_bytes = rect_w * 4;

                    /* Refuse to read past the bytes actually received into recv_buf. */
                    if ((size_t)(src - recv_buf) + (size_t)rect_row_bytes * rect_h >
                        (size_t)data_size)
                        break;

                    for (row = 0; row < rect_h; row++) {
                        UINT dst_y = (UINT)top + row;
                        memcpy(d->frame_buf + dst_y * d->frame_stride + (UINT)left * 4,
                               src + row * rect_row_bytes,
                               rect_row_bytes);
                    }
                    src += rect_row_bytes * rect_h;
                }
            }

            if (rect_count == 0)
                pending_mark_full_locked(d);
            else
                pending_add_wire_rects_locked(d, dirty_rects, rect_count);
            d->recv_count++;
            LeaveCriticalSection(&d->frame_cs);
            if (!d->frame_connected) {
                d->frame_connected = TRUE;
                PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
            }

            /* Signal the window thread to repaint */
            if (d->hwnd && IsWindow(d->hwnd))
                PostMessageW(d->hwnd, WM_IDD_FRAME_READY, 0, 0);

            /* Reconnect input socket if send_input flagged it dead */
            if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
                idd_log(d, L"Input socket closed, will reconnect...");
            }
            if (input_s == INVALID_SOCKET)
                input_s = connect_input(d);
        }

        /* Frame channel lost — close it but keep input alive */
        d->frame_connected = FALSE;
        d->cursor_visible = TRUE;
        PostMessageW(d->hwnd, WM_IDD_CURSOR_CHANGED, 0, 0);
        {
            BOOL lost_current = FALSE;
            AcquireSRWLockExclusive(&d->frame_send_lock);
            if (d->frame_socket == s) {
                d->frame_socket = INVALID_SOCKET;
                d->frame_connection_generation++;
                lost_current = TRUE;
            }
            InterlockedExchange(&d->resize_capable, 0);
            ReleaseSRWLockExclusive(&d->frame_send_lock);
            if (lost_current)
                idd_invalidate_resize_connection(d);
        }
        closesocket(s);
        idd_log(d, L"Frame channel disconnected, reconnecting...");

        /* Check if input is still alive (send_input may have flagged it dead) */
        if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
            closesocket(input_s);
            input_s = INVALID_SOCKET;
            idd_log(d, L"Input socket flagged dead, will reconnect.");
        }

        /* Wait before reconnecting frame channel */
        {
            int wait;
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
        }
    }

    /* Final cleanup — close input socket on thread exit */
    AcquireSRWLockExclusive(&d->input_lock);
    d->input_socket = INVALID_SOCKET;
    d->keyboard_version = 1;
    d->mouse_version = 0;
    ZeroMemory(d->held_down, sizeof(d->held_down));
    ReleaseSRWLockExclusive(&d->input_lock);
    PostMessageW(d->hwnd, WM_IDD_INPUT_READY, 0, 0);
    if (input_s != INVALID_SOCKET) {
        closesocket(input_s);
    }
    idd_log(d, L"Input disconnected.");

    } /* end input_s scope */

    if (recv_buf)
        HeapFree(GetProcessHeap(), 0, recv_buf);

    AcquireSRWLockExclusive(&d->frame_send_lock);
    d->frame_socket = INVALID_SOCKET;
    d->frame_connection_generation++;
    InterlockedExchange(&d->resize_capable, 0);
    ReleaseSRWLockExclusive(&d->frame_send_lock);
    idd_invalidate_resize_connection(d);
    WSACleanup();
    return 0;
}

/* ==================================================================
 * Window thread — creates window, initializes D3D11, runs message pump
 * ================================================================== */

static void idd_layout_fullscreen_toolbar(VmDisplayIdd *d)
{
    RECT rc;
    int width, height, x;
    if (!d->fullscreen_toolbar || !GetClientRect(d->hwnd, &rc)) return;
    width = (int)idd_dip_to_px(d->hwnd, TOOLBAR_WIDTH_DIP);
    height = (int)idd_dip_to_px(d->hwnd, TOOLBAR_HEIGHT_DIP);
    if (width > rc.right) width = rc.right;
    if (height > rc.bottom) height = rc.bottom;
    x = (rc.right - width) / 2;
    SetWindowPos(d->fullscreen_toolbar, HWND_TOP, x, 0, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void idd_hide_fullscreen_toolbar(VmDisplayIdd *d)
{
    if (!d->fullscreen_toolbar_visible) return;
    d->fullscreen_toolbar_visible = FALSE;
    d->fullscreen_toolbar_pressed = FALSE;
    d->toolbar_leave_tick = 0;
    KillTimer(d->hwnd, IDT_FULLSCREEN_TOOLBAR);
    if (d->fullscreen_toolbar)
        ShowWindow(d->fullscreen_toolbar, SW_HIDE);
    if (d->fullscreen)
        idd_update_relative_mouse(d);
}

static void idd_show_fullscreen_toolbar(VmDisplayIdd *d)
{
    if (!d || !d->fullscreen || d->stop || !d->hwnd) return;
    if (!d->fullscreen_toolbar) {
        d->fullscreen_toolbar = CreateWindowExW(
            WS_EX_NOACTIVATE, IDD_TOOLBAR_CLASS, NULL,
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 1, 1, d->hwnd, NULL, d->hInstance, d);
        if (!d->fullscreen_toolbar) {
            idd_log(d, L"Fullscreen toolbar creation failed (err %lu).",
                    GetLastError());
            return;
        }
    }
    d->fullscreen_toolbar_visible = TRUE;
    d->toolbar_leave_tick = 0;
    idd_flush_mouse_buttons(d);
    idd_suspend_relative_mouse_capture(d);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    idd_layout_fullscreen_toolbar(d);
    SetTimer(d->hwnd, IDT_FULLSCREEN_TOOLBAR, TOOLBAR_POLL_MS, NULL);
}

static void idd_enter_fullscreen(VmDisplayIdd *d)
{
    WINDOWPLACEMENT placement;
    MONITORINFO mi;
    LONG_PTR style;
    HMONITOR monitor;
    HWND hwnd;

    if (!d || !d->hwnd || d->fullscreen) return;
    hwnd = d->hwnd;
    ZeroMemory(&placement, sizeof(placement));
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) return;
    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) return;

    d->windowed_placement = placement;
    d->windowed_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    d->windowed_ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    idd_suspend_relative_mouse_capture(d);

    style = d->windowed_style;
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
               WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP | WS_CLIPCHILDREN;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, d->windowed_ex_style);
    d->fullscreen = TRUE;
    d->fullscreen_toolbar_visible = FALSE;
    d->toolbar_leave_tick = 0;

    SetWindowPos(hwnd, HWND_TOP,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    idd_update_kbd_hook(d);
    idd_update_relative_mouse(d);
    idd_log(d, L"Entered borderless fullscreen.");
}

static void idd_exit_fullscreen(VmDisplayIdd *d)
{
    HWND hwnd;
    if (!d || !d->hwnd || !d->fullscreen) return;
    hwnd = d->hwnd;

    idd_suspend_relative_mouse_capture(d);
    idd_flush_held_keys(d);
    d->fullscreen = FALSE;
    idd_hide_fullscreen_toolbar(d);
    SetWindowLongPtrW(hwnd, GWL_STYLE, d->windowed_style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, d->windowed_ex_style);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetWindowPlacement(hwnd, &d->windowed_placement);
    idd_update_kbd_hook(d);
    idd_update_relative_mouse(d);
    idd_log(d, L"Exited borderless fullscreen.");
}

static DWORD WINAPI idd_window_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    wchar_t title[300];
    MSG msg;

    ensure_idd_class(d->hInstance);

    swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);

    /* Start at the configured size. Window geometry is independent after
       creation; resizing only changes the host swap chain/scaling. */
    {
        DWORD style   = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN;
        DWORD exstyle = 0;
        RECT wr = { 0, 0, (LONG)d->desired_width, (LONG)d->desired_height };
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);

        d->hwnd = CreateWindowExW(
            exstyle, IDD_DISPLAY_CLASS, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, d->hInstance, d);
    }

    if (!d->hwnd) {
        ui_log(L"IDD: CreateWindowEx failed (0x%08X)", GetLastError());
        d->open = FALSE;
        return 1;
    }

    /* Dark mode title bar to match AppSandbox main window */
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* Add options to the system menu (right-click title bar) */
    {
        HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
        if (sysmenu) {
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_AUDIO_MUTE, L"Mute audio");
            AppendMenuW(sysmenu, MF_STRING, IDM_XMIT_HOTKEYS, L"Transmit Keyboard Hotkeys");
            AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_LOG, L"Show Log");
            AppendMenuW(sysmenu, MF_STRING, IDM_ENTER_FULLSCREEN, L"Enter Fullscreen");
            CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                          MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    /* Bring the display window to the foreground on open */
    ShowWindow(d->hwnd, SW_SHOW);
    BringWindowToTop(d->hwnd);
    SetForegroundWindow(d->hwnd);

    /* Render child fills entire client area */
    {
        RECT rc;
        GetClientRect(d->hwnd, &rc);

        d->render_hwnd = CreateWindowExW(
            0, IDD_RENDER_CLASS, NULL,
            WS_CHILD | WS_VISIBLE,
            0, 0, rc.right, rc.bottom,
            d->hwnd, NULL, d->hInstance, NULL);
    }

    /* Separate top-level log window */
    {
        wchar_t log_title[300];
        HFONT font;
        swprintf_s(log_title, 300, L"%s - IDD Log", d->vm_name);

        /* Created hidden — shown on demand via the "Show Log" system-menu item. */
        d->log_hwnd = CreateWindowExW(
            0, L"AppSandboxIddLog", log_title,
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT, CW_USEDEFAULT, LOG_WINDOW_W, LOG_WINDOW_H,
            NULL, NULL, d->hInstance, NULL);

        if (d->log_hwnd) {
            /* Dark mode title bar to match AppSandbox main window */
            BOOL dark = TRUE;
            DwmSetWindowAttribute(d->log_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            /* Fill the log window with a listbox */
            RECT lrc;
            GetClientRect(d->log_hwnd, &lrc);
            d->log_list_hwnd = CreateWindowExW(
                0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_HASSTRINGS,
                0, 0, lrc.right, lrc.bottom,
                d->log_hwnd, (HMENU)(INT_PTR)IDC_LOG_LIST, d->hInstance, NULL);

            /* Subclass the listbox so Ctrl+A / Ctrl+C work when it has focus */
            if (d->log_list_hwnd)
                g_orig_listbox_proc = (WNDPROC)SetWindowLongPtrW(
                    d->log_list_hwnd, GWLP_WNDPROC, (LONG_PTR)idd_log_listbox_proc);

            font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
            if (font && d->log_list_hwnd)
                SendMessageW(d->log_list_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
        }
        idd_log(d, L"IDD display started.");
    }

    /* Initialize D3D11 */
    if (!d3d_init(d)) {
        ui_log(L"IDD: D3D11 initialization failed.");
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    d->control_send_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (d->control_send_event)
        d->control_send_thread = CreateThread(NULL, 0,
                                               idd_control_send_thread_proc,
                                               d, 0, NULL);
    if (!d->control_send_thread) {
        idd_log(d, L"IDD: display control sender unavailable.");
        if (d->control_send_event) {
            CloseHandle(d->control_send_event);
            d->control_send_event = NULL;
        }
    }

    /* Start the recv thread now that the window and D3D11 are ready */
    d->recv_thread = CreateThread(NULL, 0, idd_recv_thread_proc, d, 0, NULL);
    if (!d->recv_thread) {
        ui_log(L"IDD: Failed to create recv thread.");
        d3d_cleanup(d);
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start a present timer for steady rendering */
    SetTimer(d->hwnd, IDT_PRESENT, PRESENT_MS, NULL);

    idd_update_kbd_hook(d);

    /* Message pump */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        AddClipboardFormatListener(hwnd);
        return 0;
    }

    d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (d && d->mouse_sync_pending &&
        (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
         msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL))
        idd_resume_absolute_mouse(d, NULL);

    switch (msg) {
    case WM_SYSCOMMAND:
        if (d && (wp & 0xFFF0) == IDM_ENTER_FULLSCREEN) {
            if (!d->fullscreen) idd_enter_fullscreen(d);
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_AUDIO_MUTE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            wchar_t title[300];
            d->audio_muted = !d->audio_muted;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_AUDIO_MUTE,
                              MF_BYCOMMAND | (d->audio_muted ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->audio_muted)
                swprintf_s(title, 300, L"\U0001F507 %s - IDD Display", d->vm_name);
            else
                swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);
            SetWindowTextW(hwnd, title);
            idd_log(d, d->audio_muted ? L"Audio muted." : L"Audio unmuted.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_XMIT_HOTKEYS) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->transmit_hotkeys = !d->transmit_hotkeys;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                              MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            }
            if (!d->transmit_hotkeys) {
                /* Release anything the guest may be holding from this mode. */
                idd_flush_held_keys(d);
            }
            idd_update_kbd_hook(d);
            idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys);
            idd_log(d, d->transmit_hotkeys
                        ? L"Transmit Keyboard Hotkeys: ON."
                        : L"Transmit Keyboard Hotkeys: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_SHOW_LOG) {
            /* Reveal the log window (hidden by default). Closing it via its
               own [X] just hides it again (see idd_log_proc WM_CLOSE), so
               this item can re-open it any number of times. */
            if (d->log_hwnd && IsWindow(d->log_hwnd)) {
                if (IsIconic(d->log_hwnd))
                    ShowWindow(d->log_hwnd, SW_RESTORE);
                else
                    ShowWindow(d->log_hwnd, SW_SHOW);
                BringWindowToTop(d->log_hwnd);
                SetForegroundWindow(d->log_hwnd);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            /* Do not leave the configuration modal in Applying... when the
             * user closes the display or the VM teardown closes it for us. */
            idd_post_runtime_result(d, FALSE, 0, 0);

            RemoveClipboardFormatListener(hwnd);

            /* Remove the hotkey hook and release any keys still held in the
               guest before tearing the window down. */
            idd_remove_kbd_hook(d);
            idd_flush_held_keys(d);
            idd_flush_mouse_buttons(d);

            /* Stop recv threads */
            d->stop = TRUE;
            d->control_send_stop = TRUE;
            if (d->control_send_event)
                SetEvent(d->control_send_event);
            if (d->control_send_thread) {
                WaitForSingleObject(d->control_send_thread, 2000);
                CloseHandle(d->control_send_thread);
                d->control_send_thread = NULL;
            }
            if (d->control_send_event) {
                CloseHandle(d->control_send_event);
                d->control_send_event = NULL;
            }
            AcquireSRWLockShared(&d->frame_send_lock);
            if (d->frame_socket != INVALID_SOCKET)
                shutdown(d->frame_socket, SD_BOTH);
            ReleaseSRWLockShared(&d->frame_send_lock);
            idd_update_relative_mouse(d);

            /* Destroy clipboard module */
            if (d->clipboard) {
                vm_clipboard_destroy(d->clipboard);
                d->clipboard = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                if (d->audio_socket != INVALID_SOCKET) {
                    closesocket(d->audio_socket);
                    d->audio_socket = INVALID_SOCKET;
                }
                WaitForSingleObject(d->audio_recv_thread, 2000);
                CloseHandle(d->audio_recv_thread);
                d->audio_recv_thread = NULL;
            }

            /* Wait briefly for recv thread to exit */
            if (d->recv_thread) {
                WaitForSingleObject(d->recv_thread, 2000);
                CloseHandle(d->recv_thread);
                d->recv_thread = NULL;
            }

            d->open = FALSE;

            /* Close the separate log window */
            if (d->log_hwnd && IsWindow(d->log_hwnd))
                DestroyWindow(d->log_hwnd);
            d->log_hwnd = NULL;
            d->log_list_hwnd = NULL;

            /* Clean up D3D11 */
            d3d_cleanup(d);

            /* Clean up guest cursor */
            if (d->guest_cursor) {
                DestroyCursor(d->guest_cursor);
                d->guest_cursor = NULL;
            }

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             1, (LPARAM)d->vm);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PRESENT);
        KillTimer(hwnd, IDT_DISPLAY_RESIZE);
        KillTimer(hwnd, IDT_FULLSCREEN_TOOLBAR);
        if (d) idd_remove_kbd_hook(d);  /* safety net if WM_CLOSE was bypassed */
        if (d) {
            d->stop = TRUE;
            idd_update_relative_mouse(d);
        }
        if (d) d->hwnd = NULL;
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        RECT wr;
        /* Minimum: 320x180 client area */
        wr.left = 0; wr.top = 0; wr.right = 320; wr.bottom = 180;
        AdjustWindowRectEx(&wr, style, FALSE, exstyle);
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;
        /* Max: protocol limit, never the current frame size. This lets a
           window grow before the guest has produced its larger ASFR. */
        if (!d || !d->fullscreen) {
            wr.left = 0; wr.top = 0;
            wr.right = (LONG)ASB_DISPLAY_RAW_MAX_WIDTH;
            wr.bottom = (LONG)ASB_DISPLAY_RAW_MAX_HEIGHT;
            AdjustWindowRectEx(&wr, style, FALSE, exstyle);
            mmi->ptMaxTrackSize.x = wr.right - wr.left;
            mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        }
        return 0;
    }

    case WM_DPICHANGED:
        if (d) {
            RECT *suggested = (RECT *)lp;
            if (d->fullscreen) {
                HMONITOR monitor = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (monitor && GetMonitorInfoW(monitor, &mi)) {
                    SetWindowPos(hwnd, NULL,
                                 mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left,
                                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE |
                                 SWP_NOOWNERZORDER);
                }
            } else if (suggested) {
                SetWindowPos(hwnd, NULL,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (d->fullscreen_toolbar_visible)
                idd_layout_fullscreen_toolbar(d);
        }
        return 0;

    case WM_SIZE:
        if (d) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (d->render_hwnd)
                MoveWindow(d->render_hwnd, 0, 0, rc.right, rc.bottom, TRUE);
            if (d->fullscreen_toolbar_visible)
                idd_layout_fullscreen_toolbar(d);
            d3d_resize_swap_chain(d);
            if (d->relative_mouse) {
                AcquireSRWLockExclusive(&g_mouse_capture_lock);
                if (g_mouse_capture_hwnd == hwnd) idd_clip_mouse(d);
                ReleaseSRWLockExclusive(&g_mouse_capture_lock);
            }
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_MOVE:
        if (d && d->relative_mouse) {
            AcquireSRWLockExclusive(&g_mouse_capture_lock);
            if (g_mouse_capture_hwnd == hwnd) idd_clip_mouse(d);
            ReleaseSRWLockExclusive(&g_mouse_capture_lock);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        if (d) {
            d->input_sizing = msg == WM_ENTERSIZEMOVE;
            if (msg == WM_EXITSIZEMOVE) {
                /* Deliberately no guest mode request here. */
            }
            idd_update_relative_mouse(d);
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (d) d3d_render_frame(d);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_FULLSCREEN_TOOLBAR && d) {
            POINT pt;
            BOOL in_region = GetCursorPos(&pt) &&
                             idd_point_in_fullscreen_toolbar_region(d, pt);
            if (in_region) {
                d->toolbar_leave_tick = 0;
            } else if (!d->toolbar_leave_tick) {
                d->toolbar_leave_tick = GetTickCount64();
            } else if (GetTickCount64() - d->toolbar_leave_tick >= TOOLBAR_HIDE_MS) {
                idd_hide_fullscreen_toolbar(d);
            }
            return 0;
        }
        if (wp == IDT_PRESENT && d) {
            if (d->mouse_sync_pending) idd_poll_mouse_position(d);
            if (d->frame_dirty)
                d3d_render_frame(d);
            else
                maybe_log_host_stats(d);
        }
        if (wp == IDT_DISPLAY_RESIZE && d) {
            idd_check_resize_timeout(d);
            idd_flush_resize(d);
            {
                BOOL keep_timer;
                AcquireSRWLockShared(&d->resize_lock);
                keep_timer = d->resize_queued || d->pending_resize_id != 0 ||
                             d->runtime_request_active;
                ReleaseSRWLockShared(&d->resize_lock);
                if (!keep_timer ||
                    InterlockedCompareExchange(&d->resize_capable, 0, 0) == 0)
                    KillTimer(hwnd, IDT_DISPLAY_RESIZE);
            }
        }
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) d3d_render_frame(d);
        return 0;

    case WM_IDD_RESIZE_CAPABLE:
        if (d) {
            idd_schedule_resize(d, d->desired_width, d->desired_height, TRUE);
        }
        return 0;

    case WM_IDD_RESIZE_RETRY:
        if (d)
            idd_flush_resize(d);
        return 0;

    case WM_IDD_ACTUAL_SIZE:
        if (d) {
            idd_flush_resize(d);
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (d && d->clipboard) {
            vm_clipboard_on_clipboard_update(d->clipboard);
        }
        return 0;

    case WM_CLIP_READER_APPLY:
        if (d && d->clipboard) {
            vm_clipboard_on_reader_apply(d->clipboard);
        }
        return 0;

    /* Posted by vm_display_idd_focus() from another thread to raise an
       already-open window. Runs on the window's own thread. Restores from
       minimized (the creation path never had to handle that) then brings
       the window forward; SetForegroundWindow succeeds because the user
       just clicked our foreground main window to trigger this. */
    case WM_IDD_FOCUS:
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        return 0;

    case WM_IDD_INPUT_READY:
        if (d && !d->stop) {
            idd_cancel_mouse_sync(d);
            idd_update_kbd_hook(d);
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_IDD_CURSOR_CHANGED:
        if (d && d->render_hwnd) {
            idd_update_relative_mouse(d);
            POINT pt;
            if (GetCursorPos(&pt) && WindowFromPoint(pt) == d->render_hwnd)
                SendMessageW(d->render_hwnd, WM_SETCURSOR,
                             (WPARAM)d->render_hwnd,
                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }
        return 0;

    case WM_ENTERMENULOOP:
        if (d) {
            d->input_menu_active = TRUE;
            if (d->keyboard_version == INPUT_KEYBOARD_VERSION) idd_flush_held_keys(d);
            idd_flush_mouse_buttons(d);
            idd_update_relative_mouse(d);
        }
        break;

    case WM_EXITMENULOOP:
        if (d) {
            d->input_menu_active = FALSE;
            idd_update_relative_mouse(d);
        }
        break;

    case WM_SETFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, TRUE);
        break;

    case WM_KILLFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, FALSE);
        break;

    /* Top-level activation gates keyboard forwarding. Tracking activation
       (not WM_KILLFOCUS) is correct because focus moves between this window
       and its render child without losing activation. Losing activation —
       e.g. the user clicks another window or Alt+Tabs away with a key held —
       flushes held keys so nothing sticks down in the guest. */
    case WM_ACTIVATE:
        if (d) {
            d->input_focused = (LOWORD(wp) != WA_INACTIVE);
            if (!d->input_focused) {
                idd_flush_held_keys(d);
                idd_flush_mouse_buttons(d);
            }
            idd_update_relative_mouse(d);
        }
        break;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
    case WM_INPUT:
        if (d && d->relative_mouse && d->input_focused &&
            d->frame_connected && !d->cursor_visible &&
            d->mouse_version == INPUT_MOUSE_VERSION &&
            GetForegroundWindow() == hwnd) {
            RAWINPUT raw = {0};
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                raw.header.dwType == RIM_TYPEMOUSE) {
                LONG dx = raw.data.mouse.lLastX, dy = raw.data.mouse.lLastY;
                if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    BOOL virtual_desktop = (raw.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                    POINT pt = {
                        MulDiv(dx, GetSystemMetrics(virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535),
                        MulDiv(dy, GetSystemMetrics(virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535)
                    };
                    dx = dy = 0;
                    if (d->raw_absolute_valid && d->raw_absolute_device == raw.header.hDevice) {
                        dx = pt.x - d->raw_absolute_position.x;
                        dy = pt.y - d->raw_absolute_position.y;
                    }
                    d->raw_absolute_valid = TRUE;
                    d->raw_absolute_device = raw.header.hDevice;
                    d->raw_absolute_position = pt;
                } else {
                    d->raw_absolute_valid = FALSE;
                }
                if (dx || dy) {
                    send_input(d, INPUT_MOUSE_RELATIVE, (UINT32)dx, (UINT32)dy, 0);
                }
            }
        }
        break;

    case WM_MOUSEMOVE:
        if (d) {
            POINT screen_pt;
            if (!d->tracking && d->render_hwnd) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = d->render_hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                d->tracking = TRUE;
            }

            if (d->fullscreen &&
                (int)(short)HIWORD(lp) <=
                    (int)idd_dip_to_px(d->hwnd, TOOLBAR_HOTZONE_DIP))
                idd_show_fullscreen_toolbar(d);

            /* While the host overlay is active, the whole top interaction
               strip belongs to the host. Do not leak absolute motion into
               the guest after relative capture has been suspended. Keeping
               mouse_in false also blocks guest button and wheel forwarding. */
            if (d->fullscreen_toolbar_visible &&
                GetCursorPos(&screen_pt) &&
                idd_point_in_fullscreen_toolbar_region(d, screen_pt)) {
                d->mouse_in = FALSE;
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
                return 0;
            }

            d->mouse_in = TRUE;
            if (!d->relative_mouse && !d->cursor_visible)
                idd_update_relative_mouse(d);

            if (!d->relative_mouse && !d->mouse_sync_pending && d->frame_width && d->frame_height) {
                UINT vx, vy;
                window_to_vm_coords(d->render_hwnd,
                                    (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                                    d->frame_width, d->frame_height, &vx, &vy);
                send_input(d, INPUT_MOUSE_MOVE, vx, vy, 0);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (d) {
            d->mouse_in = FALSE;
            d->tracking = FALSE;
            idd_update_relative_mouse(d);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (d && d->fullscreen_toolbar_visible) {
                POINT pt;
                if (GetCursorPos(&pt) &&
                    idd_point_in_fullscreen_toolbar_region(d, pt)) {
                    SetCursor(LoadCursorW(NULL, IDC_ARROW));
                    return TRUE;
                }
            }

            if (d && (!d->cursor_visible || d->mouse_sync_pending))
                SetCursor(NULL);
            else if (d && d->guest_cursor)
                SetCursor(d->guest_cursor);
            else
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    /* ---- Mouse button/wheel forwarding (only when cursor is in render area) ---- */
    case WM_LBUTTONDOWN:
        if (d && d->mouse_in) {
            if (d->render_hwnd) SetCapture(d->render_hwnd);
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_LEFT;
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_LEFT)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_LEFT);
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (d && d->mouse_in) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_RIGHT;
        }
        return 0;
    case WM_RBUTTONUP:
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_RIGHT)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_RIGHT);
        }
        return 0;

    case WM_MBUTTONDOWN:
        if (d && d->mouse_in) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
            d->mouse_buttons |= 1u << INPUT_BTN_MIDDLE;
        }
        return 0;
    case WM_MBUTTONUP:
        if (d && (d->mouse_in || (d->mouse_buttons & (1u << INPUT_BTN_MIDDLE)))) {
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
            d->mouse_buttons &= ~(1u << INPUT_BTN_MIDDLE);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;

    /* ---- Keyboard input forwarding (gated on window activation) ----
       In Transmit mode key events are captured by the low-level hook
       and never reach here. The mode check below covers Default mode: a
       reserved hotkey is neither forwarded to the guest nor consumed — it
       falls through to DefWindowProc so the host handles it normally (this
       is what avoids the old stuck-key bug). Normal keys are forwarded while
       the window is the active foreground window. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        BOOL ext = (lp & (1 << 24)) != 0;
        BOOL up  = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
        if (!d) break;
        if (wp == VK_F11) {
            if (!up && !d->fullscreen) {
                d->suppress_f11_up = TRUE;
                idd_enter_fullscreen(d);
                return 0;
            }
            if (up && d->suppress_f11_up) {
                d->suppress_f11_up = FALSE;
                return 0;
            }
            if (up && !d->fullscreen)
                return 0;
        }
        if (!idd_capture_all_keys(d) &&
            idd_is_reserved_hotkey((DWORD)wp, (GetKeyState(VK_MENU) & 0x8000) != 0))
            break;  /* Default mode: let the host handle this hotkey. */
        if (d->kbd_hook && d->keyboard_version == INPUT_KEYBOARD_VERSION)
            return 0;  /* The hook already forwarded modifiers passed through to the host. */
        if (d->input_focused)
            idd_forward_key(d, (DWORD)wp, scan, ext, up);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Public API
 * ================================================================== */

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    VmDisplayIdd *d;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm           = vm;
    wcscpy_s(d->vm_name, 256, vm->name);
    wcscpy_s(d->vhdx_path, MAX_PATH, vm->vhdx_path);
    d->runtime_id   = vm->runtime_id;
    wcscpy_s(d->os_type, 32, vm->os_type);
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->open         = TRUE;
    d->stop         = FALSE;
    InitializeSRWLock(&d->input_lock);
    d->keyboard_version   = 1;
    d->input_socket       = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;
    d->frame_socket       = INVALID_SOCKET;
    d->clipboard          = NULL;
    d->cursor_visible     = TRUE;
    InitializeSRWLock(&d->frame_send_lock);
    InitializeSRWLock(&d->control_queue_lock);
    InitializeSRWLock(&d->resize_lock);

    /* Load the per-VM display setting, creating display_settings.json with
       the default (off) if this VM doesn't have one yet. The hook itself is
       installed later, on the window thread, once the window exists. */
    d->transmit_hotkeys = idd_display_settings_load_or_create(vm->vhdx_path);

    /* Initialize frame buffer at default resolution */
    d->frame_width  = DEFAULT_WIDTH;
    d->frame_height = DEFAULT_HEIGHT;
    d->frame_stride = DEFAULT_WIDTH * 4;
    d->desired_width = vm->display_width ? vm->display_width : DEFAULT_WIDTH;
    d->desired_height = vm->display_height ? vm->display_height : DEFAULT_HEIGHT;
    if (!asb_display_is_preset(d->desired_width, d->desired_height)) {
        d->desired_width = DEFAULT_WIDTH;
        d->desired_height = DEFAULT_HEIGHT;
    }
    d->runtime_notify_hwnd = main_hwnd;
    d->actual_width = 0;
    d->actual_height = 0;
    d->actual_valid = FALSE;
    d->actual_generation = 0;
    d->frame_buf    = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         d->frame_stride * d->frame_height);
    if (!d->frame_buf) {
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    InitializeCriticalSection(&d->frame_cs);

    /* Start the window thread (which will then start the recv thread) */
    d->window_thread = CreateThread(NULL, 0, idd_window_thread_proc, d, 0, NULL);
    if (!d->window_thread) {
        ui_log(L"IDD: Failed to create window thread.");
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

BOOL vm_display_idd_set_runtime_display(VmDisplayIdd *display,
                                         DWORD width, DWORD height)
{
    BOOL already_actual;

    if (!display || !display->open || display->stop ||
        !asb_display_is_preset(width, height))
        return FALSE;

    AcquireSRWLockExclusive(&display->resize_lock);
    if (display->runtime_request_active) {
        ReleaseSRWLockExclusive(&display->resize_lock);
        return FALSE;
    }
    display->runtime_request_active = TRUE;
    display->runtime_request_width = width;
    display->runtime_request_height = height;
    display->runtime_request_deadline =
        GetTickCount64() + ASB_DISPLAY_RUNTIME_REQUEST_TIMEOUT_MS;
    already_actual = display->actual_valid &&
                     display->actual_width == width &&
                     display->actual_height == height &&
                     InterlockedCompareExchange(&display->resize_capable, 0, 0) != 0;
    ReleaseSRWLockExclusive(&display->resize_lock);

    if (already_actual) {
        idd_post_runtime_result(display, TRUE, width, height);
        return TRUE;
    }

    /* Keep the lifecycle deadline running even before frame HELLO/capability
     * arrives. A disconnected or never-started guest must not leave the UI
     * in Applying forever. */
    if (display->hwnd)
        SetTimer(display->hwnd, IDT_DISPLAY_RESIZE, 250, NULL);

    /* idd_schedule_resize retains the desired target until HELLO arrives,
     * so a request made while the frame helper is reconnecting is not lost. */
    idd_schedule_resize(display, width, height, TRUE);
    return TRUE;
}

void vm_display_idd_destroy(VmDisplayIdd *display)
{
    if (!display) return;

    if (display->runtime_request_active)
        idd_post_runtime_result(display, FALSE, 0, 0);

    /* Signal stop */
    display->stop = TRUE;
    display->open = FALSE;

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(
                1, &display->window_thread, FALSE, 5000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
        CloseHandle(display->window_thread);
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        WaitForSingleObject(display->recv_thread, 3000);
        CloseHandle(display->recv_thread);
    }

    /* Clipboard is cleaned up by WM_CLOSE handler, but guard */
    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    /* audio_recv_thread guard */
    if (display->audio_recv_thread) {
        if (display->audio_socket != INVALID_SOCKET) {
            closesocket(display->audio_socket);
            display->audio_socket = INVALID_SOCKET;
        }
        WaitForSingleObject(display->audio_recv_thread, 3000);
        CloseHandle(display->audio_recv_thread);
    }

    DeleteCriticalSection(&display->frame_cs);

    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    HeapFree(GetProcessHeap(), 0, display);
}

BOOL vm_display_idd_is_open(VmDisplayIdd *display)
{
    if (!display) return FALSE;
    return display->open && display->hwnd && IsWindow(display->hwnd);
}

void vm_display_idd_focus(VmDisplayIdd *display)
{
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd))
        return;
    /* Marshal to the window thread; that thread owns the window and runs
       the activation (restore-if-minimized + foreground) in WM_IDD_FOCUS. */
    PostMessageW(display->hwnd, WM_IDD_FOCUS, 0, 0);
}
