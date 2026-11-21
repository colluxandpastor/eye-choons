#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0601
#define COBJMACROS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "msimg32.lib")

#define MAX_QUEUE_TRACKS 4096
#define MAX_DEVICE_TRACKS 32768

// Custom Messages
#define WM_APP_DEVICE_CONNECTED    (WM_USER + 101)
#define WM_APP_DEVICE_DISCONNECTED (WM_USER + 102)

// Control IDs
#define IDC_BTN_PUSHALBUM  2001
#define IDC_BTN_ADDTRACKS  2002
#define IDC_BTN_EXPORT     2003
#define IDC_BTN_REFRESH    2004
#define IDC_BTN_SYNCQUEUE  2005
#define IDC_BTN_REMOVE     2006
#define IDC_BTN_CLEAR      2007
#define IDC_EDIT_SEARCH    2008
#define IDC_LIST_MAIN      2010
#define IDC_TREE_SIDEBAR   2011
#define IDC_ART_PREVIEW    2012

// Context Menu IDs
#define IDM_REVEAL_EXPLORER 4001
#define IDM_REMOVE_QUEUE    4002
#define IDM_CLEAR_QUEUE     4003
#define IDM_EXPORT_TRACKS   4004
#define IDM_COPY_INFO       4005
#define IDM_SELECT_ALL      4006
#define IDM_REFRESH_LIB     4007

// Modern Clean Glossy Palette
#define COLOR_TOOLBAR_TOP   RGB(253, 254, 255)
#define COLOR_TOOLBAR_BOT   RGB(240, 243, 248)
#define COLOR_BORDER_LINE   RGB(212, 216, 222)
#define COLOR_SIDEBAR_BG    RGB(244, 246, 249)
#define COLOR_SIDEBAR_SEL   RGB(218, 228, 245)

#define COLOR_ZEBRA_EVEN    RGB(255, 255, 255)
#define COLOR_ZEBRA_ODD     RGB(248, 250, 253)
#define COLOR_ZEBRA_SEL     RGB(0, 122, 255)

#define COLOR_TEXT_MUTED    RGB(130, 136, 146)
#define COLOR_TEXT_SUCCESS  RGB(38, 142, 68)
#define COLOR_TEXT_PRIMARY  RGB(28, 32, 38)

typedef enum {
    VIEW_DEVICE_LIBRARY = 0,
    VIEW_TRANSFER_QUEUE = 1
} ActiveView;

typedef enum {
    STATUS_READY = 0,
    STATUS_DUPLICATE,
    STATUS_SYNCING,
    STATUS_SYNCED,
    STATUS_ERROR
} SyncStatus;

// Queued item to transfer to iPhone
typedef struct {
    wchar_t title[256];
    wchar_t artist[256];
    wchar_t album[256];
    wchar_t albumArtist[256];
    wchar_t track[32];
    wchar_t year[32];
    wchar_t filePath[MAX_PATH];
    long fileSize;
    long durationMs;
    int trackNum;
    int trackCountVal;
    int discNum;
    int yearVal;
    int isCompilation;
    SyncStatus status;
    int isDuplicate;
} QueueTrack;

// Item already on the iPhone
typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char normTitle[256];
    char normArtist[256];
    char location[64];      // e.g. "IPAE.mp3"
    char remoteDir[64];     // e.g. "iTunes_Control/Music/F36"
    char artworkToken[32];  // e.g. "477"
    long long item_pid;
    long long fileSize;
    long long durationMs;
    int trackNum;
} DeviceTrack;

#include "apple_conduit.h"

// SQLite3 Pointers
typedef void sqlite3;
typedef void sqlite3_stmt;
typedef int (*sqlite3_open_t)(const char *, sqlite3 **);
typedef int (*sqlite3_close_t)(sqlite3 *);
typedef int (*sqlite3_prepare_v2_t)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (*sqlite3_step_t)(sqlite3_stmt *);
typedef long long (*sqlite3_column_int64_t)(sqlite3_stmt *, int);
typedef const unsigned char *(*sqlite3_column_text_t)(sqlite3_stmt *, int);
typedef int (*sqlite3_finalize_t)(sqlite3_stmt *);
typedef int (*sqlite3_exec_t)(sqlite3 *, const char *, int (*callback)(void*,int,char**,char**), void *, char **);
typedef const char *(*sqlite3_errmsg_t)(sqlite3 *);


static sqlite3_open_t pSqlOpen = NULL;
static sqlite3_close_t pSqlClose = NULL;
static sqlite3_prepare_v2_t pSqlPrepare = NULL;
static sqlite3_step_t pSqlStep = NULL;
static sqlite3_column_int64_t pSqlColInt64 = NULL;
static sqlite3_column_text_t pSqlColText = NULL;
static sqlite3_finalize_t pSqlFinalize = NULL;
static sqlite3_exec_t pSqlExec = NULL;
static sqlite3_errmsg_t pSqlErrMsg = NULL;

// Global App State
static QueueTrack g_queue[MAX_QUEUE_TRACKS];
static int g_queueCount = 0;

static DeviceTrack *g_deviceTracks = NULL;
static int g_deviceTrackCount = 0;
static long long g_deviceAudioTotalBytes = 0;

static HWND g_hMainWnd = NULL;
static HWND g_hListView = NULL;
static HWND g_hSidebar = NULL;
static HWND g_hSearchEdit = NULL;
static HWND g_hBtnPushAlbum = NULL;
static HWND g_hBtnAddTracks = NULL;
static HWND g_hBtnExport = NULL;
static HWND g_hBtnRefresh = NULL;
static HWND g_hBtnSyncQueue = NULL;
static HWND g_hBtnRemove = NULL;
static HWND g_hBtnClear = NULL;

static HTREEITEM g_hItemLibrary = NULL;
static HTREEITEM g_hItemQueue = NULL;

static ActiveView g_activeView = VIEW_DEVICE_LIBRARY;
static wchar_t g_searchText[128] = {0};

static int g_deviceConnected = 0;
static wchar_t g_deviceName[256] = L"Searching for iPhone...";
static int g_currentDeviceId = 0;
static char g_currentDeviceUdid[64] = {0};
static HANDLE g_hDevThread = NULL;
static volatile int g_devThreadRunning = 1;
static volatile int g_isSyncing = 0;

static char g_appDir[MAX_PATH] = {0};
static char g_localDbPath[MAX_PATH] = {0};
static wchar_t g_statusText[256] = L"Ready";

// Win32 ListView Param Helpers
static void LvSetParam(HWND hList, int item, LPARAM param) {
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(LVITEMW));
    lvi.mask = LVIF_PARAM;
    lvi.iItem = item;
    lvi.iSubItem = 0;
    lvi.lParam = param;
    SendMessageW(hList, LVM_SETITEMW, 0, (LPARAM)&lvi);
}

static LPARAM LvGetParam(HWND hList, int item) {
    LVITEMW lvi;
    memset(&lvi, 0, sizeof(LVITEMW));
    lvi.mask = LVIF_PARAM;
    lvi.iItem = item;
    lvi.iSubItem = 0;
    if (SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi)) {
        return lvi.lParam;
    }
    return 0;
}

// Forward declarations
void SwitchView(ActiveView view);
void PopulateListView();
void CheckDuplicates();
int IsDuplicateOnDevice(const wchar_t *title, const wchar_t *artist);
int ReadAudioMetadata(const wchar_t *filePath, QueueTrack *track);
void AddFileToQueue(const wchar_t *filePath);
void AddFolderToQueue(const wchar_t *folderPath);
int LoadDeviceLibrary();
int LoadLocalCachedLibrary();
DWORD WINAPI SyncQueueWorker(LPVOID param);
DWORD WINAPI PushAlbumWorker(LPVOID param);
DWORD WINAPI DeviceMonitorThread(LPVOID param);
void ExportSelectedTracks();
void OnPushAlbumClicked();
void UpdateSelectedArtworkPreview();
static void PreCacheDeviceArtwork(AppleSession *pSess);
static void calc_sha1(const unsigned char *data, size_t len, char hexOut[41]);

// Normalize string for fuzzy comparison
static void NormalizeStr(const char *in, char *out, int maxLen) {
    int o = 0;
    for (int i = 0; in[i] && o < maxLen - 1; ++i) {
        char c = in[i];
        if (isalnum((unsigned char)c)) {
            out[o++] = (char)tolower((unsigned char)c);
        }
    }
    out[o] = '\0';
}

static void NormalizeWideStr(const wchar_t *in, char *out, int maxLen) {
    char utf8[512] = {0};
    WideCharToMultiByte(CP_UTF8, 0, in, -1, utf8, sizeof(utf8), NULL, NULL);
    NormalizeStr(utf8, out, maxLen);
}

int IsDuplicateOnDevice(const wchar_t *title, const wchar_t *artist) {
    if (!g_deviceTracks || g_deviceTrackCount == 0) return 0;
    char normTitle[256] = {0};
    char normArtist[256] = {0};
    NormalizeWideStr(title, normTitle, sizeof(normTitle));
    NormalizeWideStr(artist, normArtist, sizeof(normArtist));
    if (normTitle[0] == '\0') return 0;

    for (int i = 0; i < g_deviceTrackCount; ++i) {
        if (strcmp(normTitle, g_deviceTracks[i].normTitle) == 0) {
            if (normArtist[0] != '\0') {
                if (strcmp(normArtist, g_deviceTracks[i].normArtist) == 0) return 1;
            } else {
                return 1;
            }
        }
    }
    return 0;
}

void CheckDuplicates() {
    for (int i = 0; i < g_queueCount; ++i) {
        if (IsDuplicateOnDevice(g_queue[i].title, g_queue[i].artist)) {
            g_queue[i].isDuplicate = 1;
            g_queue[i].status = STATUS_DUPLICATE;
        } else {
            g_queue[i].isDuplicate = 0;
            if (g_queue[i].status == STATUS_DUPLICATE) {
                g_queue[i].status = STATUS_READY;
            }
        }
    }
}

// Synchsafe integer conversion for ID3v2
static unsigned int read_syncsafe(const unsigned char *b) {
    return ((b[0] & 0x7F) << 21) | ((b[1] & 0x7F) << 14) | ((b[2] & 0x7F) << 7) | (b[3] & 0x7F);
}

static unsigned int read_be32(const unsigned char *b) {
    return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8) | (unsigned int)b[3];
}

static void parse_text_frame(const unsigned char *data, int dataSize, wchar_t *out, int maxLen) {
    if (dataSize <= 1 || maxLen <= 1) { out[0] = L'\0'; return; }
    unsigned char enc = data[0];
    const unsigned char *payload = data + 1;
    int payloadSize = dataSize - 1;

    if (enc == 0) {
        int len = MultiByteToWideChar(CP_ACP, 0, (const char*)payload, payloadSize, out, maxLen - 1);
        out[len > 0 ? len : 0] = L'\0';
    } else if (enc == 1 && payloadSize >= 2) {
        unsigned short bom = (payload[0] | (payload[1] << 8));
        if (bom == 0xFEFF) {
            int wchars = (payloadSize - 2) / 2;
            if (wchars >= maxLen) wchars = maxLen - 1;
            memcpy(out, payload + 2, wchars * 2);
            out[wchars] = L'\0';
        } else if (bom == 0xFFFE) {
            int wchars = (payloadSize - 2) / 2;
            if (wchars >= maxLen) wchars = maxLen - 1;
            for (int i = 0; i < wchars; ++i) out[i] = (payload[2 + i*2] << 8) | payload[3 + i*2];
            out[wchars] = L'\0';
        } else {
            int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)payload, payloadSize, out, maxLen - 1);
            out[len > 0 ? len : 0] = L'\0';
        }
    } else if (enc == 3) {
        int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)payload, payloadSize, out, maxLen - 1);
        out[len > 0 ? len : 0] = L'\0';
    } else {
        int len = MultiByteToWideChar(CP_ACP, 0, (const char*)payload, payloadSize, out, maxLen - 1);
        out[len > 0 ? len : 0] = L'\0';
    }
}

int ReadAudioMetadata(const wchar_t *filePath, QueueTrack *track) {
    memset(track, 0, sizeof(QueueTrack));
    wcsncpy(track->filePath, filePath, MAX_PATH - 1);

    const wchar_t *fn = wcsrchr(filePath, L'\\');
    if (!fn) fn = wcsrchr(filePath, L'/');
    fn = fn ? fn + 1 : filePath;
    wcsncpy(track->title, fn, 255);
    wchar_t *dot = wcsrchr(track->title, L'.');
    if (dot) *dot = L'\0';

    wcscpy(track->artist, L"Unknown Artist");
    wcscpy(track->album, L"Unknown Album");
    track->albumArtist[0] = L'\0';
    wcscpy(track->track, L"-");
    wcscpy(track->year, L"-");
    track->discNum = 1;
    track->trackNum = 1;
    track->trackCountVal = 1;
    track->yearVal = 2024;
    track->durationMs = 210000;
    track->isCompilation = 0;

    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    track->fileSize = (long)GetFileSize(hFile, NULL);

    unsigned char header[10];
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, header, 10, &bytesRead, NULL) || bytesRead < 10 || memcmp(header, "ID3", 3) != 0) {
        CloseHandle(hFile);
        return 1;
    }

    int majorVer = header[3];
    unsigned int tagSize = read_syncsafe(header + 6);
    if (tagSize > 8 * 1024 * 1024) tagSize = 8 * 1024 * 1024;
    unsigned char *tagBuf = (unsigned char*)malloc(tagSize);
    if (!tagBuf) { CloseHandle(hFile); return 1; }

    if (ReadFile(hFile, tagBuf, tagSize, &bytesRead, NULL)) {
        unsigned int offset = 0;
        while (offset + 10 <= bytesRead) {
            char frameID[5] = {0};
            memcpy(frameID, tagBuf + offset, 4);
            if (frameID[0] == 0) break;
            unsigned int frameSize = (majorVer == 4) ? read_syncsafe(tagBuf + offset + 4) : read_be32(tagBuf + offset + 4);
            if (offset + 10 + frameSize > bytesRead) break;

            const unsigned char *data = tagBuf + offset + 10;
            if (strcmp(frameID, "TIT2") == 0) parse_text_frame(data, frameSize, track->title, 256);
            else if (strcmp(frameID, "TPE1") == 0) parse_text_frame(data, frameSize, track->artist, 256);
            else if (strcmp(frameID, "TPE2") == 0) parse_text_frame(data, frameSize, track->albumArtist, 256);
            else if (strcmp(frameID, "TALB") == 0) parse_text_frame(data, frameSize, track->album, 256);
            else if (strcmp(frameID, "TCMP") == 0) {
                if (frameSize > 0 && (data[0] == '1' || (frameSize > 1 && data[1] == '1'))) {
                    track->isCompilation = 1;
                }
            }
            else if (strcmp(frameID, "TRCK") == 0) {
                parse_text_frame(data, frameSize, track->track, 32);
                int tn = 0, tc = 0;
                if (swscanf(track->track, L"%d/%d", &tn, &tc) >= 1) {
                    track->trackNum = tn;
                    if (tc > 0) track->trackCountVal = tc;
                }
            }
            else if (strcmp(frameID, "TYER") == 0 || strcmp(frameID, "TDRC") == 0) {
                parse_text_frame(data, frameSize, track->year, 32);
                int yr = 0;
                if (swscanf(track->year, L"%d", &yr) == 1) track->yearVal = yr;
            }
            else if (strcmp(frameID, "TPOS") == 0) {
                wchar_t discStr[32] = {0};
                parse_text_frame(data, frameSize, discStr, 32);
                int dn = 0;
                if (swscanf(discStr, L"%d", &dn) == 1) track->discNum = dn;
            }

            offset += 10 + frameSize;
        }
    }

    free(tagBuf);
    CloseHandle(hFile);
    return 1;
}

// Draw gradient helper
static void DrawGradient(HDC hdc, RECT rect, COLORREF topColor, COLORREF bottomColor) {
    TRIVERTEX vertex[2];
    vertex[0].x = rect.left; vertex[0].y = rect.top;
    vertex[0].Red = (COLOR16)(GetRValue(topColor) << 8);
    vertex[0].Green = (COLOR16)(GetGValue(topColor) << 8);
    vertex[0].Blue = (COLOR16)(GetBValue(topColor) << 8);
    vertex[0].Alpha = 0x0000;

    vertex[1].x = rect.right; vertex[1].y = rect.bottom;
    vertex[1].Red = (COLOR16)(GetRValue(bottomColor) << 8);
    vertex[1].Green = (COLOR16)(GetGValue(bottomColor) << 8);
    vertex[1].Blue = (COLOR16)(GetBValue(bottomColor) << 8);
    vertex[1].Alpha = 0x0000;

    GRADIENT_RECT gRect = {0, 1};
    GradientFill(hdc, vertex, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
}

// Track hover state for modern buttons
static LRESULT CALLBACK ModernButtonSubclass(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!GetPropW(hWnd, L"HoverTracking")) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            SetPropW(hWnd, L"HoverTracking", (HANDLE)1);
            SetPropW(hWnd, L"IsHovered", (HANDLE)1);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        RemovePropW(hWnd, L"HoverTracking");
        RemovePropW(hWnd, L"IsHovered");
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }
    case WM_DESTROY: {
        RemovePropW(hWnd, L"HoverTracking");
        RemovePropW(hWnd, L"IsHovered");
        WNDPROC oldProc = (WNDPROC)GetPropW(hWnd, L"OldProc");
        RemovePropW(hWnd, L"OldProc");
        if (oldProc) {
            SetWindowLongPtrW(hWnd, GWLP_WNDPROC, (LONG_PTR)oldProc);
            return CallWindowProcW(oldProc, hWnd, msg, wParam, lParam);
        }
        break;
    }
    }
    WNDPROC oldProc = (WNDPROC)GetPropW(hWnd, L"OldProc");
    if (oldProc) return CallWindowProcW(oldProc, hWnd, msg, wParam, lParam);
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void SubclassModernButton(HWND hBtn) {
    if (!hBtn) return;
    WNDPROC oldProc = (WNDPROC)SetWindowLongPtrW(hBtn, GWLP_WNDPROC, (LONG_PTR)ModernButtonSubclass);
    SetPropW(hBtn, L"OldProc", (HANDLE)oldProc);
}

// Render custom glossy modern button with high-DPI Segoe UI, subtle gradient, specular sheen & hover
static void DrawGlossyButton(DRAWITEMSTRUCT *dis, int isHovered, const wchar_t *text,
                             COLORREF topCol, COLORREF botCol,
                             COLORREF hovTopCol, COLORREF hovBotCol,
                             COLORREF pressTopCol, COLORREF pressBotCol,
                             COLORREF disTopCol, COLORREF disBotCol,
                             COLORREF borderCol, COLORREF borderHovCol,
                             COLORREF textCol, COLORREF textDisCol,
                             COLORREF sheenCol, int cornerRadius, HFONT hFont) {
    int w = dis->rcItem.right - dis->rcItem.left;
    int h = dis->rcItem.bottom - dis->rcItem.top;
    if (w <= 0 || h <= 0) return;

    HDC memDC = CreateCompatibleDC(dis->hDC);
    HBITMAP memBmp = CreateCompatibleBitmap(dis->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    // Parent background fill behind the rounded corners
    RECT rcFull = {0, 0, w, h};
    HBRUSH bgBrush = CreateSolidBrush(COLOR_TOOLBAR_BOT);
    FillRect(memDC, &rcFull, bgBrush);
    DeleteObject(bgBrush);

    int isDisabled = (dis->itemState & ODS_DISABLED);
    int isPressed = (dis->itemState & ODS_SELECTED);

    COLORREF curTop = topCol;
    COLORREF curBot = botCol;
    COLORREF curBorder = borderCol;
    COLORREF curText = textCol;
    COLORREF curSheen = sheenCol;

    if (isDisabled) {
        curTop = disTopCol;
        curBot = disBotCol;
        curBorder = RGB(218, 222, 228);
        curText = textDisCol;
        curSheen = 0;
    } else if (isPressed) {
        curTop = pressTopCol;
        curBot = pressBotCol;
        curBorder = borderCol;
        curText = textCol;
        curSheen = 0;
    } else if (isHovered) {
        curTop = hovTopCol;
        curBot = hovBotCol;
        curBorder = borderHovCol;
        curText = textCol;
        curSheen = sheenCol;
    }

    // Clip to rounded rectangle
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, cornerRadius, cornerRadius);
    SelectClipRgn(memDC, rgn);

    // Vertical gradient
    DrawGradient(memDC, rcFull, curTop, curBot);

    // Glossy top specular reflection highlight (Apple sheen)
    if (curSheen != 0 && !isPressed && !isDisabled && h > 10) {
        HPEN sheenPen = CreatePen(PS_SOLID, 1, curSheen);
        HGDIOBJ oldP = SelectObject(memDC, sheenPen);
        int margin = cornerRadius / 2;
        if (margin < 3) margin = 3;
        MoveToEx(memDC, margin, 1, NULL);
        LineTo(memDC, w - margin, 1);
        SelectObject(memDC, oldP);
        DeleteObject(sheenPen);
    }

    SelectClipRgn(memDC, NULL);
    DeleteObject(rgn);

    // Rounded Border
    HPEN bPen = CreatePen(PS_SOLID, 1, curBorder);
    HGDIOBJ oldPen = SelectObject(memDC, bPen);
    HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
    RoundRect(memDC, 0, 0, w, h, cornerRadius, cornerRadius);
    SelectObject(memDC, oldBrush);
    SelectObject(memDC, oldPen);
    DeleteObject(bPen);

    // Draw text
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, curText);
    HGDIOBJ oldFont = SelectObject(memDC, hFont);
    RECT rcText = {0, 0, w, h};
    if (isPressed) {
        OffsetRect(&rcText, 1, 1);
    }
    DrawTextW(memDC, text, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(memDC, oldFont);

    // Copy to button DC
    BitBlt(dis->hDC, dis->rcItem.left, dis->rcItem.top, w, h, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

// Reveal local audio file in Windows Explorer
static void RevealInExplorer(const wchar_t *path) {
    if (!path || path[0] == L'\0') return;
    wchar_t param[MAX_PATH + 32];
    swprintf(param, sizeof(param)/sizeof(wchar_t), L"/select,\"%ls\"", path);
    ShellExecuteW(NULL, L"open", L"explorer.exe", param, NULL, SW_SHOWNORMAL);
}

// Copy selected tracks info to Windows Clipboard
static void CopySelectedToClipboard(HWND hList) {
    int selCount = ListView_GetSelectedCount(hList);
    if (selCount <= 0) return;

    size_t estLen = (size_t)selCount * 512 + 64;
    wchar_t *buf = (wchar_t*)malloc(estLen * sizeof(wchar_t));
    if (!buf) return;
    buf[0] = L'\0';

    int row = -1;
    while ((row = ListView_GetNextItem(hList, row, LVNI_SELECTED)) != -1) {
        wchar_t title[256] = {0};
        wchar_t artist[256] = {0};
        if (g_activeView == VIEW_DEVICE_LIBRARY) {
            ListView_GetItemText(hList, row, 1, title, 256);
            ListView_GetItemText(hList, row, 2, artist, 256);
        } else {
            ListView_GetItemText(hList, row, 2, title, 256);
            ListView_GetItemText(hList, row, 3, artist, 256);
        }
        wchar_t line[512];
        if (artist[0] != L'\0') {
            swprintf(line, 512, L"%ls - %ls\r\n", artist, title);
        } else {
            swprintf(line, 512, L"%ls\r\n", title);
        }
        if (wcslen(buf) + wcslen(line) < estLen) {
            wcscat(buf, line);
        }
    }

    if (OpenClipboard(g_hMainWnd)) {
        EmptyClipboard();
        size_t bytes = (wcslen(buf) + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void *pMem = GlobalLock(hMem);
            if (pMem) {
                memcpy(pMem, buf, bytes);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        CloseClipboard();
    }
    free(buf);
}

// Search Edit Subclass to handle Escape key
static WNDPROC s_oldEditProc = NULL;
static LRESULT CALLBACK SearchEditSubclass(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        SetWindowTextW(hWnd, L"");
        if (g_hListView) SetFocus(g_hListView);
        return 0;
    }
    return CallWindowProcW(s_oldEditProc, hWnd, msg, wParam, lParam);
}

// Switch between Library View and Transfer Queue View
void SwitchView(ActiveView view) {
    g_activeView = view;

    while (ListView_DeleteColumn(g_hListView, 0));

    LVCOLUMNW lvc;
    memset(&lvc, 0, sizeof(LVCOLUMNW));
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    if (view == VIEW_DEVICE_LIBRARY) {
        lvc.iSubItem = 0; lvc.cx = 45;  lvc.pszText = L"#";           ListView_InsertColumn(g_hListView, 0, &lvc);
        lvc.iSubItem = 1; lvc.cx = 260; lvc.pszText = L"Title";       ListView_InsertColumn(g_hListView, 1, &lvc);
        lvc.iSubItem = 2; lvc.cx = 180; lvc.pszText = L"Artist";      ListView_InsertColumn(g_hListView, 2, &lvc);
        lvc.iSubItem = 3; lvc.cx = 200; lvc.pszText = L"Album";       ListView_InsertColumn(g_hListView, 3, &lvc);
        lvc.iSubItem = 4; lvc.cx = 65;  lvc.pszText = L"Time";        ListView_InsertColumn(g_hListView, 4, &lvc);
        lvc.iSubItem = 5; lvc.cx = 75;  lvc.pszText = L"Size";        ListView_InsertColumn(g_hListView, 5, &lvc);
        lvc.iSubItem = 6; lvc.cx = 140; lvc.pszText = L"Location";    ListView_InsertColumn(g_hListView, 6, &lvc);

        EnableWindow(g_hBtnExport, TRUE);
        ShowWindow(g_hBtnRemove, SW_HIDE);
        ShowWindow(g_hBtnClear, SW_HIDE);
        ShowWindow(g_hBtnSyncQueue, SW_HIDE);
    } else {
        lvc.iSubItem = 0; lvc.cx = 40;  lvc.pszText = L"#";           ListView_InsertColumn(g_hListView, 0, &lvc);
        lvc.iSubItem = 1; lvc.cx = 145; lvc.pszText = L"Status";      ListView_InsertColumn(g_hListView, 1, &lvc);
        lvc.iSubItem = 2; lvc.cx = 220; lvc.pszText = L"Title";       ListView_InsertColumn(g_hListView, 2, &lvc);
        lvc.iSubItem = 3; lvc.cx = 160; lvc.pszText = L"Artist";      ListView_InsertColumn(g_hListView, 3, &lvc);
        lvc.iSubItem = 4; lvc.cx = 160; lvc.pszText = L"Album";       ListView_InsertColumn(g_hListView, 4, &lvc);
        lvc.iSubItem = 5; lvc.cx = 75;  lvc.pszText = L"Size";        ListView_InsertColumn(g_hListView, 5, &lvc);
        lvc.iSubItem = 6; lvc.cx = 260; lvc.pszText = L"Local Path";  ListView_InsertColumn(g_hListView, 6, &lvc);

        EnableWindow(g_hBtnExport, FALSE);
        ShowWindow(g_hBtnRemove, SW_SHOW);
        ShowWindow(g_hBtnClear, SW_SHOW);
        ShowWindow(g_hBtnSyncQueue, SW_SHOW);
    }

    PopulateListView();
    InvalidateRect(g_hMainWnd, NULL, FALSE);
}

// Populate ListView rows based on current view and search filter
void PopulateListView() {
    SendMessage(g_hListView, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hListView);

    wchar_t filter[128] = {0};
    if (g_hSearchEdit) {
        GetWindowTextW(g_hSearchEdit, filter, 128);
        for (int i = 0; filter[i]; ++i) filter[i] = towlower(filter[i]);
    }

    LVITEMW lvi;
    memset(&lvi, 0, sizeof(LVITEMW));
    lvi.mask = LVIF_TEXT;

    if (g_activeView == VIEW_DEVICE_LIBRARY) {
        int rowIdx = 0;
        for (int i = 0; i < g_deviceTrackCount; ++i) {
            DeviceTrack *dt = &g_deviceTracks[i];

            wchar_t wTitle[256] = {0};
            wchar_t wArtist[256] = {0};
            wchar_t wAlbum[256] = {0};
            MultiByteToWideChar(CP_UTF8, 0, dt->title, -1, wTitle, 256);
            MultiByteToWideChar(CP_UTF8, 0, dt->artist, -1, wArtist, 256);
            MultiByteToWideChar(CP_UTF8, 0, dt->album, -1, wAlbum, 256);

            // Filter check
            if (filter[0] != L'\0') {
                wchar_t lowT[256], lowA[256], lowAl[256];
                int lenT = wcslen(wTitle); for (int k = 0; k < lenT; ++k) lowT[k] = towlower(wTitle[k]); lowT[lenT] = 0;
                int lenA = wcslen(wArtist); for (int k = 0; k < lenA; ++k) lowA[k] = towlower(wArtist[k]); lowA[lenA] = 0;
                int lenAl = wcslen(wAlbum); for (int k = 0; k < lenAl; ++k) lowAl[k] = towlower(wAlbum[k]); lowAl[lenAl] = 0;

                if (!wcsstr(lowT, filter) && !wcsstr(lowA, filter) && !wcsstr(lowAl, filter)) {
                    continue;
                }
            }

            lvi.iItem = rowIdx;
            lvi.iSubItem = 0;
            wchar_t numStr[16];
            swprintf(numStr, 16, L"%d", rowIdx + 1);
            lvi.pszText = numStr;
            ListView_InsertItem(g_hListView, &lvi);

            ListView_SetItemText(g_hListView, rowIdx, 1, wTitle);
            ListView_SetItemText(g_hListView, rowIdx, 2, wArtist);
            ListView_SetItemText(g_hListView, rowIdx, 3, wAlbum);

            wchar_t timeStr[32];
            int mins = (int)(dt->durationMs / 60000);
            int secs = (int)((dt->durationMs % 60000) / 1000);
            swprintf(timeStr, 32, L"%d:%02d", mins, secs);
            ListView_SetItemText(g_hListView, rowIdx, 4, timeStr);

            wchar_t sizeStr[32];
            swprintf(sizeStr, 32, L"%.1f MB", (double)dt->fileSize / (1024.0 * 1024.0));
            ListView_SetItemText(g_hListView, rowIdx, 5, sizeStr);

            wchar_t locStr[64];
            MultiByteToWideChar(CP_UTF8, 0, dt->location, -1, locStr, 64);
            ListView_SetItemText(g_hListView, rowIdx, 6, locStr);

            LvSetParam(g_hListView, rowIdx, (LPARAM)i);
            rowIdx++;
        }

        swprintf(g_statusText, 256, L"Showing %d of %d songs in iPhone library", rowIdx, g_deviceTrackCount);
    } else {
        // VIEW_TRANSFER_QUEUE
        for (int i = 0; i < g_queueCount; ++i) {
            QueueTrack *qt = &g_queue[i];

            lvi.iItem = i;
            lvi.iSubItem = 0;
            wchar_t numStr[16];
            swprintf(numStr, 16, L"%d", i + 1);
            lvi.pszText = numStr;
            ListView_InsertItem(g_hListView, &lvi);

            const wchar_t *st = L"Ready to Sync";
            if (qt->status == STATUS_DUPLICATE) st = L"✓ In Library (Skipped)";
            else if (qt->status == STATUS_SYNCING) st = L"⚡ Syncing...";
            else if (qt->status == STATUS_SYNCED) st = L"✓ Synced to iPhone";
            else if (qt->status == STATUS_ERROR) st = L"❌ Error";

            ListView_SetItemText(g_hListView, i, 1, (LPWSTR)st);
            ListView_SetItemText(g_hListView, i, 2, qt->title);
            ListView_SetItemText(g_hListView, i, 3, qt->artist);
            ListView_SetItemText(g_hListView, i, 4, qt->album);

            wchar_t sizeStr[32];
            swprintf(sizeStr, 32, L"%.2f MB", (double)qt->fileSize / (1024.0 * 1024.0));
            ListView_SetItemText(g_hListView, i, 5, sizeStr);
            ListView_SetItemText(g_hListView, i, 6, qt->filePath);
        }

        if (g_queueCount == 0) {
            swprintf(g_statusText, 256, L"Queue is empty. Drop music files here or click 'Push Album'.");
        } else {
            int newCnt = 0, dupCnt = 0;
            for (int i = 0; i < g_queueCount; ++i) {
                if (g_queue[i].isDuplicate) dupCnt++;
                else newCnt++;
            }
            if (dupCnt > 0) {
                swprintf(g_statusText, 256, L"Queue: %d tracks (%d new, %d already synced)", g_queueCount, newCnt, dupCnt);
            } else {
                swprintf(g_statusText, 256, L"Queue: %d tracks ready to sync", g_queueCount);
            }
        }
    }

    if (g_hSidebar && g_hItemQueue) {
        wchar_t qTitle[64];
        swprintf(qTitle, 64, L"Transfer Queue (%d)", g_queueCount);
        TVITEMW tviQ; memset(&tviQ, 0, sizeof(tviQ));
        tviQ.hItem = g_hItemQueue; tviQ.mask = TVIF_TEXT; tviQ.pszText = qTitle;
        SendMessageW(g_hSidebar, TVM_SETITEMW, 0, (LPARAM)&tviQ);
    }

    SendMessage(g_hListView, WM_SETREDRAW, TRUE, 0);
    UpdateSelectedArtworkPreview();
    UpdateWindow(g_hListView);
}

void AddFileToQueue(const wchar_t *filePath) {
    if (g_queueCount >= MAX_QUEUE_TRACKS) return;
    for (int i = 0; i < g_queueCount; ++i) {
        if (_wcsicmp(g_queue[i].filePath, filePath) == 0) return;
    }

    QueueTrack t;
    if (!ReadAudioMetadata(filePath, &t)) return;

    if (IsDuplicateOnDevice(t.title, t.artist)) {
        t.isDuplicate = 1;
        t.status = STATUS_DUPLICATE;
    } else {
        t.isDuplicate = 0;
        t.status = STATUS_READY;
    }

    g_queue[g_queueCount++] = t;
}

void AddFolderToQueue(const wchar_t *folderPath) {
    wchar_t searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%ls\\*", folderPath);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t fullPath[MAX_PATH];
        swprintf(fullPath, MAX_PATH, L"%ls\\%ls", folderPath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            AddFolderToQueue(fullPath);
        } else {
            const wchar_t *ext = wcsrchr(fd.cFileName, L'.');
            if (ext) {
                if (_wcsicmp(ext, L".mp3") == 0 || _wcsicmp(ext, L".m4a") == 0 ||
                    _wcsicmp(ext, L".aac") == 0 || _wcsicmp(ext, L".flac") == 0 ||
                    _wcsicmp(ext, L".wav") == 0) {
                    AddFileToQueue(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

// Determine unified Album Artist & Compilation status for any track in an album
static void ResolveAlbumArtistForTrack(QueueTrack *t, wchar_t *outAlbumArtist, int maxLen, int *outIsComp) {
    *outIsComp = t->isCompilation;
    if (t->albumArtist[0] != L'\0') {
        wcsncpy(outAlbumArtist, t->albumArtist, maxLen - 1);
        outAlbumArtist[maxLen - 1] = L'\0';
        return;
    }

    // Check if any other track in the queue with the same album has an explicit albumArtist
    for (int k = 0; k < g_queueCount; ++k) {
        if (_wcsicmp(g_queue[k].album, t->album) == 0 && g_queue[k].albumArtist[0] != L'\0') {
            wcsncpy(outAlbumArtist, g_queue[k].albumArtist, maxLen - 1);
            outAlbumArtist[maxLen - 1] = L'\0';
            return;
        }
    }

    // Count artist occurrences among tracks in the same album
    int totalAlbumTracks = 0;
    int maxCount = 0;
    const wchar_t *dominantArtist = t->artist;

    for (int i = 0; i < g_queueCount; ++i) {
        if (_wcsicmp(g_queue[i].album, t->album) == 0) {
            totalAlbumTracks++;
            int c = 0;
            for (int j = 0; j < g_queueCount; ++j) {
                if (_wcsicmp(g_queue[j].album, t->album) == 0 && _wcsicmp(g_queue[j].artist, g_queue[i].artist) == 0) {
                    c++;
                }
            }
            if (c > maxCount) {
                maxCount = c;
                dominantArtist = g_queue[i].artist;
            }
        }
    }

    if (maxCount >= (totalAlbumTracks + 2) / 3 || maxCount >= 2) {
        wcsncpy(outAlbumArtist, dominantArtist, maxLen - 1);
        outAlbumArtist[maxLen - 1] = L'\0';
    } else {
        wcsncpy(outAlbumArtist, L"Various Artists", maxLen - 1);
        outAlbumArtist[maxLen - 1] = L'\0';
        *outIsComp = 1;
    }
}

// Push Album directly to iPhone
void OnPushAlbumClicked() {
    if (!g_deviceConnected || !g_currentDeviceId) {
        MessageBoxW(g_hMainWnd, L"Connect your iPhone via USB first.", L"iPhone Not Connected", MB_ICONWARNING);
        return;
    }

    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(BROWSEINFOW));
    bi.hwndOwner = g_hMainWnd;
    bi.lpszTitle = L"Select an album folder:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t albumFolder[MAX_PATH];
    if (!SHGetPathFromIDListW(pidl, albumFolder)) {
        CoTaskMemFree(pidl);
        return;
    }
    CoTaskMemFree(pidl);

    // Save previous count
    int prevQueueCount = g_queueCount;
    AddFolderToQueue(albumFolder);
    int addedCount = g_queueCount - prevQueueCount;

    if (addedCount == 0) {
        MessageBoxW(g_hMainWnd, L"No supported audio files found in folder.", L"No Audio Files", MB_ICONINFORMATION);
        return;
    }

    wchar_t *albumName = g_queue[prevQueueCount].album;
    wchar_t artistName[256] = {0};
    int isComp = 0;
    ResolveAlbumArtistForTrack(&g_queue[prevQueueCount], artistName, 256, &isComp);

    int newTracks = 0, dupTracks = 0;
    for (int i = prevQueueCount; i < g_queueCount; ++i) {
        if (g_queue[i].isDuplicate) dupTracks++;
        else newTracks++;
    }

    if (newTracks == 0 && addedCount > 0) {
        wchar_t alreadyMsg[512];
        swprintf(alreadyMsg, 512, L"All tracks in \"%ls\" are already on your iPhone.", albumName);
        MessageBoxW(g_hMainWnd, alreadyMsg, L"Already Synced", MB_ICONINFORMATION);
        SwitchView(VIEW_TRANSFER_QUEUE);
        SendMessageW(g_hSidebar, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_hItemQueue);
        return;
    }

    wchar_t confirmMsg[512];
    if (dupTracks > 0) {
        swprintf(confirmMsg, 512, L"Transfer %d new track%ls from \"%ls\"?\n(%d already on device)",
                 newTracks, newTracks == 1 ? L"" : L"s", albumName, dupTracks);
    } else {
        swprintf(confirmMsg, 512, L"Transfer %d track%ls from \"%ls\" to your iPhone?",
                 newTracks, newTracks == 1 ? L"" : L"s", albumName);
    }

    if (MessageBoxW(g_hMainWnd, confirmMsg, L"Push Album", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        // Start push worker thread
        CreateThread(NULL, 0, SyncQueueWorker, NULL, 0, NULL);
    } else {
        // Switch to queue so user can inspect
        SwitchView(VIEW_TRANSFER_QUEUE);
        SendMessageW(g_hSidebar, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_hItemQueue);
    }
}

void OnAddTracksClicked() {
    wchar_t fileBuf[32768] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(OPENFILENAMEW));
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Music Files (*.mp3;*.m4a;*.aac;*.flac;*.wav)\0*.mp3;*.m4a;*.aac;*.flac;*.wav\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        wchar_t *p = fileBuf;
        wchar_t dir[MAX_PATH];
        wcsncpy(dir, p, MAX_PATH - 1);
        p += wcslen(p) + 1;

        if (*p == L'\0') {
            AddFileToQueue(dir);
        } else {
            while (*p) {
                wchar_t fullPath[MAX_PATH];
                swprintf(fullPath, MAX_PATH, L"%ls\\%ls", dir, p);
                AddFileToQueue(fullPath);
                p += wcslen(p) + 1;
            }
        }
        CheckDuplicates();
        SwitchView(VIEW_TRANSFER_QUEUE);
        SendMessageW(g_hSidebar, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_hItemQueue);
    }
}

// CopyTrans Feature: Export selected tracks from iPhone directly to a folder on PC
void ExportSelectedTracks() {
    if (g_activeView != VIEW_DEVICE_LIBRARY || !g_deviceConnected || !g_currentDeviceId) return;

    int selCount = ListView_GetSelectedCount(g_hListView);
    if (selCount == 0) {
        MessageBoxW(g_hMainWnd, L"Select tracks to export first.", L"Export", MB_ICONINFORMATION);
        return;
    }

    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(BROWSEINFOW));
    bi.hwndOwner = g_hMainWnd;
    bi.lpszTitle = L"Select destination folder on PC:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t destFolder[MAX_PATH];
    if (!SHGetPathFromIDListW(pidl, destFolder)) {
        CoTaskMemFree(pidl);
        return;
    }
    CoTaskMemFree(pidl);

    AppleSession sess;
    if (!Apple_StartSession(&sess, g_currentDeviceId, g_currentDeviceUdid)) {
        MessageBoxW(g_hMainWnd, L"Failed to connect to device storage.", L"Export Error", MB_ICONERROR);
        return;
    }

    Apple_PostNotification(&sess, "com.apple.itunes-mobdev.syncWillStart");
    Sleep(200);

    int exported = 0;
    int idx = -1;
    while ((idx = ListView_GetNextItem(g_hListView, idx, LVNI_SELECTED)) != -1) {
        int origIdx = (int)LvGetParam(g_hListView, idx);
        if (origIdx < 0 || origIdx >= g_deviceTrackCount) continue;
        DeviceTrack *dt = &g_deviceTracks[origIdx];

        char remotePath[512];
        snprintf(remotePath, sizeof(remotePath), "/%s/%s", dt->remoteDir, dt->location);

        unsigned long long hRemote = 0;
        if (Apple_AfcOpenFile(&sess, remotePath, AFC_MODE_RDONLY, &hRemote) == 0 && hRemote) {
            wchar_t wTitle[256] = {0};
            wchar_t wArtist[256] = {0};
            MultiByteToWideChar(CP_UTF8, 0, dt->title, -1, wTitle, 256);
            MultiByteToWideChar(CP_UTF8, 0, dt->artist, -1, wArtist, 256);

            for (int k = 0; wTitle[k]; ++k) if (wcschr(L"\\/:*?\"<>|", wTitle[k])) wTitle[k] = L'_';
            for (int k = 0; wArtist[k]; ++k) if (wcschr(L"\\/:*?\"<>|", wArtist[k])) wArtist[k] = L'_';

            wchar_t outPath[MAX_PATH];
            swprintf(outPath, MAX_PATH, L"%ls\\%ls - %ls.mp3", destFolder, wArtist, wTitle);

            FILE *fOut = _wfopen(outPath, L"wb");
            if (fOut) {
                char chunk[32768];
                unsigned long long rBytes = 0;
                while (Apple_AfcReadFile(&sess, hRemote, chunk, sizeof(chunk), &rBytes) == 0 && rBytes > 0) {
                    fwrite(chunk, 1, (size_t)rBytes, fOut);
                    rBytes = 0;
                }
                fclose(fOut);
                exported++;
            }
            Apple_AfcCloseFile(&sess, hRemote);
        }
    }

    Apple_StopSession(&sess);

    wchar_t msg[256];
    swprintf(msg, 256, L"Exported %d track%ls to:\n%ls", exported, exported == 1 ? L"" : L"s", destFolder);
    MessageBoxW(g_hMainWnd, msg, L"Export Complete", MB_ICONINFORMATION);
}

// Load library from local cached SQLite db
int LoadLocalCachedLibrary() {
    if (!pSqlOpen || !pSqlPrepare) return 0;
    sqlite3 *db = NULL;
    if (pSqlOpen(g_localDbPath, &db) != 0 || !db) return 0;

    if (!g_deviceTracks) {
        g_deviceTracks = (DeviceTrack*)malloc(sizeof(DeviceTrack) * MAX_DEVICE_TRACKS);
    }
    g_deviceTrackCount = 0;
    g_deviceAudioTotalBytes = 0;

    const char *query = 
        "SELECT item.item_pid, "
        "       COALESCE(item_extra.title, 'Unknown Title'), "
        "       COALESCE(item_artist.item_artist, 'Unknown Artist'), "
        "       COALESCE(album.album, 'Unknown Album'), "
        "       COALESCE(item_extra.location, ''), "
        "       COALESCE(base_location.path, 'iTunes_Control/Music/F00'), "
        "       COALESCE(item_extra.file_size, 0), "
        "       COALESCE(item_extra.total_time_ms, 0), "
        "       COALESCE(item.track_number, 0), "
        "       COALESCE(at_alb.artwork_token, at_item.artwork_token, '') "
        "FROM item "
        "LEFT JOIN item_extra ON item.item_pid=item_extra.item_pid "
        "LEFT JOIN item_artist ON item.item_artist_pid=item_artist.item_artist_pid "
        "LEFT JOIN album ON item.album_pid=album.album_pid "
        "LEFT JOIN base_location ON item.base_location_id=base_location.base_location_id "
        "LEFT JOIN artwork_token at_alb ON (item.album_pid=at_alb.entity_pid AND at_alb.entity_type=4) "
        "LEFT JOIN artwork_token at_item ON (item.item_pid=at_item.entity_pid AND at_item.entity_type=0);";

    sqlite3_stmt *stmt = NULL;
    if (pSqlPrepare(db, query, -1, &stmt, NULL) == 0 && stmt) {
        while (pSqlStep(stmt) == 100 && g_deviceTrackCount < MAX_DEVICE_TRACKS) {
            DeviceTrack *dt = &g_deviceTracks[g_deviceTrackCount];
            dt->item_pid = pSqlColInt64(stmt, 0);
            const char *t = (const char*)pSqlColText(stmt, 1);
            const char *a = (const char*)pSqlColText(stmt, 2);
            const char *al = (const char*)pSqlColText(stmt, 3);
            const char *loc = (const char*)pSqlColText(stmt, 4);
            const char *dir = (const char*)pSqlColText(stmt, 5);
            long long fSize = pSqlColInt64(stmt, 6);
            long long dur = pSqlColInt64(stmt, 7);
            int trk = (int)pSqlColInt64(stmt, 8);
            const char *tok = (const char*)pSqlColText(stmt, 9);

            snprintf(dt->title, sizeof(dt->title), "%s", t ? t : "");
            snprintf(dt->artist, sizeof(dt->artist), "%s", a ? a : "");
            snprintf(dt->album, sizeof(dt->album), "%s", al ? al : "");
            NormalizeStr(dt->title, dt->normTitle, sizeof(dt->normTitle));
            NormalizeStr(dt->artist, dt->normArtist, sizeof(dt->normArtist));
            snprintf(dt->location, sizeof(dt->location), "%s", loc ? loc : "");
            snprintf(dt->remoteDir, sizeof(dt->remoteDir), "%s", dir ? dir : "iTunes_Control/Music/F00");
            snprintf(dt->artworkToken, sizeof(dt->artworkToken), "%s", tok ? tok : "");
            dt->fileSize = fSize;
            dt->durationMs = dur;
            dt->trackNum = trk;

            g_deviceAudioTotalBytes += fSize;
            g_deviceTrackCount++;
        }
        pSqlFinalize(stmt);
    }
    pSqlClose(db);
    return 1;
}

static void PreCacheDeviceArtwork(AppleSession *pSess) {
    if (!pSess) return;

    char cacheDir[MAX_PATH];
    snprintf(cacheDir, sizeof(cacheDir), "%s\\ArtworkCache", g_appDir);
    CreateDirectoryA(cacheDir, NULL);

    char visitedTokens[512][32];
    int visitedCount = 0;

    for (int i = 0; i < g_deviceTrackCount && visitedCount < 512; i++) {
        const char *tok = g_deviceTracks[i].artworkToken;
        if (!tok || !tok[0]) continue;

        int seen = 0;
        for (int v = 0; v < visitedCount; v++) {
            if (strcmp(visitedTokens[v], tok) == 0) { seen = 1; break; }
        }
        if (seen) continue;
        strncpy(visitedTokens[visitedCount++], tok, 31);

        char localJpg[MAX_PATH];
        snprintf(localJpg, sizeof(localJpg), "%s\\%s.jpg", cacheDir, tok);

        wchar_t wLocalJpg[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, localJpg, -1, wLocalJpg, MAX_PATH);

        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(wLocalJpg, GetFileExInfoStandard, &fad) && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (fad.nFileSizeLow > 0 || fad.nFileSizeHigh > 0) continue; // Already valid on disk
            DeleteFileW(wLocalJpg); // Delete corrupt 0-byte file
        }

        char sha1Hex[41] = {0};
        calc_sha1((const unsigned char*)tok, strlen(tok), sha1Hex);

        char remotePath[256];
        snprintf(remotePath, sizeof(remotePath), "/iTunes_Control/iTunes/Artwork/Originals/%.2s/%s", sha1Hex, sha1Hex + 2);

        unsigned long long hRemote = 0;
        if (Apple_AfcOpenFile(pSess, remotePath, AFC_MODE_RDONLY, &hRemote) == 0 && hRemote) {
            FILE *fOut = fopen(localJpg, "wb");
            if (fOut) {
                char buf[65536];
                unsigned long long bytesRead = 0;
                unsigned long long totalRead = 0;
                while (Apple_AfcReadFile(pSess, hRemote, buf, sizeof(buf), &bytesRead) == 0 && bytesRead > 0) {
                    fwrite(buf, 1, (size_t)bytesRead, fOut);
                    totalRead += bytesRead;
                    bytesRead = 0;
                }
                fclose(fOut);
                if (totalRead == 0) DeleteFileA(localJpg);
            }
            Apple_AfcCloseFile(pSess, hRemote);
        }
    }
}

// Pull iPhone database and read all tracks
int LoadDeviceLibrary() {
    AppleSession sess;
    if (!Apple_StartSession(&sess, g_currentDeviceId, g_currentDeviceUdid)) return 0;

    unsigned long long file = 0;
    int r = Apple_AfcOpenFile(&sess, "/iTunes_Control/iTunes/MediaLibrary.sqlitedb", AFC_MODE_RDONLY, &file);
    if (r == 0 && file) {
        FILE *localF = fopen(g_localDbPath, "wb");
        if (localF) {
            char *buf = (char*)malloc(262144);
            if (buf) {
                unsigned long long bytesRead = 0;
                while (Apple_AfcReadFile(&sess, file, buf, 262144, &bytesRead) == 0 && bytesRead > 0) {
                    fwrite(buf, 1, (size_t)bytesRead, localF);
                    bytesRead = 0;
                }
                free(buf);
            }
            fclose(localF);
        }
        Apple_AfcCloseFile(&sess, file);
    }

    int ok = LoadLocalCachedLibrary();
    if (ok) {
        PreCacheDeviceArtwork(&sess);
    }
    Apple_StopSession(&sess);
    return ok;
}

DWORD WINAPI DeviceMonitorThread(LPVOID param) {
    while (g_devThreadRunning) {
        int devId = 0;
        char udid[64] = {0};
        if (Apple_FindDevice(&devId, udid, sizeof(udid))) {
            if (!g_deviceConnected || g_currentDeviceId != devId) {
                g_currentDeviceId = devId;
                strncpy(g_currentDeviceUdid, udid, sizeof(g_currentDeviceUdid) - 1);
                int ok = LoadDeviceLibrary();
                if (ok) {
                    g_deviceConnected = 1;
                    wcsncpy(g_deviceName, L"iPhone Connected", 255);
                    PostMessageW(g_hMainWnd, WM_APP_DEVICE_CONNECTED, 0, 0);
                }
            }
        } else {
            if (g_deviceConnected) {
                g_deviceConnected = 0;
                g_currentDeviceId = 0;
                PostMessageW(g_hMainWnd, WM_APP_DEVICE_DISCONNECTED, 0, 0);
            }
        }
        Sleep(1000);
    }
    return 0;
}

// Apple MediaLibrary Calibrated Section Order Mapping
static long long GetSectionOrder(char c, long long pid) {
    static const int bases[26] = {
        20, 110, 200, 280, 360, 410,
        450, 490, 540, 590, 615, 640,
        700, 735, 760, 800, 840, 920,
        980, 1080, 1120, 1140, 1160, 1180,
        1195, 1205
    };
    c = (char)toupper((unsigned char)c);
    int hi = 1210;
    if (c >= 'A' && c <= 'Z') {
        hi = bases[c - 'A'];
    }
    return (((long long)hi) << 32) | (pid & 0xFFFFFFFF);
}

static void CapitalizeSort(const char *in, char *out, int maxLen) {
    if (!in || !out || maxLen <= 0) return;
    strncpy(out, in, maxLen - 1);
    out[maxLen - 1] = '\0';
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)toupper((unsigned char)out[0]);
    }
}

static void EncodeGroupingKey(const char *src, char *hexOut, int maxHexLen) {
    if (!src || !hexOut || maxHexLen < 8) return;
    hexOut[0] = '\0';
    unsigned char temp[512];
    int o = 0;
    for (int i = 0; src[i] && o < (int)sizeof(temp) - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        unsigned char u = 0;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            u = (unsigned char)toupper(c);
        } else if (c == 0xC3 && src[i+1]) { // UTF-8 Latin-1 Supplement accented characters
            unsigned char c2 = (unsigned char)src[++i];
            if ((c2 >= 0x80 && c2 <= 0x85) || (c2 >= 0xA0 && c2 <= 0xA5)) u = 'A';
            else if ((c2 >= 0x88 && c2 <= 0x8B) || (c2 >= 0xA8 && c2 <= 0xAB)) u = 'E';
            else if ((c2 >= 0x8C && c2 <= 0x8F) || (c2 >= 0xAC && c2 <= 0xAF)) u = 'I';
            else if ((c2 >= 0x92 && c2 <= 0x96) || (c2 >= 0xB2 && c2 <= 0xB6)) u = 'O';
            else if ((c2 >= 0x99 && c2 <= 0x9C) || (c2 >= 0xB9 && c2 <= 0xBC)) u = 'U';
            else if (c2 == 0x91 || c2 == 0xB1) u = 'N';
            else if (c2 == 0x87 || c2 == 0xA7) u = 'C';
        }
        if (u >= 'A' && u <= 'Z') {
            temp[o++] = (unsigned char)(0x2B + 2 * (u - 'A'));
        } else if (c == ' ') {
            temp[o++] = 0x04;
        } else if (c >= '0' && c <= '9') {
            temp[o++] = 0x10;
            temp[o++] = (unsigned char)(0x02 + (c - '0'));
        } else if (c == '&') {
            temp[o++] = 0x06;
        } else if (c == '-') {
            temp[o++] = 0x05;
        }
    }
    if (o * 2 + 4 < maxHexLen) {
        int ho = 0;
        hexOut[ho++] = 'X';
        hexOut[ho++] = '\'';
        for (int i = 0; i < o; i++) {
            sprintf(hexOut + ho, "%02X", temp[i]);
            ho += 2;
        }
        hexOut[ho++] = '\'';
        hexOut[ho] = '\0';
    } else {
        strcpy(hexOut, "X''");
    }
}

// Compact Self-Contained SHA-1 for Artwork Caching
static void sha1_transform(unsigned int state[5], const unsigned char buffer[64]) {
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    unsigned int w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((unsigned int)buffer[i*4] << 24) | ((unsigned int)buffer[i*4+1] << 16) |
               ((unsigned int)buffer[i*4+2] << 8) | ((unsigned int)buffer[i*4+3]);
    }
    for (int i = 16; i < 80; i++) {
        unsigned int val = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (val << 1) | (val >> 31);
    }
    for (int i = 0; i < 80; i++) {
        unsigned int f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        unsigned int temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void calc_sha1(const unsigned char *data, size_t len, char hexOut[41]) {
    unsigned int state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    unsigned long long bitLen = (unsigned long long)len * 8;
    size_t i = 0;
    while (i + 64 <= len) {
        sha1_transform(state, data + i);
        i += 64;
    }
    unsigned char pad[128] = {0};
    size_t rem = len - i;
    memcpy(pad, data + i, rem);
    pad[rem] = 0x80;
    size_t padLen = (rem < 56) ? 64 : 128;
    for (int k = 0; k < 8; k++) pad[padLen - 8 + k] = (unsigned char)((bitLen >> ((7 - k) * 8)) & 0xFF);
    for (size_t k = 0; k < padLen; k += 64) sha1_transform(state, pad + k);
    snprintf(hexOut, 41, "%08x%08x%08x%08x%08x", state[0], state[1], state[2], state[3], state[4]);
}

// Flat C GDI+ definitions for pre-rendered artwork caches
typedef void GpImage;
typedef void GpBitmap;
typedef void GpGraphics;

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef int (WINAPI *GdiplusStartup_t)(ULONG_PTR *, const GdiplusStartupInput *, void *);
typedef void (WINAPI *GdiplusShutdown_t)(ULONG_PTR);
typedef int (WINAPI *GdipCreateBitmapFromStream_t)(IStream *, GpBitmap **);
typedef int (WINAPI *GdipCreateBitmapFromScan0_t)(INT, INT, INT, INT, BYTE *, GpBitmap **);
typedef int (WINAPI *GdipGetImageGraphicsContext_t)(GpImage *, GpGraphics **);
typedef int (WINAPI *GdipSetInterpolationMode_t)(GpGraphics *, INT);
typedef int (WINAPI *GdipDrawImageRectRectI_t)(GpGraphics *, GpImage *, INT, INT, INT, INT, INT, INT, INT, INT, INT, void *, void *, void *);
typedef int (WINAPI *GdipSaveImageToStream_t)(GpImage *, IStream *, const CLSID *, const void *);
typedef int (WINAPI *GdipDisposeImage_t)(GpImage *);
typedef int (WINAPI *GdipDeleteGraphics_t)(GpGraphics *);
typedef int (WINAPI *GdipGetImageWidth_t)(GpImage *, UINT *);
typedef int (WINAPI *GdipGetImageHeight_t)(GpImage *, UINT *);
typedef int (WINAPI *GdipCreateFromHDC_t)(HDC, GpGraphics **);
typedef int (WINAPI *GdipDrawImageRectI_t)(GpGraphics *, GpImage *, INT, INT, INT, INT);
typedef int (WINAPI *GdipCreateBitmapFromFile_t)(const WCHAR *, GpBitmap **);

static HMODULE g_hGdi = NULL;
static ULONG_PTR g_gdiToken = 0;
static GdiplusShutdown_t pGdiShutdown = NULL;
static GdipCreateBitmapFromStream_t pCreateBmpFromStream = NULL;
static GdipCreateBitmapFromScan0_t pCreateBmp = NULL;
static GdipGetImageGraphicsContext_t pGetCtx = NULL;
static GdipSetInterpolationMode_t pSetInterp = NULL;
static GdipDrawImageRectRectI_t pDraw = NULL;
static GdipSaveImageToStream_t pSaveToStream = NULL;
static GdipDisposeImage_t pDispose = NULL;
static GdipDeleteGraphics_t pDelCtx = NULL;
static GdipGetImageWidth_t pGetWidth = NULL;
static GdipGetImageHeight_t pGetHeight = NULL;
static GdipCreateFromHDC_t pGetCtxHdc = NULL;
static GdipDrawImageRectI_t pDrawRectI = NULL;
static GdipCreateBitmapFromFile_t pCreateBmpFromFile = NULL;

static HWND g_hArtPreview = NULL;
static GpBitmap *g_pPreviewBitmap = NULL;
static wchar_t g_previewTitle[256] = {0};
static wchar_t g_previewArtist[256] = {0};
static wchar_t g_previewAlbum[256] = {0};
static wchar_t g_currentPreviewKey[MAX_PATH] = {0};

static const CLSID clsidJPEG = {0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

// Genuine Apple ImageIO Exif APP1 header template (ColorSpace=1 sRGB, PixelXDimension, PixelYDimension)
static const unsigned char s_appleExifPrefix[182] = {
    0xff, 0xe1, 0x0f, 0xfe, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x4d, 0x4d, 0x00, 0x2a, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x05, 0x01, 0x1a, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4a,
    0x01, 0x1b, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x52, 0x01, 0x28, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x02, 0x13, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x87, 0x69, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x5a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x90, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x04, 0x30, 0x32,
    0x32, 0x31, 0x91, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x04, 0x01, 0x02, 0x03, 0x00, 0xa0, 0x00,
    0x00, 0x07, 0x00, 0x00, 0x00, 0x04, 0x30, 0x31, 0x30, 0x30, 0xa0, 0x01, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xa0, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x02, 0x2c, 0xa0, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x2c, 0xa4, 0x06,
    0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
};

// 100% In-Memory Apple Music thumbnail generation & direct AFC upload
static void GenerateAndUploadThumbnails(AppleSession *pSess, const unsigned char *origJpgBytes, size_t origJpgLen, const char *sha1TokHex) {
    if (!pSess || !pCreateBmpFromStream || !pCreateBmp || !pSaveToStream || !pGetWidth || !pGetHeight || !origJpgBytes || origJpgLen < 4) return;

    HGLOBAL hMemIn = GlobalAlloc(GMEM_MOVEABLE, origJpgLen);
    if (!hMemIn) return;
    void *pMemIn = GlobalLock(hMemIn);
    if (!pMemIn) { GlobalFree(hMemIn); return; }
    memcpy(pMemIn, origJpgBytes, origJpgLen);
    GlobalUnlock(hMemIn);

    IStream *pInStream = NULL;
    if (CreateStreamOnHGlobal(hMemIn, TRUE, &pInStream) != S_OK || !pInStream) {
        GlobalFree(hMemIn);
        return;
    }

    GpBitmap *srcBmp = NULL;
    if (pCreateBmpFromStream(pInStream, &srcBmp) == 0 && srcBmp) {
        UINT origW = 0, origH = 0;
        pGetWidth((GpImage*)srcBmp, &origW);
        pGetHeight((GpImage*)srcBmp, &origH);

        static const int thumbSizes[] = {144, 150, 180, 390, 480, 531, 556};
        static int s_cacheDirsCreated = 0;
        if (!s_cacheDirsCreated) {
            for (int k = 0; k < sizeof(thumbSizes)/sizeof(thumbSizes[0]); k++) {
                char d[128];
                snprintf(d, sizeof(d), "/iTunes_Control/iTunes/Artwork/Caches/%dx%d", thumbSizes[k], thumbSizes[k]);
                Apple_AfcMakeDir(pSess, d);
            }
            s_cacheDirsCreated = 1;
        }

        for (int i = 0; i < sizeof(thumbSizes)/sizeof(thumbSizes[0]); i++) {
            int s = thumbSizes[i];
            GpBitmap *thumbBmp = NULL;
            if (pCreateBmp(s, s, 0, 0x26200A, NULL, &thumbBmp) == 0 && thumbBmp) {
                GpGraphics *g = NULL;
                pGetCtx((GpImage*)thumbBmp, &g);
                if (g) {
                    pSetInterp(g, 7);
                    pDraw(g, (GpImage*)srcBmp, 0, 0, s, s, 0, 0, (INT)origW, (INT)origH, 2, NULL, NULL, NULL);
                    pDelCtx(g);
                }

                IStream *pOutStream = NULL;
                if (CreateStreamOnHGlobal(NULL, TRUE, &pOutStream) == S_OK && pOutStream) {
                    if (pSaveToStream((GpImage*)thumbBmp, pOutStream, &clsidJPEG, NULL) == 0) {
                        HGLOBAL hMemOut = NULL;
                        if (GetHGlobalFromStream(pOutStream, &hMemOut) == S_OK && hMemOut) {
                            STATSTG stg;
                            if (IStream_Stat(pOutStream, &stg, STATFLAG_NONAME) == S_OK) {
                                size_t inSize = (size_t)stg.cbSize.QuadPart;
                                unsigned char *rawJpg = (unsigned char*)GlobalLock(hMemOut);
                                if (rawJpg && inSize > 4) {
                                    unsigned char app1[4096];
                                    memset(app1, 0, sizeof(app1));
                                    memcpy(app1, s_appleExifPrefix, sizeof(s_appleExifPrefix));
                                    app1[160] = 0; app1[161] = 0; app1[162] = (unsigned char)((s >> 8) & 0xFF); app1[163] = (unsigned char)(s & 0xFF);
                                    app1[172] = 0; app1[173] = 0; app1[174] = (unsigned char)((s >> 8) & 0xFF); app1[175] = (unsigned char)(s & 0xFF);

                                    size_t offset = 2;
                                    if (rawJpg[2] == 0xFF && rawJpg[3] == 0xE0) {
                                        size_t app0Len = ((size_t)rawJpg[4] << 8) | rawJpg[5];
                                        offset = 4 + app0Len;
                                    }

                                    size_t totalLen = 2 + 4096 + (inSize > offset ? (inSize - offset) : 0);
                                    unsigned char *fullThumb = (unsigned char*)malloc(totalLen);
                                    if (fullThumb) {
                                        fullThumb[0] = 0xFF; fullThumb[1] = 0xD8;
                                        memcpy(fullThumb + 2, app1, 4096);
                                        if (inSize > offset) {
                                            memcpy(fullThumb + 4098, rawJpg + offset, inSize - offset);
                                        }

                                        char dirSub[128], remThumb[256];
                                        snprintf(dirSub, sizeof(dirSub), "/iTunes_Control/iTunes/Artwork/Caches/%dx%d/%.2s", s, s, sha1TokHex);
                                        snprintf(remThumb, sizeof(remThumb), "%s/%s", dirSub, sha1TokHex + 2);

                                        Apple_AfcMakeDir(pSess, dirSub);

                                        unsigned long long hRemote = 0;
                                        if (Apple_AfcOpenFile(pSess, remThumb, AFC_MODE_RW, &hRemote) == 0 && hRemote) {
                                            Apple_AfcWriteFile(pSess, hRemote, fullThumb, (unsigned long long)totalLen);
                                            Apple_AfcCloseFile(pSess, hRemote);
                                        }
                                        free(fullThumb);
                                    }
                                    GlobalUnlock(hMemOut);
                                }
                            }
                        }
                    }
                    IStream_Release(pOutStream);
                }
                pDispose((GpImage*)thumbBmp);
            }
        }
        pDispose((GpImage*)srcBmp);
    }
    IStream_Release(pInStream);
}

static GpBitmap* LoadBitmapForTrack(const wchar_t *filePath) {
    if (!pCreateBmpFromStream) return NULL;

    // 1. Check for folder image in same directory
    wchar_t dirPath[MAX_PATH];
    wcsncpy(dirPath, filePath, MAX_PATH - 1);
    dirPath[MAX_PATH - 1] = L'\0';
    wchar_t *lastSlash = wcsrchr(dirPath, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
        const wchar_t *artNames[] = { L"cover.jpg", L"folder.jpg", L"front.jpg", L"album.jpg", L"cover.png", L"folder.png" };
        for (int i = 0; i < 6; ++i) {
            wchar_t imgPath[MAX_PATH];
            swprintf(imgPath, MAX_PATH, L"%ls\\%ls", dirPath, artNames[i]);
            DWORD attr = GetFileAttributesW(imgPath);
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                GpBitmap *bmp = NULL;
                if (pCreateBmpFromFile && pCreateBmpFromFile(imgPath, &bmp) == 0 && bmp) {
                    return bmp;
                }
            }
        }
    }

    // 2. Scan audio file for embedded APIC (ID3v2)
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    DWORD dwSize = GetFileSize(hFile, NULL);
    if (dwSize < 1024) { CloseHandle(hFile); return NULL; }

    DWORD toRead = dwSize > 4 * 1024 * 1024 ? 4 * 1024 * 1024 : dwSize;
    unsigned char *buf = (unsigned char*)malloc(toRead);
    if (!buf) { CloseHandle(hFile); return NULL; }

    DWORD dwRead = 0;
    GpBitmap *resBmp = NULL;
    if (ReadFile(hFile, buf, toRead, &dwRead, NULL) && dwRead > 64) {
        for (DWORD i = 0; i + 30 < dwRead; ++i) {
            if (memcmp(buf + i, "APIC", 4) == 0) {
                DWORD fSync = ((DWORD)buf[i+4] << 21) | ((DWORD)buf[i+5] << 14) | ((DWORD)buf[i+6] << 7) | (DWORD)buf[i+7];
                DWORD fBe = ((DWORD)buf[i+4] << 24) | ((DWORD)buf[i+5] << 16) | ((DWORD)buf[i+6] << 8) | (DWORD)buf[i+7];
                DWORD frameSz = (fSync > 0 && i + 10 + fSync <= dwRead) ? fSync : fBe;
                if (i + 10 + frameSz > dwRead) frameSz = dwRead - (i + 10);

                DWORD soi = 0, eoi = 0;
                int isPng = 0;
                for (DWORD s = i + 10; s + 8 < i + 10 + frameSz; ++s) {
                    if (buf[s] == 0xFF && buf[s+1] == 0xD8) {
                        soi = s;
                        break;
                    }
                    if (buf[s] == 0x89 && buf[s+1] == 0x50 && buf[s+2] == 0x4E && buf[s+3] == 0x47) {
                        soi = s;
                        isPng = 1;
                        break;
                    }
                }
                if (soi > 0) {
                    if (isPng) {
                        for (DWORD e = i + 10 + frameSz - 8; e >= soi + 8; --e) {
                            if (memcmp(buf + e, "IEND", 4) == 0) {
                                eoi = e + 8;
                                break;
                            }
                        }
                    } else {
                        for (DWORD e = i + 10 + frameSz - 2; e >= soi + 2; --e) {
                            if (buf[e] == 0xFF && buf[e+1] == 0xD9) {
                                eoi = e + 2;
                                break;
                            }
                        }
                    }
                    if (eoi > soi) {
                        DWORD len = eoi - soi;
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
                        if (hMem) {
                            void *pMem = GlobalLock(hMem);
                            if (pMem) {
                                memcpy(pMem, buf + soi, len);
                                GlobalUnlock(hMem);
                                IStream *pStream = NULL;
                                if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) == S_OK && pStream) {
                                    pCreateBmpFromStream(pStream, &resBmp);
                                    IStream_Release(pStream);
                                }
                            } else {
                                GlobalFree(hMem);
                            }
                        }
                    }
                }
                break;
            }
        }
    }
    free(buf);
    CloseHandle(hFile);
    return resBmp;
}

static int FetchDeviceArtworkToCache(const char *token, wchar_t *outCachePath, int maxLen) {
    if (!token || !token[0]) return 0;

    char cacheDir[MAX_PATH];
    snprintf(cacheDir, sizeof(cacheDir), "%s\\ArtworkCache", g_appDir);
    CreateDirectoryA(cacheDir, NULL);

    char localJpg[MAX_PATH];
    snprintf(localJpg, sizeof(localJpg), "%s\\%s.jpg", cacheDir, token);

    MultiByteToWideChar(CP_UTF8, 0, localJpg, -1, outCachePath, maxLen);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(outCachePath, GetFileExInfoStandard, &fad) && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (fad.nFileSizeLow > 0 || fad.nFileSizeHigh > 0) return 1;
        DeleteFileW(outCachePath);
    }

    if (!g_deviceConnected || g_currentDeviceId == 0) {
        return 0;
    }

    char sha1Hex[41] = {0};
    calc_sha1((const unsigned char*)token, strlen(token), sha1Hex);

    char remotePath[256];
    snprintf(remotePath, sizeof(remotePath), "/iTunes_Control/iTunes/Artwork/Originals/%.2s/%s", sha1Hex, sha1Hex + 2);

    AppleSession sess;
    if (!Apple_StartSession(&sess, g_currentDeviceId, g_currentDeviceUdid)) return 0;

    int success = 0;
    unsigned long long hRemote = 0;
    if (Apple_AfcOpenFile(&sess, remotePath, AFC_MODE_RDONLY, &hRemote) == 0 && hRemote) {
        FILE *fOut = fopen(localJpg, "wb");
        if (fOut) {
            char buf[65536];
            unsigned long long bytesRead = 0;
            unsigned long long totalRead = 0;
            while (Apple_AfcReadFile(&sess, hRemote, buf, sizeof(buf), &bytesRead) == 0 && bytesRead > 0) {
                fwrite(buf, 1, (size_t)bytesRead, fOut);
                totalRead += bytesRead;
                bytesRead = 0;
            }
            fclose(fOut);
            if (totalRead > 0) {
                success = 1;
            } else {
                DeleteFileA(localJpg);
            }
        }
        Apple_AfcCloseFile(&sess, hRemote);
    }
    Apple_StopSession(&sess);

    return success;
}

static LRESULT CALLBACK ArtPreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        if (rc.right <= rc.left || rc.bottom <= rc.top) {
            EndPaint(hWnd, &ps);
            return 0;
        }

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // Sidebar background
        HBRUSH bgBrush = CreateSolidBrush(COLOR_SIDEBAR_BG);
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        // Modern White Card Container with rounded corners
        RECT rcCard = { 2, 2, rc.right - 2, rc.bottom - 2 };
        HPEN cardBorderPen = CreatePen(PS_SOLID, 1, RGB(214, 218, 226));
        HGDIOBJ oldPen = SelectObject(memDC, cardBorderPen);
        HBRUSH cardBrush = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ oldBrush = SelectObject(memDC, cardBrush);
        RoundRect(memDC, rcCard.left, rcCard.top, rcCard.right, rcCard.bottom, 10, 10);
        SelectObject(memDC, oldBrush);
        DeleteObject(cardBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(cardBorderPen);

        // Artwork Display Area
        int cardW = rcCard.right - rcCard.left;
        int cardH = rcCard.bottom - rcCard.top;
        int imgSize = cardW - 12;
        if (imgSize > cardH - 52) imgSize = cardH - 52;
        if (imgSize < 40) imgSize = 40;

        int imgX = rcCard.left + (cardW - imgSize) / 2;
        int imgY = rcCard.top + 6;
        RECT rcImg = { imgX, imgY, imgX + imgSize, imgY + imgSize };

        if (g_pPreviewBitmap && pGetCtxHdc && pDrawRectI && pDelCtx && pSetInterp) {
            GpGraphics *graphics = NULL;
            if (pGetCtxHdc(memDC, &graphics) == 0 && graphics) {
                pSetInterp(graphics, 7); // HighQualityBicubic
                UINT origW = 0, origH = 0;
                if (pGetWidth) pGetWidth((GpImage*)g_pPreviewBitmap, &origW);
                if (pGetHeight) pGetHeight((GpImage*)g_pPreviewBitmap, &origH);

                int drawW = imgSize;
                int drawH = imgSize;
                int drawX = rcImg.left;
                int drawY = rcImg.top;
                if (origW > 0 && origH > 0) {
                    if ((long long)origW * drawH > (long long)origH * drawW) {
                        int scaledH = (int)(((long long)origH * drawW) / origW);
                        drawY += (drawH - scaledH) / 2;
                        drawH = scaledH;
                    } else {
                        int scaledW = (int)(((long long)origW * drawH) / origH);
                        drawX += (drawW - scaledW) / 2;
                        drawW = scaledW;
                    }
                }
                pDrawRectI(graphics, (GpImage*)g_pPreviewBitmap, drawX, drawY, drawW, drawH);
                pDelCtx(graphics);
            }

            // Subtle border around artwork
            HPEN artBorder = CreatePen(PS_SOLID, 1, RGB(220, 224, 230));
            HGDIOBJ pOld = SelectObject(memDC, artBorder);
            HGDIOBJ bOld = SelectObject(memDC, GetStockObject(NULL_BRUSH));
            Rectangle(memDC, rcImg.left, rcImg.top, rcImg.right, rcImg.bottom);

            // Specular sheen highlight along top edge of artwork
            HPEN sheenPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            SelectObject(memDC, sheenPen);
            MoveToEx(memDC, rcImg.left + 2, rcImg.top + 1, NULL);
            LineTo(memDC, rcImg.right - 2, rcImg.top + 1);
            SelectObject(memDC, bOld);
            SelectObject(memDC, pOld);
            DeleteObject(artBorder);
            DeleteObject(sheenPen);
        } else {
            // Vinyl Record Placeholder
            int cx = (rcImg.left + rcImg.right) / 2;
            int cy = (rcImg.top + rcImg.bottom) / 2 - 10;
            int r = (imgSize / 2) - 18;
            if (r > 14) {
                // Vinyl outer body
                HPEN dPen = CreatePen(PS_SOLID, 1, RGB(28, 32, 40));
                HGDIOBJ pP = SelectObject(memDC, dPen);
                HBRUSH dBrush = CreateSolidBrush(RGB(34, 38, 48));
                HGDIOBJ pB = SelectObject(memDC, dBrush);
                Ellipse(memDC, cx - r, cy - r, cx + r, cy + r);

                // Vinyl sound grooves
                HPEN gPen = CreatePen(PS_SOLID, 1, RGB(52, 58, 70));
                SelectObject(memDC, gPen);
                SelectObject(memDC, GetStockObject(NULL_BRUSH));
                int rG1 = r * 5 / 6;
                Ellipse(memDC, cx - rG1, cy - rG1, cx + rG1, cy + rG1);
                int rG2 = r * 4 / 6;
                Ellipse(memDC, cx - rG2, cy - rG2, cx + rG2, cy + rG2);
                int rG3 = r * 3 / 6;
                Ellipse(memDC, cx - rG3, cy - rG3, cx + rG3, cy + rG3);

                // Center vinyl label (Apple Music Accent)
                int rLabel = r / 3;
                HPEN labelPen = CreatePen(PS_SOLID, 1, RGB(220, 30, 68));
                SelectObject(memDC, labelPen);
                HBRUSH labelBrush = CreateSolidBrush(RGB(255, 45, 85));
                SelectObject(memDC, labelBrush);
                Ellipse(memDC, cx - rLabel, cy - rLabel, cx + rLabel, cy + rLabel);

                // Center spindle hole
                int rHole = 3;
                HBRUSH holeBrush = CreateSolidBrush(RGB(255, 255, 255));
                SelectObject(memDC, holeBrush);
                HPEN holePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                SelectObject(memDC, holePen);
                Ellipse(memDC, cx - rHole, cy - rHole, cx + rHole, cy + rHole);

                SelectObject(memDC, pB);
                SelectObject(memDC, pP);
                DeleteObject(holePen);
                DeleteObject(holeBrush);
                DeleteObject(labelBrush);
                DeleteObject(labelPen);
                DeleteObject(gPen);
                DeleteObject(dBrush);
                DeleteObject(dPen);
            }

            HFONT hPhFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HGDIOBJ oldF = SelectObject(memDC, hPhFont);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(145, 152, 164));
            RECT rcNoArt = { rcImg.left + 4, cy + r + 2, rcImg.right - 4, rcImg.bottom - 2 };
            const wchar_t *phText = g_previewTitle[0] ? L"No Artwork Available" : L"Select a Track";
            DrawTextW(memDC, phText, -1, &rcNoArt, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(memDC, oldF);
            DeleteObject(hPhFont);
        }

        // Hairline divider
        HPEN divPen = CreatePen(PS_SOLID, 1, RGB(236, 240, 246));
        HGDIOBJ pDivOld = SelectObject(memDC, divPen);
        MoveToEx(memDC, rcCard.left + 10, rcImg.bottom + 6, NULL);
        LineTo(memDC, rcCard.right - 10, rcImg.bottom + 6);
        SelectObject(memDC, pDivOld);
        DeleteObject(divPen);

        // Track Title & Artist typography
        SetBkMode(memDC, TRANSPARENT);
        HFONT hTitleF = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(memDC, hTitleF);
        SetTextColor(memDC, RGB(24, 28, 38));
        RECT rcT = { rcCard.left + 4, rcImg.bottom + 9, rcCard.right - 4, rcImg.bottom + 25 };
        const wchar_t *dispTitle = g_previewTitle[0] ? g_previewTitle : L"—";
        DrawTextW(memDC, dispTitle, -1, &rcT, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        HFONT hArtF = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(memDC, hArtF);
        SetTextColor(memDC, COLOR_TEXT_MUTED);
        RECT rcA = { rcCard.left + 4, rcImg.bottom + 26, rcCard.right - 4, rcImg.bottom + 42 };
        const wchar_t *dispArtist = g_previewArtist[0] ? g_previewArtist : L"";
        DrawTextW(memDC, dispArtist, -1, &rcA, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        SelectObject(memDC, oldF);
        DeleteObject(hTitleF);
        DeleteObject(hArtF);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void UpdateSelectedArtworkPreview() {
    if (!g_hArtPreview) return;

    int sel = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
    if (sel < 0) {
        g_previewTitle[0] = L'\0';
        g_previewArtist[0] = L'\0';
        g_previewAlbum[0] = L'\0';
        g_currentPreviewKey[0] = L'\0';
        if (g_pPreviewBitmap && pDispose) {
            pDispose((GpImage*)g_pPreviewBitmap);
            g_pPreviewBitmap = NULL;
        }
        InvalidateRect(g_hArtPreview, NULL, FALSE);
        return;
    }

    if (g_activeView == VIEW_TRANSFER_QUEUE) {
        if (sel >= g_queueCount) return;
        QueueTrack *t = &g_queue[sel];

        if (wcscmp(g_currentPreviewKey, t->filePath) == 0 && g_pPreviewBitmap) {
            return;
        }
        wcsncpy(g_currentPreviewKey, t->filePath, MAX_PATH - 1);
        wcsncpy(g_previewTitle, t->title, 255);
        wcsncpy(g_previewArtist, t->artist, 255);
        wcsncpy(g_previewAlbum, t->album, 255);

        if (g_pPreviewBitmap && pDispose) {
            pDispose((GpImage*)g_pPreviewBitmap);
            g_pPreviewBitmap = NULL;
        }
        g_pPreviewBitmap = LoadBitmapForTrack(t->filePath);
        InvalidateRect(g_hArtPreview, NULL, FALSE);
    } else {
        int idx = (int)LvGetParam(g_hListView, sel);
        if (idx < 0 || idx >= g_deviceTrackCount) return;
        DeviceTrack *dt = &g_deviceTracks[idx];

        wchar_t wTitle[256] = {0}, wArtist[256] = {0}, wAlbum[256] = {0};
        MultiByteToWideChar(CP_UTF8, 0, dt->title, -1, wTitle, 255);
        MultiByteToWideChar(CP_UTF8, 0, dt->artist, -1, wArtist, 255);
        MultiByteToWideChar(CP_UTF8, 0, dt->album, -1, wAlbum, 255);

        wchar_t tokenKey[MAX_PATH];
        swprintf(tokenKey, MAX_PATH, L"DEV_%lld_%hs", dt->item_pid, dt->artworkToken);

        if (wcscmp(g_currentPreviewKey, tokenKey) == 0 && g_pPreviewBitmap) {
            return;
        }
        wcsncpy(g_currentPreviewKey, tokenKey, MAX_PATH - 1);
        wcsncpy(g_previewTitle, wTitle, 255);
        wcsncpy(g_previewArtist, wArtist, 255);
        wcsncpy(g_previewAlbum, wAlbum, 255);

        if (g_pPreviewBitmap && pDispose) {
            pDispose((GpImage*)g_pPreviewBitmap);
            g_pPreviewBitmap = NULL;
        }

        if (dt->artworkToken[0]) {
            wchar_t localArtPath[MAX_PATH] = {0};
            if (FetchDeviceArtworkToCache(dt->artworkToken, localArtPath, MAX_PATH)) {
                if (pCreateBmpFromFile) {
                    pCreateBmpFromFile(localArtPath, &g_pPreviewBitmap);
                }
            }
        }
        InvalidateRect(g_hArtPreview, NULL, FALSE);
    }
}

// True Direct Sync Engine (Strict iTunes MediaLibrary Spec)
DWORD WINAPI SyncQueueWorker(LPVOID param) {
    if (!g_deviceConnected || !g_currentDeviceId) {
        MessageBoxW(g_hMainWnd, L"Connect your iPhone via USB to continue.", L"iPhone Not Connected", MB_ICONWARNING);
        return 0;
    }

    int toSyncCount = 0;
    for (int i = 0; i < g_queueCount; ++i) {
        if (!g_queue[i].isDuplicate && g_queue[i].status != STATUS_SYNCED) {
            toSyncCount++;
        }
    }

    if (toSyncCount == 0) {
        MessageBoxW(g_hMainWnd, L"All queued tracks are already on your iPhone.", L"Nothing to Sync", MB_ICONINFORMATION);
        return 0;
    }

    g_isSyncing = 1;
    EnableWindow(g_hBtnPushAlbum, FALSE);
    EnableWindow(g_hBtnAddTracks, FALSE);
    EnableWindow(g_hBtnSyncQueue, FALSE);

    AppleSession sess;
    if (!Apple_StartSession(&sess, g_currentDeviceId, g_currentDeviceUdid)) {
        g_isSyncing = 0;
        EnableWindow(g_hBtnPushAlbum, TRUE);
        EnableWindow(g_hBtnAddTracks, TRUE);
        EnableWindow(g_hBtnSyncQueue, TRUE);
        MessageBoxW(g_hMainWnd, L"Failed to connect to device via native AFC.", L"Sync Error", MB_ICONERROR);
        return 0;
    }

    // 1. Notify iOS that iTunes sync will start (releases DB locks & suspends background indexer)
    Apple_PostNotification(&sess, "com.apple.itunes-mobdev.syncWillStart");
    Sleep(300);

    sqlite3 *db = NULL;
    if (pSqlOpen(g_localDbPath, &db) != 0 || !db) {
        Apple_StopSession(&sess);
        g_isSyncing = 0;
        EnableWindow(g_hBtnPushAlbum, TRUE);
        EnableWindow(g_hBtnAddTracks, TRUE);
        EnableWindow(g_hBtnSyncQueue, TRUE);
        return 0;
    }

    pSqlExec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    pSqlExec(db, "PRAGMA cache_size = -64000;", NULL, NULL, NULL);
    pSqlExec(db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);
    pSqlExec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    long long maxItemPid = 16777217;
    long long maxArtistPid = 33554433;
    long long maxAlbumPid = 67108865;
    long long maxAlbArtPid = 117440513;
    long long maxRev = 0;
    long long maxArtTokVal = 1000;

    sqlite3_stmt *sMax = NULL;
    if (pSqlPrepare(db, "SELECT COALESCE(MAX(p), 16777217) FROM (SELECT MAX(item_pid) AS p FROM item UNION SELECT MAX(entity_pid) AS p FROM entity_revision WHERE class = 0);", -1, &sMax, NULL) == 0 && sMax) {
        if (pSqlStep(sMax) == 100) maxItemPid = pSqlColInt64(sMax, 0);
        pSqlFinalize(sMax);
    }
    if (pSqlPrepare(db, "SELECT COALESCE(MAX(p), 33554433) FROM (SELECT MAX(item_artist_pid) AS p FROM item_artist UNION SELECT MAX(entity_pid) AS p FROM entity_revision WHERE class = 2);", -1, &sMax, NULL) == 0 && sMax) {
        if (pSqlStep(sMax) == 100) maxArtistPid = pSqlColInt64(sMax, 0);
        pSqlFinalize(sMax);
    }
    if (pSqlPrepare(db, "SELECT COALESCE(MAX(p), 67108865) FROM (SELECT MAX(album_pid) AS p FROM album UNION SELECT MAX(entity_pid) AS p FROM entity_revision WHERE class = 4);", -1, &sMax, NULL) == 0 && sMax) {
        if (pSqlStep(sMax) == 100) maxAlbumPid = pSqlColInt64(sMax, 0);
        pSqlFinalize(sMax);
    }
    if (pSqlPrepare(db, "SELECT COALESCE(MAX(p), 117440513) FROM (SELECT MAX(album_artist_pid) AS p FROM album_artist UNION SELECT MAX(entity_pid) AS p FROM entity_revision WHERE class = 7);", -1, &sMax, NULL) == 0 && sMax) {
        if (pSqlStep(sMax) == 100) maxAlbArtPid = pSqlColInt64(sMax, 0);
        pSqlFinalize(sMax);
    }
    if (pSqlPrepare(db, "SELECT COALESCE(MAX(revision), 0) FROM entity_revision;", -1, &sMax, NULL) == 0 && sMax) {
        if (pSqlStep(sMax) == 100) maxRev = pSqlColInt64(sMax, 0);
        pSqlFinalize(sMax);
    }
    if (pSqlPrepare(db, "SELECT COALESCE(MAX(CAST(artwork_token AS INTEGER)), 1000) FROM artwork;", -1, &sMax, NULL) == 0 && sMax) {
        if (pSqlStep(sMax) == 100) maxArtTokVal = pSqlColInt64(sMax, 0);
        pSqlFinalize(sMax);
    }
    long long baseSyncRev = maxRev;

    long long createdAlbumPids[256]; int numCreatedAlbums = 0;
    long long createdAlbArtPids[256]; int numCreatedAlbArts = 0;
    long long createdItemArtPids[256]; int numCreatedItemArts = 0;

    // Fast in-memory caches to eliminate redundant SQLite queries
    #define MAX_CACHED_ITEMS 256
    typedef struct {
        char name[256];
        long long itemArtistPid;
        long long artOrder;
        int artSection;
    } CachedArtist;

    typedef struct {
        char name[256];
        long long albArtPid;
        long long albArtOrder;
        int albArtSection;
    } CachedAlbArt;

    typedef struct {
        char name[256];
        long long albArtPid;
        long long albumPid;
        long long albOrder;
        int albSection;
    } CachedAlbum;

    CachedArtist artCache[MAX_CACHED_ITEMS]; int artCacheCount = 0;
    CachedAlbArt albArtCache[MAX_CACHED_ITEMS]; int albArtCacheCount = 0;
    CachedAlbum albCache[MAX_CACHED_ITEMS]; int albCacheCount = 0;

    long long artAlbumPids[MAX_CACHED_ITEMS];
    char artTokens[MAX_CACHED_ITEMS][32];
    int artAlbumCount = 0;

    char *audioChunk = (char*)malloc(256 * 1024);
    unsigned long long createdFDirs = 0;

    int syncedCount = 0;
    srand((unsigned int)time(NULL));

    for (int i = 0; i < g_queueCount; ++i) {
        QueueTrack *t = &g_queue[i];
        if (t->isDuplicate || t->status == STATUS_SYNCED) continue;

        t->status = STATUS_SYNCING;
        syncedCount++;

        swprintf(g_statusText, 256, L"⚡ Syncing (%d of %d): %ls - %ls", syncedCount, toSyncCount, t->artist, t->title);
        RECT rcClient;
        GetClientRect(g_hMainWnd, &rcClient);
        RECT rcBottom = {0, rcClient.bottom - 42, rcClient.right, rcClient.bottom};
        InvalidateRect(g_hMainWnd, &rcBottom, FALSE);
        UpdateWindow(g_hMainWnd);
        if (g_activeView == VIEW_TRANSFER_QUEUE) {
            ListView_SetItemText(g_hListView, i, 1, L"⚡ Syncing...");
        }

        int fIdx = rand() % 50;
        char randChars[5] = {0};
        for (int k = 0; k < 4; ++k) randChars[k] = 'A' + (rand() % 26);

        char remoteDir[128];
        snprintf(remoteDir, sizeof(remoteDir), "/iTunes_Control/Music/F%02d", fIdx);
        char remotePath[256];
        snprintf(remotePath, sizeof(remotePath), "%s/%s.mp3", remoteDir, randChars);

        // Upload raw audio file with 256 KB buffer and sequential read
        if (!(createdFDirs & (1ULL << fIdx))) {
            Apple_AfcMakeDir(&sess, remoteDir);
            createdFDirs |= (1ULL << fIdx);
        }

        HANDLE hLocal = CreateFileW(t->filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hLocal != INVALID_HANDLE_VALUE) {
            unsigned long long hRemote = 0;
            if (Apple_AfcOpenFile(&sess, remotePath, AFC_MODE_RW, &hRemote) == 0 && hRemote) {
                if (audioChunk) {
                    DWORD rBytes = 0;
                    while (ReadFile(hLocal, audioChunk, 256 * 1024, &rBytes, NULL) && rBytes > 0) {
                        Apple_AfcWriteFile(&sess, hRemote, audioChunk, (unsigned long long)rBytes);
                    }
                }
                Apple_AfcCloseFile(&sess, hRemote);
            }
            CloseHandle(hLocal);
        }

        char artistUtf8[256] = {0}, titleUtf8[256] = {0}, albumUtf8[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, t->artist, -1, artistUtf8, sizeof(artistUtf8), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, t->title, -1, titleUtf8, sizeof(titleUtf8), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, t->album, -1, albumUtf8, sizeof(albumUtf8), NULL, NULL);

        // Escape single quotes for SQL safety
        char escArtist[512] = {0}, escTitle[512] = {0}, escAlbum[512] = {0};
        int o = 0; for (int k = 0; artistUtf8[k] && o < 500; ++k) { if (artistUtf8[k] == '\'') escArtist[o++] = '\''; escArtist[o++] = artistUtf8[k]; }
        o = 0; for (int k = 0; titleUtf8[k] && o < 500; ++k) { if (titleUtf8[k] == '\'') escTitle[o++] = '\''; escTitle[o++] = titleUtf8[k]; }
        o = 0; for (int k = 0; albumUtf8[k] && o < 500; ++k) { if (albumUtf8[k] == '\'') escAlbum[o++] = '\''; escAlbum[o++] = albumUtf8[k]; }

        char sortArtist[512] = {0};
        CapitalizeSort(escArtist, sortArtist, sizeof(sortArtist));

        // Resolve Album Artist and Compilation status
        wchar_t albArtW[256] = {0};
        int isComp = 0;
        ResolveAlbumArtistForTrack(t, albArtW, 256, &isComp);

        char albArtUtf8[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, albArtW, -1, albArtUtf8, sizeof(albArtUtf8), NULL, NULL);

        char escAlbArt[512] = {0};
        o = 0; for (int k = 0; albArtUtf8[k] && o < 500; ++k) { if (albArtUtf8[k] == '\'') escAlbArt[o++] = '\''; escAlbArt[o++] = albArtUtf8[k]; }

        char sortAlbArt[512] = {0};
        CapitalizeSort(escAlbArt, sortAlbArt, sizeof(sortAlbArt));

        long long newItemPid = ++maxItemPid;

        // 1. Item Artist (item_artist - track performer, cached for speed)
        long long currentArtistPid = 0;
        long long artOrder = 0;
        int artSection = 26;

        for (int a = 0; a < artCacheCount; a++) {
            if (strcmp(artCache[a].name, escArtist) == 0) {
                currentArtistPid = artCache[a].itemArtistPid;
                artOrder = artCache[a].artOrder;
                artSection = artCache[a].artSection;
                break;
            }
        }

        if (currentArtistPid == 0) {
            char cA = (char)toupper((unsigned char)sortArtist[0]);
            if (cA >= 'A' && cA <= 'Z') artSection = cA - 'A';

            sqlite3_stmt *sArt = NULL;
            char sqlArtFind[512];
            snprintf(sqlArtFind, sizeof(sqlArtFind), "SELECT item_artist_pid FROM item_artist WHERE item_artist = '%s' LIMIT 1;", escArtist);
            if (pSqlPrepare(db, sqlArtFind, -1, &sArt, NULL) == 0 && sArt) {
                if (pSqlStep(sArt) == 100) currentArtistPid = pSqlColInt64(sArt, 0);
                pSqlFinalize(sArt);
            }
            if (currentArtistPid == 0) {
                currentArtistPid = ++maxArtistPid;
                if (numCreatedItemArts < 256) createdItemArtPids[numCreatedItemArts++] = currentArtistPid;
                char gkArt[128];
                EncodeGroupingKey(escArtist, gkArt, sizeof(gkArt));
                char sqlArtIns[1024];
                snprintf(sqlArtIns, sizeof(sqlArtIns), 
                    "INSERT OR IGNORE INTO item_artist (item_artist_pid, item_artist, sort_item_artist, representative_item_pid, grouping_key) VALUES (%lld, '%s', '%s', %lld, %s);",
                    currentArtistPid, escArtist, sortArtist, newItemPid, gkArt);
                pSqlExec(db, sqlArtIns, NULL, NULL, NULL);
            }
            artOrder = GetSectionOrder(sortArtist[0], currentArtistPid);

            if (artCacheCount < MAX_CACHED_ITEMS) {
                strncpy(artCache[artCacheCount].name, escArtist, 255);
                artCache[artCacheCount].name[255] = '\0';
                artCache[artCacheCount].itemArtistPid = currentArtistPid;
                artCache[artCacheCount].artOrder = artOrder;
                artCache[artCacheCount].artSection = artSection;
                artCacheCount++;
            }
        }

        // 2. Album Artist (album_artist - unified album performer, cached for speed)
        long long currentAlbArtPid = 0;
        long long albArtOrder = 0;
        int albArtSection = 26;

        for (int a = 0; a < albArtCacheCount; a++) {
            if (strcmp(albArtCache[a].name, escAlbArt) == 0) {
                currentAlbArtPid = albArtCache[a].albArtPid;
                albArtOrder = albArtCache[a].albArtOrder;
                albArtSection = albArtCache[a].albArtSection;
                break;
            }
        }

        if (currentAlbArtPid == 0) {
            char cAA = (char)toupper((unsigned char)sortAlbArt[0]);
            if (cAA >= 'A' && cAA <= 'Z') albArtSection = cAA - 'A';

            sqlite3_stmt *sAlbArt = NULL;
            char sqlAlbArtFind[512];
            snprintf(sqlAlbArtFind, sizeof(sqlAlbArtFind), "SELECT album_artist_pid FROM album_artist WHERE album_artist = '%s' LIMIT 1;", escAlbArt);
            if (pSqlPrepare(db, sqlAlbArtFind, -1, &sAlbArt, NULL) == 0 && sAlbArt) {
                if (pSqlStep(sAlbArt) == 100) currentAlbArtPid = pSqlColInt64(sAlbArt, 0);
                pSqlFinalize(sAlbArt);
            }
            if (currentAlbArtPid == 0) {
                currentAlbArtPid = ++maxAlbArtPid;
                if (numCreatedAlbArts < 256) createdAlbArtPids[numCreatedAlbArts++] = currentAlbArtPid;
                char gkAlbArt[128];
                EncodeGroupingKey(escAlbArt, gkAlbArt, sizeof(gkAlbArt));
                albArtOrder = GetSectionOrder(sortAlbArt[0], currentAlbArtPid);
                long long syncIdArt = ((long long)rand() << 32) | rand();
                char sqlAlbArtIns[1024];
                snprintf(sqlAlbArtIns, sizeof(sqlAlbArtIns),
                    "INSERT OR IGNORE INTO album_artist (album_artist_pid, album_artist, sort_album_artist, representative_item_pid, name_order, sort_order, sort_order_section, sync_id, grouping_key) VALUES (%lld, '%s', '%s', %lld, %lld, %lld, %d, %lld, %s);",
                    currentAlbArtPid, escAlbArt, sortAlbArt, newItemPid, albArtOrder, albArtOrder, albArtSection, syncIdArt, gkAlbArt);
                pSqlExec(db, sqlAlbArtIns, NULL, NULL, NULL);
            } else {
                albArtOrder = GetSectionOrder(sortAlbArt[0], currentAlbArtPid);
            }

            if (albArtCacheCount < MAX_CACHED_ITEMS) {
                strncpy(albArtCache[albArtCacheCount].name, escAlbArt, 255);
                albArtCache[albArtCacheCount].name[255] = '\0';
                albArtCache[albArtCacheCount].albArtPid = currentAlbArtPid;
                albArtCache[albArtCacheCount].albArtOrder = albArtOrder;
                albArtCache[albArtCacheCount].albArtSection = albArtSection;
                albArtCacheCount++;
            }
        }

        // 3. Album (album - single unified row per album, cached for speed)
        long long currentAlbumPid = 0;
        long long albOrder = 0;
        int albSection = 26;

        for (int a = 0; a < albCacheCount; a++) {
            if (strcmp(albCache[a].name, escAlbum) == 0 && (isComp ? 1 : (albCache[a].albArtPid == currentAlbArtPid))) {
                currentAlbumPid = albCache[a].albumPid;
                albOrder = albCache[a].albOrder;
                albSection = albCache[a].albSection;
                break;
            }
        }

        if (currentAlbumPid == 0) {
            char sortAlbum[512] = {0};
            CapitalizeSort(escAlbum, sortAlbum, sizeof(sortAlbum));
            char cAl = (char)toupper((unsigned char)sortAlbum[0]);
            if (cAl >= 'A' && cAl <= 'Z') albSection = cAl - 'A';

            sqlite3_stmt *sAlb = NULL;
            char sqlAlbFind[512];
            if (isComp) {
                snprintf(sqlAlbFind, sizeof(sqlAlbFind), "SELECT album_pid FROM album WHERE album = '%s' AND (all_compilations = 1 OR album_artist_pid = %lld) LIMIT 1;", escAlbum, currentAlbArtPid);
            } else {
                snprintf(sqlAlbFind, sizeof(sqlAlbFind), "SELECT album_pid FROM album WHERE album = '%s' AND album_artist_pid = %lld LIMIT 1;", escAlbum, currentAlbArtPid);
            }
            if (pSqlPrepare(db, sqlAlbFind, -1, &sAlb, NULL) == 0 && sAlb) {
                if (pSqlStep(sAlb) == 100) currentAlbumPid = pSqlColInt64(sAlb, 0);
                pSqlFinalize(sAlb);
            }
            if (currentAlbumPid == 0) {
                currentAlbumPid = ++maxAlbumPid;
                if (numCreatedAlbums < 256) createdAlbumPids[numCreatedAlbums++] = currentAlbumPid;
                albOrder = GetSectionOrder(sortAlbum[0], currentAlbumPid);
                char gkAlb[128];
                EncodeGroupingKey(escAlbum, gkAlb, sizeof(gkAlb));
                long long syncIdAlb = ((long long)rand() << 32) | rand();
                char sqlAlbIns[1024];
                snprintf(sqlAlbIns, sizeof(sqlAlbIns), 
                    "INSERT OR IGNORE INTO album (album_pid, album, sort_album, album_artist_pid, representative_item_pid, album_year, album_type, keep_local_status, all_compilations, sync_id, feed_url, grouping_key) VALUES (%lld, '%s', '%s', %lld, %lld, %d, 1, 2, %d, %lld, '', %s);",
                    currentAlbumPid, escAlbum, sortAlbum, currentAlbArtPid, newItemPid, t->yearVal, isComp ? 1 : 0, syncIdAlb, gkAlb);
                pSqlExec(db, sqlAlbIns, NULL, NULL, NULL);
            } else {
                albOrder = GetSectionOrder(sortAlbum[0], currentAlbumPid);
            }

            if (albCacheCount < MAX_CACHED_ITEMS) {
                strncpy(albCache[albCacheCount].name, escAlbum, 255);
                albCache[albCacheCount].name[255] = '\0';
                albCache[albCacheCount].albArtPid = currentAlbArtPid;
                albCache[albCacheCount].albumPid = currentAlbumPid;
                albCache[albCacheCount].albOrder = albOrder;
                albCache[albCacheCount].albSection = albSection;
                albCacheCount++;
            }
        }

        char sortTitle[512] = {0};
        CapitalizeSort(escTitle, sortTitle, sizeof(sortTitle));
        int titleSection = 26;
        char cT = (char)toupper((unsigned char)sortTitle[0]);
        if (cT >= 'A' && cT <= 'Z') titleSection = cT - 'A';
        long long titleOrder = GetSectionOrder(sortTitle[0], newItemPid);

        long nowMacEpoch = (long)(time(NULL) - 978307200);
        int baseLocationId = 3840 + fIdx;
        char qBase[128];
        snprintf(qBase, sizeof(qBase), "SELECT base_location_id FROM base_location WHERE path LIKE '%%/F%02d' LIMIT 1;", fIdx);
        sqlite3_stmt *sBase = NULL;
        if (pSqlPrepare(db, qBase, -1, &sBase, NULL) == 0 && sBase) {
            if (pSqlStep(sBase) == 100) baseLocationId = (int)pSqlColInt64(sBase, 0);
            pSqlFinalize(sBase);
        }
        long durationMs = t->durationMs > 0 ? t->durationMs : 210000;
        long long durationSamples = (long long)(durationMs * 44.1);

        // 4-10. Consolidated Track Metadata Batch (Atomic single-pass execution)
        char locStr[32];
        snprintf(locStr, sizeof(locStr), "%s.mp3", randChars);
        long long syncIdItem = ((long long)rand() << 32) | rand();

        char sqlTrackBatch[6144];
        snprintf(sqlTrackBatch, sizeof(sqlTrackBatch),
            // 4. Item
            "INSERT INTO item ("
            "item_pid, media_type, item_artist_pid, item_artist_order, item_artist_order_section, "
            "album_pid, album_order, album_order_section, album_artist_pid, album_artist_order, album_artist_order_section, "
            "title_order, title_order_section, disc_number, track_number, episode_sort_id, base_location_id, "
            "in_my_library, keep_local, keep_local_status, date_added, date_downloaded"
            ") VALUES ("
            "%lld, 8, %lld, %lld, %d, "
            "%lld, %lld, %d, %lld, %lld, %d, "
            "%lld, %d, %d, %d, %d, %d, "
            "1, 1, 2, %ld, %ld.0"
            ");"
            // 5. Item Extra
            "INSERT INTO item_extra ("
            "item_pid, title, sort_title, disc_count, track_count, total_time_ms, year, location, file_size, location_kind_id, media_kind, date_modified"
            ") VALUES ("
            "%lld, '%s', '%s', 1, %d, %ld.0, %d, '%s', %ld, 42, 1, %ld"
            ");"
            // 5b. Update item_state
            "UPDATE item_state SET is_valid_content_type = 1, is_placeholder = 0, cloud_id = 0, sync_id = 0, radio_station_id = 0, keep_local_managed_status_reason = 0, is_library_added = 1, keep_local_enable_state = 1, keep_local_managed_status = 2, has_non_purgeable_asset = 1, media_type = 8, title = '%s', location = '%s' WHERE item_pid = %lld;"
            // 6. Item Playback
            "INSERT OR REPLACE INTO item_playback ("
            "item_pid, audio_format, bit_rate, codec_type, codec_subtype, duration, gapless_heuristic_info, "
            "gapless_encoding_delay, gapless_encoding_drain, gapless_last_frame_resynch, has_video, sample_rate, start_time_ms, stop_time_ms"
            ") VALUES ("
            "%lld, 301, 320, 0, 0, %lld, 1, 528, 2287, %lld, 0, 44100.0, 0.0, 0.0"
            ");"
            // 7. Item Store
            "INSERT OR REPLACE INTO item_store (item_pid, sync_id, sync_in_my_library, cloud_status, store_item_id) VALUES (%lld, %lld, 1, 0, 0);"
            // 8. Item Search
            "INSERT OR REPLACE INTO item_search (item_pid, search_title, search_album, search_artist, search_album_artist, search_composer) VALUES (%lld, %lld, %lld, %lld, %lld, 0);"
            // 9. Item Video, Chapter, Lyrics, Stats
            "INSERT OR IGNORE INTO item_video (item_pid) VALUES (%lld);"
            "INSERT OR IGNORE INTO chapter (item_pid, chapter_data) VALUES (%lld, NULL);"
            "INSERT OR IGNORE INTO lyrics (item_pid, checksum, lyrics) VALUES (%lld, 0, '');"
            "INSERT OR IGNORE INTO item_stats (item_pid, user_rating, play_count_user, play_count_recent, has_been_played, date_accessed) VALUES (%lld, 0, 0, 0, 0, %ld.0);"
            // 10. Sort Map
            "INSERT OR IGNORE INTO sort_map (name, name_order, name_section) VALUES ('%s', %lld, %d);"
            "INSERT OR IGNORE INTO sort_map (name, name_order, name_section) VALUES ('%s', %lld, %d);"
            "INSERT OR IGNORE INTO sort_map (name, name_order, name_section) VALUES ('%s', %lld, %d);"
            "INSERT OR IGNORE INTO sort_map (name, name_order, name_section) VALUES ('%s', %lld, %d);"
            "INSERT OR IGNORE INTO sort_map (name, name_order, name_section) VALUES ('%s', %lld, %d);",
            newItemPid, currentArtistPid, artOrder, artSection,
            currentAlbumPid, albOrder, albSection, currentAlbArtPid, albArtOrder, albArtSection,
            titleOrder, titleSection, t->discNum > 0 ? t->discNum : 1, t->trackNum > 0 ? t->trackNum : 1, t->trackNum > 0 ? t->trackNum : 1, baseLocationId,
            nowMacEpoch, nowMacEpoch,
            newItemPid, escTitle, escTitle, t->trackCountVal > 0 ? t->trackCountVal : 1,
            durationMs, t->yearVal > 0 ? t->yearVal : 2024,
            locStr, t->fileSize, nowMacEpoch,
            escTitle, locStr, newItemPid,
            newItemPid, durationSamples, durationSamples > 1000000 ? durationSamples - 1000000 : 0,
            newItemPid, syncIdItem,
            newItemPid, titleOrder, albOrder, artOrder, albArtOrder,
            newItemPid, newItemPid, newItemPid, newItemPid, nowMacEpoch,
            escTitle, titleOrder, titleSection,
            escAlbum, albOrder, albSection,
            escArtist, artOrder, artSection,
            sortArtist, artOrder, artSection,
            escAlbArt, albArtOrder, albArtSection);
        pSqlExec(db, sqlTrackBatch, NULL, NULL, NULL);
        pSqlExec(db, sqlTrackBatch, NULL, NULL, NULL);

        // 11. Artwork extraction & upload (Apple spec - Optimized: Once per album!)
        int foundArtIdx = -1;
        for (int a = 0; a < artAlbumCount; a++) {
            if (artAlbumPids[a] == currentAlbumPid) {
                foundArtIdx = a;
                break;
            }
        }

        // Check if album already had artwork in device library from earlier syncs
        if (foundArtIdx == -1) {
            sqlite3_stmt *sExistTok = NULL;
            char sqlExistTok[512];
            snprintf(sqlExistTok, sizeof(sqlExistTok), "SELECT artwork_token FROM artwork_token WHERE entity_pid = %lld AND entity_type = 4 LIMIT 1;", currentAlbumPid);
            if (pSqlPrepare(db, sqlExistTok, -1, &sExistTok, NULL) == 0 && sExistTok) {
                if (pSqlStep(sExistTok) == 100) {
                    const char *exTok = (const char*)pSqlColText(sExistTok, 0);
                    if (exTok && exTok[0] != '\0' && artAlbumCount < MAX_CACHED_ITEMS) {
                        artAlbumPids[artAlbumCount] = currentAlbumPid;
                        strncpy(artTokens[artAlbumCount], exTok, 31);
                        artTokens[artAlbumCount][31] = '\0';
                        foundArtIdx = artAlbumCount++;
                    }
                }
                pSqlFinalize(sExistTok);
            }
        }

        if (foundArtIdx >= 0) {
            // Fast path: Album artwork already uploaded! Link this track to the album token instantly!
            const char *cachedTok = artTokens[foundArtIdx];
            char sqlArtLink[1024];
            snprintf(sqlArtLink, sizeof(sqlArtLink),
                "INSERT OR REPLACE INTO artwork_token (entity_pid, entity_type, artwork_token, artwork_type, artwork_source_type, artwork_variant_type, background_color, primary_text_color, secondary_text_color, tertiary_text_color, quaternary_text_color, gradient_text_color, gradient_color, gradient_size_start, gradient_size_end) VALUES (%lld, 0, '%s', 1, 300, 0, '', '', '', '', '', '', '', -1.0, -1.0);"
                "INSERT OR REPLACE INTO best_artwork_token (entity_pid, entity_type, artwork_type, available_artwork_token, fetchable_artwork_token, fetchable_artwork_source_type, artwork_variant_type) VALUES (%lld, 0, 1, '%s', '', 0, 0);",
                newItemPid, cachedTok,
                newItemPid, cachedTok);
            pSqlExec(db, sqlArtLink, NULL, NULL, NULL);
        } else {
            // Full path: First track of this album with artwork - extract and upload once
            HANDLE hArtCheck = CreateFileW(t->filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (hArtCheck != INVALID_HANDLE_VALUE) {
                DWORD dwArtSize = GetFileSize(hArtCheck, NULL);
                if (dwArtSize > 1024 && dwArtSize < 50 * 1024 * 1024) {
                    DWORD toRead = dwArtSize > 4 * 1024 * 1024 ? 4 * 1024 * 1024 : dwArtSize;
                    unsigned char *hdrBuf = (unsigned char*)malloc(toRead);
                    DWORD dwReadArt = 0;
                    if (hdrBuf && ReadFile(hArtCheck, hdrBuf, toRead, &dwReadArt, NULL)) {
                        for (DWORD bi = 0; bi + 30 < dwReadArt; bi++) {
                            if (memcmp(hdrBuf + bi, "APIC", 4) == 0) {
                                // Determine APIC frame size (syncsafe vs big-endian)
                                DWORD frameSzSyncsafe = ((DWORD)hdrBuf[bi+4] << 21) | ((DWORD)hdrBuf[bi+5] << 14) | ((DWORD)hdrBuf[bi+6] << 7) | (DWORD)hdrBuf[bi+7];
                                DWORD frameSzBe = ((DWORD)hdrBuf[bi+4] << 24) | ((DWORD)hdrBuf[bi+5] << 16) | ((DWORD)hdrBuf[bi+6] << 8) | (DWORD)hdrBuf[bi+7];
                                DWORD frameSz = (frameSzSyncsafe > 0 && bi + 10 + frameSzSyncsafe <= dwReadArt) ? frameSzSyncsafe : frameSzBe;
                                if (bi + 10 + frameSz > dwReadArt) frameSz = dwReadArt - (bi + 10);

                                DWORD soi = 0;
                                for (DWORD si = bi + 10; si + 1 < bi + 10 + frameSz; si++) {
                                    if (hdrBuf[si] == 0xFF && hdrBuf[si+1] == 0xD8) {
                                        soi = si;
                                        break;
                                    }
                                }
                                if (soi > 0) {
                                    DWORD eoi = 0;
                                    for (DWORD ei = bi + 10 + frameSz - 2; ei >= soi + 2; ei--) {
                                        if (hdrBuf[ei] == 0xFF && hdrBuf[ei+1] == 0xD9) {
                                            eoi = ei + 2;
                                            break;
                                        }
                                    }
                                    if (eoi > soi) {
                                        DWORD jpgLen = eoi - soi;

                                        // Generate unique incremental token string
                                        char artTok[32];
                                        snprintf(artTok, sizeof(artTok), "%lld", ++maxArtTokVal);

                                        // Compute SHA-1 of the token string (CRITICAL: Apple MediaLibrary hashes token, not image bytes!)
                                        char sha1TokHex[41] = {0};
                                        calc_sha1((const unsigned char*)artTok, strlen(artTok), sha1TokHex);

                                        char dir1[128], dir2[128], remArt[256];
                                        snprintf(dir1, sizeof(dir1), "/iTunes_Control/iTunes/Artwork/Originals");
                                        snprintf(dir2, sizeof(dir2), "/iTunes_Control/iTunes/Artwork/Originals/%.2s", sha1TokHex);
                                        snprintf(remArt, sizeof(remArt), "%s/%s", dir2, sha1TokHex + 2);

                                        Apple_AfcMakeDir(&sess, dir1);
                                        Apple_AfcMakeDir(&sess, dir2);

                                        unsigned long long hRemoteArt = 0;
                                        if (Apple_AfcOpenFile(&sess, remArt, AFC_MODE_RW, &hRemoteArt) == 0 && hRemoteArt) {
                                            Apple_AfcWriteFile(&sess, hRemoteArt, hdrBuf + soi, (unsigned long long)jpgLen);
                                            Apple_AfcCloseFile(&sess, hRemoteArt);
                                        }

                                        char cacheDir[MAX_PATH];
                                        snprintf(cacheDir, sizeof(cacheDir), "%s\\ArtworkCache", g_appDir);
                                        CreateDirectoryA(cacheDir, NULL);
                                        char localCachePath[MAX_PATH];
                                        snprintf(localCachePath, sizeof(localCachePath), "%s\\%s.jpg", cacheDir, artTok);
                                        FILE *fLocalArt = fopen(localCachePath, "wb");
                                        if (fLocalArt) {
                                            fwrite(hdrBuf + soi, 1, jpgLen, fLocalArt);
                                            fclose(fLocalArt);
                                        }

                                        // Pre-render and upload all Apple Music resolution thumbnail caches (100% in-memory)
                                        GenerateAndUploadThumbnails(&sess, hdrBuf + soi, (size_t)jpgLen, sha1TokHex);

                                        char relPath[128];
                                        snprintf(relPath, sizeof(relPath), "%.2s/%s", sha1TokHex, sha1TokHex + 2);

                                        char sqlArtIns[3072];
                                        snprintf(sqlArtIns, sizeof(sqlArtIns),
                                            "INSERT OR REPLACE INTO artwork (artwork_token, artwork_source_type, relative_path, artwork_type, interest_data, artwork_variant_type) "
                                            "VALUES ('%s', 300, '%s', 6, CAST('{\"ColorAnalysis\":{\"1\":{\"primaryTextColorLight\":\"YES\",\"secondaryTextColorLight\":\"YES\",\"secondaryTextColor\":\"#C8B8A8\",\"tertiaryTextColorLight\":\"NO\",\"primaryTextColor\":\"#E8E0D8\",\"tertiaryTextColor\":\"#A09080\",\"backgroundColorLight\":\"NO\",\"backgroundColor\":\"#1A1618\"}}}' AS BLOB), 0);"
                                            "INSERT OR REPLACE INTO artwork_token (entity_pid, entity_type, artwork_token, artwork_type, artwork_source_type, artwork_variant_type, background_color, primary_text_color, secondary_text_color, tertiary_text_color, quaternary_text_color, gradient_text_color, gradient_color, gradient_size_start, gradient_size_end) VALUES (%lld, 4, '%s', 6, 300, 0, '', '', '', '', '', '', '', -1.0, -1.0);"
                                            "INSERT OR REPLACE INTO best_artwork_token (entity_pid, entity_type, artwork_type, available_artwork_token, fetchable_artwork_token, fetchable_artwork_source_type, artwork_variant_type) VALUES (%lld, 4, 6, '%s', '', 0, 0);"
                                            "INSERT OR REPLACE INTO artwork_token (entity_pid, entity_type, artwork_token, artwork_type, artwork_source_type, artwork_variant_type, background_color, primary_text_color, secondary_text_color, tertiary_text_color, quaternary_text_color, gradient_text_color, gradient_color, gradient_size_start, gradient_size_end) VALUES (%lld, 0, '%s', 1, 300, 0, '', '', '', '', '', '', '', -1.0, -1.0);"
                                            "INSERT OR REPLACE INTO best_artwork_token (entity_pid, entity_type, artwork_type, available_artwork_token, fetchable_artwork_token, fetchable_artwork_source_type, artwork_variant_type) VALUES (%lld, 0, 1, '%s', '', 0, 0);",
                                            artTok, relPath,
                                            currentAlbumPid, artTok,
                                            currentAlbumPid, artTok,
                                            newItemPid, artTok,
                                            newItemPid, artTok);
                                        pSqlExec(db, sqlArtIns, NULL, NULL, NULL);

                                        if (artAlbumCount < MAX_CACHED_ITEMS) {
                                            artAlbumPids[artAlbumCount] = currentAlbumPid;
                                            strncpy(artTokens[artAlbumCount], artTok, 31);
                                            artTokens[artAlbumCount][31] = '\0';
                                            artAlbumCount++;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        free(hdrBuf);
                    }
                }
                CloseHandle(hArtCheck);
            }
        }

        // 12. Entity Revision (Track: class=0)
        maxRev++;
        char sqlRev[512];
        snprintf(sqlRev, sizeof(sqlRev),
            "INSERT OR REPLACE INTO entity_revision (revision, entity_pid, class, deleted, revision_type) VALUES (%lld, %lld, 0, 0, 0);",
            maxRev, newItemPid);
        pSqlExec(db, sqlRev, NULL, NULL, NULL);

        t->status = STATUS_SYNCED;
        if (g_activeView == VIEW_TRANSFER_QUEUE) {
            ListView_SetItemText(g_hListView, i, 1, L"✓ Synced to iPhone");
        }
    }

    if (audioChunk) free(audioChunk);

    // Entity Revision for Albums (class=4), Album Artists (class=7), Item Artists (class=2)
    for (int a = 0; a < numCreatedAlbums; ++a) {
        maxRev++;
        char sqlRevAlb[512];
        snprintf(sqlRevAlb, sizeof(sqlRevAlb),
            "INSERT OR REPLACE INTO entity_revision (revision, entity_pid, class, deleted, revision_type) VALUES (%lld, %lld, 4, 0, 0);",
            maxRev, createdAlbumPids[a]);
        pSqlExec(db, sqlRevAlb, NULL, NULL, NULL);
    }

    for (int a = 0; a < numCreatedAlbArts; ++a) {
        maxRev++;
        char sqlRevArt[512];
        snprintf(sqlRevArt, sizeof(sqlRevArt),
            "INSERT OR REPLACE INTO entity_revision (revision, entity_pid, class, deleted, revision_type) VALUES (%lld, %lld, 7, 0, 0);",
            maxRev, createdAlbArtPids[a]);
        pSqlExec(db, sqlRevArt, NULL, NULL, NULL);
    }

    for (int a = 0; a < numCreatedItemArts; ++a) {
        maxRev++;
        char sqlRevIArt[512];
        snprintf(sqlRevIArt, sizeof(sqlRevIArt),
            "INSERT OR REPLACE INTO entity_revision (revision, entity_pid, class, deleted, revision_type) VALUES (%lld, %lld, 2, 0, 0);",
            maxRev, createdItemArtPids[a]);
        pSqlExec(db, sqlRevIArt, NULL, NULL, NULL);
    }

    // Update _MLDatabaseProperties sync timestamp and revision
    // Notice: MLSyncClientLastSyncedRevision is anchored to baseSyncRev so medialibraryd processes all new revisions (> baseSyncRev)
    long nowMacEpoch = (long)(time(NULL) - 978307200);
    char sqlProp[512];
    snprintf(sqlProp, sizeof(sqlProp),
        "UPDATE _MLDatabaseProperties SET value = '%lld' WHERE key = 'MLSyncClientLastSyncedRevision';"
        "UPDATE _MLDatabaseProperties SET value = '%ld.0' WHERE key = 'MPDateLastSynced';",
        baseSyncRev, nowMacEpoch);
    pSqlExec(db, sqlProp, NULL, NULL, NULL);

    pSqlExec(db, "COMMIT;", NULL, NULL, NULL);

    // Checkpoint & flush WAL to master database file
    pSqlExec(db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
    pSqlExec(db, "PRAGMA journal_mode=DELETE;", NULL, NULL, NULL);
    pSqlClose(db);

    // Push updated MediaLibrary.sqlitedb back to iPhone over AFC
    unsigned long long hRemoteDb = 0;
    if (Apple_AfcOpenFile(&sess, "/iTunes_Control/iTunes/MediaLibrary.sqlitedb", AFC_MODE_RW, &hRemoteDb) == 0 && hRemoteDb) {
        FILE *fUp = fopen(g_localDbPath, "rb");
        if (fUp) {
            char *upBuf = (char*)malloc(262144);
            if (upBuf) {
                size_t bytes = 0;
                while ((bytes = fread(upBuf, 1, 262144, fUp)) > 0) {
                    Apple_AfcWriteFile(&sess, hRemoteDb, upBuf, (unsigned long long)bytes);
                }
                free(upBuf);
            }
            fclose(fUp);
        }
        Apple_AfcCloseFile(&sess, hRemoteDb);
    }

    // Clean up WAL and SHM files on iPhone
    Apple_AfcRemovePath(&sess, "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-wal");
    Apple_AfcRemovePath(&sess, "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-shm");

    // Notify iOS that sync finished -> instantly reloads MediaLibrary without reboot!
    Apple_PostNotification(&sess, "com.apple.itunes-mobdev.syncDidFinish");
    Sleep(200);

    Apple_StopSession(&sess);

    g_isSyncing = 0;
    EnableWindow(g_hBtnPushAlbum, TRUE);
    EnableWindow(g_hBtnAddTracks, TRUE);
    EnableWindow(g_hBtnSyncQueue, TRUE);

    // Refresh in-memory list and UI view
    LoadLocalCachedLibrary();
    PopulateListView();

    // Update Sidebar
    wchar_t libTitle[64];
    swprintf(libTitle, 64, L"Library (%d songs)", g_deviceTrackCount);
    TVITEMW tvi;
    memset(&tvi, 0, sizeof(tvi));
    tvi.hItem = g_hItemLibrary;
    tvi.mask = TVIF_TEXT;
    tvi.pszText = libTitle;
    SendMessageW(g_hSidebar, TVM_SETITEMW, 0, (LPARAM)&tvi);

    // Automatically switch to Library View so user sees the new album
    SendMessageW(g_hSidebar, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_hItemLibrary);

    swprintf(g_statusText, 256, L"Sync complete. %d track%ls added.", syncedCount, syncedCount == 1 ? L"" : L"s");
    InvalidateRect(g_hMainWnd, NULL, FALSE);

    wchar_t doneMsg[256];
    swprintf(doneMsg, 256, L"Sync complete.\n%d track%ls added to library.", syncedCount, syncedCount == 1 ? L"" : L"s");
    MessageBoxW(g_hMainWnd, doneMsg, L"Sync Complete", MB_ICONINFORMATION);
    return 0;
}

// Window Procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        HFONT hFontBold = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        // Action Buttons in Header Toolbar (Custom Owner-Drawn Glossy Buttons)
        g_hBtnPushAlbum = CreateWindowW(L"BUTTON", L"💿 Push Album",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            330, 10, 135, 32, hWnd, (HMENU)IDC_BTN_PUSHALBUM, NULL, NULL);
        SubclassModernButton(g_hBtnPushAlbum);

        g_hBtnAddTracks = CreateWindowW(L"BUTTON", L"➕ Add Files",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            475, 11, 100, 30, hWnd, (HMENU)IDC_BTN_ADDTRACKS, NULL, NULL);
        SubclassModernButton(g_hBtnAddTracks);

        g_hBtnExport = CreateWindowW(L"BUTTON", L"📥 Export",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            585, 11, 90, 30, hWnd, (HMENU)IDC_BTN_EXPORT, NULL, NULL);
        SubclassModernButton(g_hBtnExport);

        g_hBtnRefresh = CreateWindowW(L"BUTTON", L"🔄 Refresh",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            685, 11, 85, 30, hWnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);
        SubclassModernButton(g_hBtnRefresh);

        // Instant Filter Search Box
        g_hSearchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
            780, 12, 180, 28, hWnd, (HMENU)IDC_EDIT_SEARCH, NULL, NULL);
        SendMessageW(g_hSearchEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hSearchEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter tracks (Ctrl+F)...");
        s_oldEditProc = (WNDPROC)SetWindowLongPtrW(g_hSearchEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditSubclass);

        // Left Sidebar (Clean TreeView Source List)
        g_hSidebar = CreateWindowExW(0, WC_TREEVIEWW, L"",
            WS_VISIBLE | WS_CHILD | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 52, 220, 350, hWnd, (HMENU)IDC_TREE_SIDEBAR, NULL, NULL);
        SendMessageW(g_hSidebar, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hSidebar, TVM_SETBKCOLOR, 0, (LPARAM)COLOR_SIDEBAR_BG);

        // Album Art Preview Box in Sidebar
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        g_hArtPreview = CreateWindowExW(0, L"EyeChoonsArtPreview", L"",
            WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN,
            15, 410, 190, 240, hWnd, (HMENU)IDC_ART_PREVIEW, hInst, NULL);

        // Populate Sidebar
        TVINSERTSTRUCTW tvis;
        memset(&tvis, 0, sizeof(tvis));
        tvis.hParent = TVI_ROOT;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT | TVIF_STATE;
        tvis.item.state = TVIS_EXPANDED | TVIS_BOLD;
        tvis.item.stateMask = TVIS_EXPANDED | TVIS_BOLD;

        tvis.item.pszText = L"📱 iPhone";
        HTREEITEM hDev = (HTREEITEM)SendMessageW(g_hSidebar, TVM_INSERTITEMW, 0, (LPARAM)&tvis);

        tvis.hParent = hDev;
        tvis.item.state = TVIS_BOLD;
        wchar_t libTitle[64];
        swprintf(libTitle, 64, L"Library (%d)", g_deviceTrackCount);
        tvis.item.pszText = libTitle;
        g_hItemLibrary = (HTREEITEM)SendMessageW(g_hSidebar, TVM_INSERTITEMW, 0, (LPARAM)&tvis);

        tvis.item.pszText = L"Transfer Queue (0)";
        g_hItemQueue = (HTREEITEM)SendMessageW(g_hSidebar, TVM_INSERTITEMW, 0, (LPARAM)&tvis);

        SendMessageW(g_hSidebar, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_hItemLibrary);

        // Main ListView
        g_hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_VISIBLE | WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
            220, 52, 850, 560, hWnd, (HMENU)IDC_LIST_MAIN, NULL, NULL);
        SendMessageW(g_hListView, WM_SETFONT, (WPARAM)hFont, TRUE);
        ListView_SetExtendedListViewStyle(g_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        // Sync Queue Button (only shown when in queue view)
        g_hBtnSyncQueue = CreateWindowW(L"BUTTON", L"⚡ Sync Queue",
            WS_CHILD | BS_OWNERDRAW,
            870, 618, 140, 28, hWnd, (HMENU)IDC_BTN_SYNCQUEUE, NULL, NULL);
        SubclassModernButton(g_hBtnSyncQueue);

        // Remove & Clear buttons at bottom left for queue management
        g_hBtnRemove = CreateWindowW(L"BUTTON", L"Remove",
            WS_CHILD | BS_OWNERDRAW,
            12, 622, 75, 24, hWnd, (HMENU)IDC_BTN_REMOVE, NULL, NULL);
        SubclassModernButton(g_hBtnRemove);

        g_hBtnClear = CreateWindowW(L"BUTTON", L"Clear",
            WS_CHILD | BS_OWNERDRAW,
            92, 622, 65, 24, hWnd, (HMENU)IDC_BTN_CLEAR, NULL, NULL);
        SubclassModernButton(g_hBtnClear);

        DragAcceptFiles(hWnd, TRUE);

        // Initial setup & immediate library display
        SwitchView(VIEW_DEVICE_LIBRARY);

        // Start Background Apple Mobile Device Thread
        g_hDevThread = CreateThread(NULL, 0, DeviceMonitorThread, NULL, 0, NULL);
        break;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 980;
        mmi->ptMinTrackSize.y = 540;
        return 0;
    }

    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        if (width < 400 || height < 300) break;

        // Position Top Action Buttons
        int right = width - 15;
        int btnY = 11;
        int btnH = 30;

        int searchW = width > 1100 ? 190 : 150;
        MoveWindow(g_hSearchEdit, right - searchW, btnY + 1, searchW, 28, TRUE);
        right -= (searchW + 8);

        int refreshW = 85;
        MoveWindow(g_hBtnRefresh, right - refreshW, btnY, refreshW, btnH, TRUE);
        right -= (refreshW + 8);

        int exportW = 90;
        MoveWindow(g_hBtnExport, right - exportW, btnY, exportW, btnH, TRUE);
        right -= (exportW + 8);

        int addW = 100;
        MoveWindow(g_hBtnAddTracks, right - addW, btnY, addW, btnH, TRUE);
        right -= (addW + 8);

        int pushW = 135;
        MoveWindow(g_hBtnPushAlbum, right - pushW, btnY - 1, pushW, btnH + 2, TRUE);

        // Sidebar & Main ListView
        int topY = 52;
        int botH = 40;
        int sideW = 220;
        int mainH = height - topY - botH;
        int mainW = width - sideW;

        int previewW = sideW - 30; // 190px
        int previewH = previewW + 50; // 240px
        int treeH = mainH - previewH - 12;
        if (treeH < 120) {
            treeH = 120;
            previewH = mainH - treeH - 12;
        }

        MoveWindow(g_hSidebar, 0, topY, sideW, treeH, TRUE);
        if (g_hArtPreview) {
            MoveWindow(g_hArtPreview, 15, topY + treeH + 6, previewW, previewH, TRUE);
        }
        MoveWindow(g_hListView, sideW, topY, mainW, mainH, TRUE);

        // Bottom Left Buttons
        MoveWindow(g_hBtnRemove, 12, height - 32, 75, 24, TRUE);
        MoveWindow(g_hBtnClear, 92, height - 32, 65, 24, TRUE);
        MoveWindow(g_hBtnSyncQueue, width - 155, height - 34, 140, 28, TRUE);

        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case WM_APP_DEVICE_CONNECTED: {
        wchar_t libTitle[64];
        swprintf(libTitle, 64, L"Library (%d songs)", g_deviceTrackCount);
        TVITEMW tvi;
        memset(&tvi, 0, sizeof(tvi));
        tvi.hItem = g_hItemLibrary;
        tvi.mask = TVIF_TEXT;
        tvi.pszText = libTitle;
        SendMessageW(g_hSidebar, TVM_SETITEMW, 0, (LPARAM)&tvi);

        CheckDuplicates();
        PopulateListView();
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case WM_APP_DEVICE_DISCONNECTED: {
        TVITEMW tvi;
        memset(&tvi, 0, sizeof(tvi));
        tvi.hItem = g_hItemLibrary;
        tvi.mask = TVIF_TEXT;
        tvi.pszText = L"Library (Disconnected)";
        SendMessageW(g_hSidebar, TVM_SETITEMW, 0, (LPARAM)&tvi);

        PopulateListView();
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        HWND hwndEdit = (HWND)lParam;
        if (hwndEdit == g_hSearchEdit) {
            SetTextColor(hdcEdit, RGB(25, 30, 40));
            SetBkColor(hdcEdit, RGB(255, 255, 255));
            static HBRUSH s_editBrush = NULL;
            if (!s_editBrush) s_editBrush = CreateSolidBrush(RGB(255, 255, 255));
            return (LRESULT)s_editBrush;
        }
        break;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT*)lParam;
        if (!dis) break;
        int isHov = (GetPropW(dis->hwndItem, L"IsHovered") != NULL);

        HFONT hBtnFontBold = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hBtnFontSemi = CreateFontW(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hBtnFontReg = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        switch (dis->CtlID) {
        case IDC_BTN_PUSHALBUM:
            DrawGlossyButton(dis, isHov, L"💿 Push Album",
                             RGB(255, 45, 85), RGB(218, 18, 58),
                             RGB(255, 75, 112), RGB(236, 32, 72),
                             RGB(192, 12, 48), RGB(165, 8, 38),
                             RGB(240, 242, 246), RGB(228, 231, 237),
                             RGB(190, 12, 48), RGB(255, 95, 130),
                             RGB(255, 255, 255), RGB(160, 168, 180),
                             RGB(255, 155, 180), 8, hBtnFontBold);
            break;
        case IDC_BTN_ADDTRACKS:
            DrawGlossyButton(dis, isHov, L"➕ Add Files",
                             RGB(255, 255, 255), RGB(242, 245, 250),
                             RGB(255, 255, 255), RGB(234, 239, 248),
                             RGB(226, 232, 242), RGB(235, 240, 248),
                             RGB(245, 246, 248), RGB(238, 240, 244),
                             RGB(208, 214, 224), RGB(165, 185, 220),
                             RGB(35, 42, 55), RGB(160, 168, 180),
                             RGB(255, 255, 255), 7, hBtnFontSemi);
            break;
        case IDC_BTN_EXPORT:
            DrawGlossyButton(dis, isHov, L"📥 Export",
                             RGB(255, 255, 255), RGB(242, 245, 250),
                             RGB(255, 255, 255), RGB(234, 239, 248),
                             RGB(226, 232, 242), RGB(235, 240, 248),
                             RGB(245, 246, 248), RGB(238, 240, 244),
                             RGB(208, 214, 224), RGB(165, 185, 220),
                             RGB(35, 42, 55), RGB(160, 168, 180),
                             RGB(255, 255, 255), 7, hBtnFontSemi);
            break;
        case IDC_BTN_REFRESH:
            DrawGlossyButton(dis, isHov, L"🔄 Refresh",
                             RGB(255, 255, 255), RGB(242, 245, 250),
                             RGB(255, 255, 255), RGB(234, 239, 248),
                             RGB(226, 232, 242), RGB(235, 240, 248),
                             RGB(245, 246, 248), RGB(238, 240, 244),
                             RGB(208, 214, 224), RGB(165, 185, 220),
                             RGB(35, 42, 55), RGB(160, 168, 180),
                             RGB(255, 255, 255), 7, hBtnFontSemi);
            break;
        case IDC_BTN_SYNCQUEUE:
            DrawGlossyButton(dis, isHov, L"⚡ Sync Queue",
                             RGB(0, 122, 255), RGB(0, 95, 205),
                             RGB(32, 145, 255), RGB(0, 115, 225),
                             RGB(0, 80, 175), RGB(0, 68, 150),
                             RGB(240, 242, 246), RGB(228, 231, 237),
                             RGB(0, 85, 190), RGB(80, 170, 255),
                             RGB(255, 255, 255), RGB(160, 168, 180),
                             RGB(120, 190, 255), 8, hBtnFontBold);
            break;
        case IDC_BTN_REMOVE:
            DrawGlossyButton(dis, isHov, L"Remove",
                             RGB(255, 255, 255), RGB(244, 246, 250),
                             RGB(255, 255, 255), RGB(236, 240, 247),
                             RGB(228, 233, 242), RGB(236, 240, 248),
                             RGB(245, 246, 248), RGB(238, 240, 244),
                             RGB(212, 216, 224), RGB(175, 185, 205),
                             RGB(60, 68, 80), RGB(160, 168, 180),
                             RGB(255, 255, 255), 6, hBtnFontReg);
            break;
        case IDC_BTN_CLEAR:
            DrawGlossyButton(dis, isHov, L"Clear",
                             RGB(255, 255, 255), RGB(244, 246, 250),
                             RGB(255, 255, 255), RGB(236, 240, 247),
                             RGB(228, 233, 242), RGB(236, 240, 248),
                             RGB(245, 246, 248), RGB(238, 240, 244),
                             RGB(212, 216, 224), RGB(175, 185, 205),
                             RGB(60, 68, 80), RGB(160, 168, 180),
                             RGB(255, 255, 255), 6, hBtnFontReg);
            break;
        }

        DeleteObject(hBtnFontBold);
        DeleteObject(hBtnFontSemi);
        DeleteObject(hBtnFontReg);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_EDIT_SEARCH && code == EN_CHANGE) {
            PopulateListView();
            break;
        }

        switch (id) {
        case IDC_BTN_PUSHALBUM: OnPushAlbumClicked(); break;
        case IDC_BTN_ADDTRACKS: OnAddTracksClicked(); break;
        case IDC_BTN_EXPORT:    ExportSelectedTracks(); break;
        case IDC_BTN_REFRESH: {
            int devId = 0;
            char udid[64] = {0};
            if (Apple_FindDevice(&devId, udid, sizeof(udid))) {
                g_currentDeviceId = devId;
                strncpy(g_currentDeviceUdid, udid, sizeof(g_currentDeviceUdid) - 1);
                if (LoadDeviceLibrary()) {
                    g_deviceConnected = 1;
                    PostMessageW(hWnd, WM_APP_DEVICE_CONNECTED, 0, 0);
                }
                PopulateListView();
            } else if (g_deviceConnected && g_currentDeviceId) {
                LoadDeviceLibrary();
                PopulateListView();
            } else {
                LoadLocalCachedLibrary();
                PopulateListView();
            }
            break;
        }
        case IDC_BTN_SYNCQUEUE: CreateThread(NULL, 0, SyncQueueWorker, NULL, 0, NULL); break;
        case IDC_BTN_REMOVE: {
            if (g_activeView != VIEW_TRANSFER_QUEUE) break;
            for (int i = g_queueCount - 1; i >= 0; --i) {
                if (ListView_GetItemState(g_hListView, i, LVIS_SELECTED) & LVIS_SELECTED) {
                    for (int j = i; j < g_queueCount - 1; ++j) g_queue[j] = g_queue[j + 1];
                    g_queueCount--;
                }
            }
            PopulateListView();
            break;
        }
        case IDC_BTN_CLEAR: {
            if (g_activeView != VIEW_TRANSFER_QUEUE) break;
            g_queueCount = 0;
            PopulateListView();
            break;
        }
        case IDM_REVEAL_EXPLORER: {
            if (g_activeView == VIEW_TRANSFER_QUEUE) {
                int row = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
                if (row >= 0 && row < g_queueCount) {
                    RevealInExplorer(g_queue[row].filePath);
                }
            }
            break;
        }
        case IDM_REMOVE_QUEUE: {
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_REMOVE, 0), 0);
            break;
        }
        case IDM_CLEAR_QUEUE: {
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_CLEAR, 0), 0);
            break;
        }
        case IDM_EXPORT_TRACKS: {
            ExportSelectedTracks();
            break;
        }
        case IDM_COPY_INFO: {
            CopySelectedToClipboard(g_hListView);
            break;
        }
        case IDM_SELECT_ALL: {
            ListView_SetItemState(g_hListView, -1, LVIS_SELECTED, LVIS_SELECTED);
            break;
        }
        case IDM_REFRESH_LIB: {
            SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_REFRESH, 0), 0);
            break;
        }
        }
        break;
    }

    case WM_CONTEXTMENU: {
        HWND hTarget = (HWND)wParam;
        if (hTarget == g_hListView) {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetWindowRect(g_hListView, &rc);
                pt.x = rc.left + 50; pt.y = rc.top + 50;
            }

            HMENU hMenu = CreatePopupMenu();
            int selCount = ListView_GetSelectedCount(g_hListView);

            if (g_activeView == VIEW_TRANSFER_QUEUE) {
                if (selCount > 0) {
                    AppendMenuW(hMenu, MF_STRING, IDM_REVEAL_EXPLORER, L"📂 Reveal in File Explorer\tDouble-Click");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING, IDM_REMOVE_QUEUE, L"❌ Remove Selected\tDel");
                }
                AppendMenuW(hMenu, MF_STRING, IDM_CLEAR_QUEUE, L"🧹 Clear Entire Queue");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, IDM_COPY_INFO, L"📋 Copy Song Info\tCtrl+C");
                AppendMenuW(hMenu, MF_STRING, IDM_SELECT_ALL, L"Select All\tCtrl+A");
            } else {
                if (selCount > 0) {
                    AppendMenuW(hMenu, MF_STRING, IDM_EXPORT_TRACKS, L"📥 Export Selected Tracks to PC...");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                }
                AppendMenuW(hMenu, MF_STRING, IDM_COPY_INFO, L"📋 Copy Song Info\tCtrl+C");
                AppendMenuW(hMenu, MF_STRING, IDM_SELECT_ALL, L"Select All\tCtrl+A");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(hMenu, MF_STRING, IDM_REFRESH_LIB, L"🔄 Refresh Library\tF5");
            }

            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
            break;
        }
        break;
    }

    // Sidebar Node Selection Switcher & ListView Notifications
    case WM_NOTIFY: {
        LPNMHDR pnmh = (LPNMHDR)lParam;
        if (pnmh->idFrom == IDC_TREE_SIDEBAR && pnmh->code == TVN_SELCHANGEDW) {
            LPNMTREEVIEWW pnmtv = (LPNMTREEVIEWW)lParam;
            wchar_t text[64] = {0};
            TVITEMW item;
            item.hItem = pnmtv->itemNew.hItem;
            item.mask = TVIF_TEXT;
            item.pszText = text;
            item.cchTextMax = 64;
            SendMessageW(g_hSidebar, TVM_GETITEMW, 0, (LPARAM)&item);

            if (wcsstr(text, L"Library")) {
                SwitchView(VIEW_DEVICE_LIBRARY);
            } else if (wcsstr(text, L"Queue") || wcsstr(text, L"Transfer")) {
                SwitchView(VIEW_TRANSFER_QUEUE);
            }
        } else if (pnmh->idFrom == IDC_LIST_MAIN && pnmh->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
            switch (lplvcd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                int row = (int)lplvcd->nmcd.dwItemSpec;
                if (ListView_GetItemState(g_hListView, row, LVIS_SELECTED) & LVIS_SELECTED) {
                    lplvcd->clrTextBk = COLOR_ZEBRA_SEL;
                    lplvcd->clrText = RGB(255, 255, 255);
                } else {
                    lplvcd->clrTextBk = (row % 2 == 0) ? COLOR_ZEBRA_EVEN : COLOR_ZEBRA_ODD;
                    if (g_activeView == VIEW_TRANSFER_QUEUE && row < g_queueCount) {
                        if (g_queue[row].isDuplicate) lplvcd->clrText = COLOR_TEXT_MUTED;
                        else if (g_queue[row].status == STATUS_SYNCED) lplvcd->clrText = COLOR_TEXT_SUCCESS;
                        else lplvcd->clrText = COLOR_TEXT_PRIMARY;
                    } else {
                        lplvcd->clrText = COLOR_TEXT_PRIMARY;
                    }
                }
                return CDRF_DODEFAULT;
            }
            }
        } else if (pnmh->idFrom == IDC_LIST_MAIN && pnmh->code == NM_DBLCLK) {
            if (g_activeView == VIEW_TRANSFER_QUEUE) {
                LPNMITEMACTIVATE pnmia = (LPNMITEMACTIVATE)lParam;
                if (pnmia->iItem >= 0 && pnmia->iItem < g_queueCount) {
                    RevealInExplorer(g_queue[pnmia->iItem].filePath);
                }
            } else {
                ExportSelectedTracks();
            }
        } else if (pnmh->idFrom == IDC_LIST_MAIN && pnmh->code == LVN_KEYDOWN) {
            LPNMLVKEYDOWN pnkd = (LPNMLVKEYDOWN)lParam;
            if (pnkd->wVKey == VK_DELETE && g_activeView == VIEW_TRANSFER_QUEUE) {
                SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_REMOVE, 0), 0);
            }
        } else if (pnmh->idFrom == IDC_LIST_MAIN && pnmh->code == LVN_ITEMCHANGED) {
            LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
            if (pnmv && (pnmv->uChanged & LVIF_STATE)) {
                if ((pnmv->uNewState & LVIS_SELECTED) || (ListView_GetSelectedCount(g_hListView) == 0)) {
                    int selCount = ListView_GetSelectedCount(g_hListView);
                    if (g_activeView == VIEW_DEVICE_LIBRARY) {
                        if (selCount > 0) {
                            swprintf(g_statusText, 256, L"%d tracks • %d selected", g_deviceTrackCount, selCount);
                        } else {
                            swprintf(g_statusText, 256, L"%d tracks in device library", g_deviceTrackCount);
                        }
                        RECT rcClient; GetClientRect(hWnd, &rcClient);
                        RECT rcBottom = {180, rcClient.bottom - 40, rcClient.right - 260, rcClient.bottom};
                        InvalidateRect(hWnd, &rcBottom, FALSE);
                    }
                    UpdateSelectedArtworkPreview();
                }
            }
        }
        break;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        int prevCount = g_queueCount;
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < fileCount; ++i) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(hDrop, i, path, MAX_PATH);
            DWORD attr = GetFileAttributesW(path);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                AddFolderToQueue(path);
            } else {
                AddFileToQueue(path);
            }
        }
        DragFinish(hDrop);

        int added = g_queueCount - prevCount;
        if (added > 0) {
            CheckDuplicates();
            SwitchView(VIEW_TRANSFER_QUEUE);
            SendMessageW(g_hSidebar, TVM_SELECTITEM, TVGN_CARET, (LPARAM)g_hItemQueue);
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT clientRc;
        GetClientRect(hWnd, &clientRc);

        // 1. Top Modern Header Bar
        RECT rcHeader = clientRc;
        rcHeader.bottom = 52;
        DrawGradient(hdc, rcHeader, COLOR_TOOLBAR_TOP, COLOR_TOOLBAR_BOT);

        // Top subtle highlight sheen
        HPEN hSheenPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(hdc, hSheenPen);
        MoveToEx(hdc, 0, 0, NULL); LineTo(hdc, clientRc.right, 0);

        // Bottom dividing border
        HPEN hLinePen = CreatePen(PS_SOLID, 1, COLOR_BORDER_LINE);
        SelectObject(hdc, hLinePen);
        MoveToEx(hdc, 0, 51, NULL); LineTo(hdc, clientRc.right, 51);
        SelectObject(hdc, oldPen);
        DeleteObject(hSheenPen);
        DeleteObject(hLinePen);

        // Glossy Connection Status Pill on Left Header
        RECT rcPill = { 14, 9, 185, 43 };
        HRGN pillRgn = CreateRoundRectRgn(rcPill.left, rcPill.top, rcPill.right + 1, rcPill.bottom + 1, 14, 14);
        SelectClipRgn(hdc, pillRgn);

        if (g_deviceConnected) {
            DrawGradient(hdc, rcPill, RGB(242, 253, 245), RGB(225, 248, 232));
        } else {
            DrawGradient(hdc, rcPill, RGB(250, 251, 252), RGB(241, 243, 246));
        }
        SelectClipRgn(hdc, NULL);
        DeleteObject(pillRgn);

        // Pill border
        HPEN pillBorder = CreatePen(PS_SOLID, 1, g_deviceConnected ? RGB(167, 243, 208) : RGB(225, 229, 235));
        HGDIOBJ oldPillPen = SelectObject(hdc, pillBorder);
        HGDIOBJ oldPillBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom, 14, 14);
        SelectObject(hdc, oldPillBrush);
        SelectObject(hdc, oldPillPen);
        DeleteObject(pillBorder);

        // Status Indicator Dot
        int dotX = 28;
        int dotY = 26;
        if (g_deviceConnected) {
            // Soft green outer glow
            HBRUSH glowBrush = CreateSolidBrush(RGB(167, 243, 208));
            HGDIOBJ oldB = SelectObject(hdc, glowBrush);
            HPEN glowPen = CreatePen(PS_SOLID, 1, RGB(110, 231, 183));
            HGDIOBJ oldP = SelectObject(hdc, glowPen);
            Ellipse(hdc, dotX - 5, dotY - 5, dotX + 6, dotY + 6);

            // Bright emerald core
            HBRUSH dotBrush = CreateSolidBrush(RGB(16, 185, 129));
            SelectObject(hdc, dotBrush);
            HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(5, 150, 105));
            SelectObject(hdc, dotPen);
            Ellipse(hdc, dotX - 3, dotY - 3, dotX + 4, dotY + 4);

            SelectObject(hdc, oldB);
            SelectObject(hdc, oldP);
            DeleteObject(glowBrush);
            DeleteObject(glowPen);
            DeleteObject(dotBrush);
            DeleteObject(dotPen);
        } else {
            HBRUSH dotBrush = CreateSolidBrush(RGB(156, 163, 175));
            HGDIOBJ oldB = SelectObject(hdc, dotBrush);
            HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(107, 114, 128));
            HGDIOBJ oldP = SelectObject(hdc, dotPen);
            Ellipse(hdc, dotX - 4, dotY - 4, dotX + 5, dotY + 5);
            SelectObject(hdc, oldB);
            SelectObject(hdc, oldP);
            DeleteObject(dotBrush);
            DeleteObject(dotPen);
        }

        // Pill text
        SetBkMode(hdc, TRANSPARENT);
        HFONT hPillTitle = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hPillSub = CreateFontW(10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(hdc, hPillTitle);
        SetTextColor(hdc, g_deviceConnected ? RGB(20, 83, 45) : RGB(75, 85, 99));
        RECT rcPillTitle = { 42, 12, rcPill.right - 6, 26 };
        DrawTextW(hdc, g_deviceConnected ? L"iPhone Connected" : L"iPhone Offline", -1, &rcPillTitle, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        SelectObject(hdc, hPillSub);
        SetTextColor(hdc, g_deviceConnected ? RGB(22, 101, 52) : RGB(107, 114, 128));
        RECT rcPillSub = { 42, 26, rcPill.right - 6, 40 };
        DrawTextW(hdc, g_deviceConnected ? L"USB High-Speed • Ready" : L"Connect via USB cable", -1, &rcPillSub, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        DeleteObject(hPillTitle);
        DeleteObject(hPillSub);

        // Beside pill: track count & audio capacity info (when window has sufficient width)
        if (clientRc.right >= 960) {
            HFONT hStatBold = CreateFontW(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hStatMuted = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SelectObject(hdc, hStatBold);
            SetTextColor(hdc, RGB(30, 36, 46));
            RECT rcLibStat = { 198, 12, 320, 26 };
            wchar_t tcStr[64];
            swprintf(tcStr, 64, L"%d tracks", g_deviceTrackCount);
            DrawTextW(hdc, tcStr, -1, &rcLibStat, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            SelectObject(hdc, hStatMuted);
            SetTextColor(hdc, COLOR_TEXT_MUTED);
            RECT rcLibSub = { 198, 26, 320, 40 };
            wchar_t gbStr[64];
            swprintf(gbStr, 64, L"%.2f GB on device", (double)g_deviceAudioTotalBytes / (1024.0 * 1024.0 * 1024.0));
            DrawTextW(hdc, gbStr, -1, &rcLibSub, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            DeleteObject(hStatBold);
            DeleteObject(hStatMuted);
        }

        // 2. Bottom Status Bar
        RECT rcBottom = clientRc;
        rcBottom.top = clientRc.bottom - 40;
        DrawGradient(hdc, rcBottom, COLOR_TOOLBAR_TOP, COLOR_TOOLBAR_BOT);

        HPEN botBorderPen = CreatePen(PS_SOLID, 1, COLOR_BORDER_LINE);
        HGDIOBJ oldBotPen = SelectObject(hdc, botBorderPen);
        MoveToEx(hdc, 0, rcBottom.top, NULL); LineTo(hdc, clientRc.right, rcBottom.top);
        SelectObject(hdc, oldBotPen);
        DeleteObject(botBorderPen);

        // Status Text on Bottom Left
        HFONT hFooterFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(hdc, hFooterFont);
        SetTextColor(hdc, RGB(70, 78, 92));
        RECT rcStat = { 180, rcBottom.top + 8, clientRc.right - 210, rcBottom.bottom - 6 };
        DrawTextW(hdc, g_statusText, -1, &rcStat, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        DeleteObject(hFooterFont);

        // Capacity pill on Bottom Right (only in Library View when queue button is hidden)
        if (g_activeView == VIEW_DEVICE_LIBRARY) {
            RECT rcCapPill = { clientRc.right - 180, rcBottom.top + 7, clientRc.right - 15, rcBottom.bottom - 7 };
            HRGN capRgn = CreateRoundRectRgn(rcCapPill.left, rcCapPill.top, rcCapPill.right + 1, rcCapPill.bottom + 1, 10, 10);
            SelectClipRgn(hdc, capRgn);
            DrawGradient(hdc, rcCapPill, RGB(255, 255, 255), RGB(242, 245, 250));
            SelectClipRgn(hdc, NULL);
            DeleteObject(capRgn);

            HPEN capBorder = CreatePen(PS_SOLID, 1, RGB(216, 221, 230));
            HGDIOBJ p1 = SelectObject(hdc, capBorder);
            HGDIOBJ p2 = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rcCapPill.left, rcCapPill.top, rcCapPill.right, rcCapPill.bottom, 10, 10);
            SelectObject(hdc, p2);
            SelectObject(hdc, p1);
            DeleteObject(capBorder);

            HFONT hCapFont = CreateFontW(11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SelectObject(hdc, hCapFont);
            SetTextColor(hdc, RGB(45, 52, 65));
            wchar_t capText[64];
            swprintf(capText, 64, L"🎵 Audio: %.2f GB", (double)g_deviceAudioTotalBytes / (1024.0 * 1024.0 * 1024.0));
            DrawTextW(hdc, capText, -1, &rcCapPill, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            DeleteObject(hCapFont);
        }

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY:
        if (g_pPreviewBitmap && pDispose) {
            pDispose((GpImage*)g_pPreviewBitmap);
            g_pPreviewBitmap = NULL;
        }
        g_devThreadRunning = 0;
        if (g_hDevThread) {
            WaitForSingleObject(g_hDevThread, 1000);
            CloseHandle(g_hDevThread);
        }
        if (g_deviceTracks) {
            free(g_deviceTracks);
            g_deviceTracks = NULL;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// Dynamic Loader for Apple MobileDevice & SQLite3 DLLs
int InitAppleConduits() {
    GetModuleFileNameA(NULL, g_appDir, MAX_PATH);
    char *lastSlash = strrchr(g_appDir, '\\');
    if (lastSlash) *lastSlash = '\0';
    else snprintf(g_appDir, sizeof(g_appDir), ".");

    snprintf(g_localDbPath, sizeof(g_localDbPath), "%s\\iPhoneLibrary.db", g_appDir);

    HMODULE hSql = LoadLibraryA("winsqlite3.dll");
    if (!hSql) hSql = LoadLibraryA("sqlite3.dll");
    if (!hSql) return 0;

    pSqlOpen = (sqlite3_open_t)GetProcAddress(hSql, "sqlite3_open");
    pSqlClose = (sqlite3_close_t)GetProcAddress(hSql, "sqlite3_close");
    pSqlPrepare = (sqlite3_prepare_v2_t)GetProcAddress(hSql, "sqlite3_prepare_v2");
    pSqlStep = (sqlite3_step_t)GetProcAddress(hSql, "sqlite3_step");
    pSqlColInt64 = (sqlite3_column_int64_t)GetProcAddress(hSql, "sqlite3_column_int64");
    pSqlColText = (sqlite3_column_text_t)GetProcAddress(hSql, "sqlite3_column_text");
    pSqlFinalize = (sqlite3_finalize_t)GetProcAddress(hSql, "sqlite3_finalize");
    pSqlExec = (sqlite3_exec_t)GetProcAddress(hSql, "sqlite3_exec");
    pSqlErrMsg = (sqlite3_errmsg_t)GetProcAddress(hSql, "sqlite3_errmsg");

    // Initialize GDI+ once for lifetime of application
    g_hGdi = LoadLibraryA("gdiplus.dll");
    if (g_hGdi) {
        GdiplusStartup_t pGdiStartup = (GdiplusStartup_t)GetProcAddress(g_hGdi, "GdiplusStartup");
        pGdiShutdown = (GdiplusShutdown_t)GetProcAddress(g_hGdi, "GdiplusShutdown");
        pCreateBmpFromStream = (GdipCreateBitmapFromStream_t)GetProcAddress(g_hGdi, "GdipCreateBitmapFromStream");
        pCreateBmp = (GdipCreateBitmapFromScan0_t)GetProcAddress(g_hGdi, "GdipCreateBitmapFromScan0");
        pGetCtx = (GdipGetImageGraphicsContext_t)GetProcAddress(g_hGdi, "GdipGetImageGraphicsContext");
        pSetInterp = (GdipSetInterpolationMode_t)GetProcAddress(g_hGdi, "GdipSetInterpolationMode");
        pDraw = (GdipDrawImageRectRectI_t)GetProcAddress(g_hGdi, "GdipDrawImageRectRectI");
        pSaveToStream = (GdipSaveImageToStream_t)GetProcAddress(g_hGdi, "GdipSaveImageToStream");
        pDispose = (GdipDisposeImage_t)GetProcAddress(g_hGdi, "GdipDisposeImage");
        pDelCtx = (GdipDeleteGraphics_t)GetProcAddress(g_hGdi, "GdipDeleteGraphics");
        pGetWidth = (GdipGetImageWidth_t)GetProcAddress(g_hGdi, "GdipGetImageWidth");
        pGetHeight = (GdipGetImageHeight_t)GetProcAddress(g_hGdi, "GdipGetImageHeight");
        pGetCtxHdc = (GdipCreateFromHDC_t)GetProcAddress(g_hGdi, "GdipCreateFromHDC");
        pDrawRectI = (GdipDrawImageRectI_t)GetProcAddress(g_hGdi, "GdipDrawImageRectI");
        pCreateBmpFromFile = (GdipCreateBitmapFromFile_t)GetProcAddress(g_hGdi, "GdipCreateBitmapFromFile");

        if (pGdiStartup) {
            GdiplusStartupInput input = {1, NULL, FALSE, FALSE};
            pGdiStartup(&g_gdiToken, &input, NULL);
        }
    }

    return 1;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    if (!InitAppleConduits()) {
        MessageBoxW(NULL,
            L"Database engine (winsqlite3.dll) failed to load.\n\n"
            L"Running in limited offline mode.",
            L"Eye Choons", MB_ICONWARNING | MB_OK);
    }

    // Immediately load the local cached library snapshot so tracks appear instantly on launch
    LoadLocalCachedLibrary();

    WNDCLASSEXW wca;
    memset(&wca, 0, sizeof(WNDCLASSEXW));
    wca.cbSize = sizeof(WNDCLASSEXW);
    wca.lpfnWndProc = ArtPreviewWndProc;
    wca.hInstance = hInstance;
    wca.hCursor = LoadCursor(NULL, IDC_ARROW);
    wca.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wca.lpszClassName = L"EyeChoonsArtPreview";
    RegisterClassExW(&wca);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(WNDCLASSEXW));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EyeChoonsWndClass";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) return 0;

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 1100;
    int winH = 720;
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    g_hMainWnd = CreateWindowExW(
        0,
        L"EyeChoonsWndClass",
        L"Eye Choons - iPhone Music Manager",
        WS_OVERLAPPEDWINDOW,
        posX, posY, winW, winH,
        NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return 0;

    ShowWindow(g_hMainWnd, SW_SHOWNORMAL);
    UpdateWindow(g_hMainWnd);
    SetForegroundWindow(g_hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && (msg.wParam == 'A' || msg.wParam == 'a')) {
                ListView_SetItemState(g_hListView, -1, LVIS_SELECTED, LVIS_SELECTED);
                continue;
            }
            if (ctrl && (msg.wParam == 'C' || msg.wParam == 'c') && GetFocus() == g_hListView) {
                CopySelectedToClipboard(g_hListView);
                continue;
            }
            if (ctrl && (msg.wParam == 'F' || msg.wParam == 'f')) {
                SetFocus(g_hSearchEdit);
                SendMessageW(g_hSearchEdit, EM_SETSEL, 0, -1);
                continue;
            }
            if (ctrl && shift && (msg.wParam == 'O' || msg.wParam == 'o')) {
                OnPushAlbumClicked();
                continue;
            }
            if (ctrl && (msg.wParam == 'O' || msg.wParam == 'o')) {
                OnAddTracksClicked();
                continue;
            }
            if (msg.wParam == VK_F5) {
                SendMessageW(g_hMainWnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_REFRESH, 0), 0);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_gdiToken && pGdiShutdown) pGdiShutdown(g_gdiToken);
    if (g_hGdi) FreeLibrary(g_hGdi);
    WSACleanup();
    CoUninitialize();

    return (int)msg.wParam;
}
