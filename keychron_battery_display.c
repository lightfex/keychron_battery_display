#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <shellapi.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define KEYCHRON_VENDOR_ID 0x3434
#define IOCTL_HID_GET_FEATURE_REPORT 0x000B0192
#define RF_REPORT_LEN 21

#define WM_TRAYICON (WM_APP + 1)
#define TIMER_ID_REFRESH 1001
#define MENU_ID_REFRESH 40001
#define MENU_ID_EXIT 40002
#define MENU_ID_AUTOSTART_TOGGLE 40003
#define DEFAULT_REFRESH_MS 10000

#define RUN_KEY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE_NAME L"KeychronBatteryDisplayDirectHid"

static const wchar_t *AUTOSTART_MENU_TEXT = L"开机启动";

typedef struct KnownInterface {
    wchar_t hid_interface[128];
} KnownInterface;

typedef struct Endpoint {
    wchar_t path[512];
    USHORT vendor_id;
    USHORT product_id;
    USHORT usage_page;
    USHORT usage;
    USHORT feature_len;
} Endpoint;

typedef struct BatteryState {
    uint8_t work_mode;
    uint8_t connected;
    uint8_t percent;
    uint8_t charging;
    uint8_t raw[RF_REPORT_LEN];
    wchar_t endpoint_path[512];
} BatteryState;

typedef struct AppState {
    HINSTANCE instance;
    HWND hwnd;
    NOTIFYICONDATAW nid;
    BOOL icon_added;
    HICON icon;
    UINT refresh_ms;
    BOOL has_battery;
    BatteryState battery;
    wchar_t config_path[MAX_PATH];
    wchar_t status_text[256];
    BOOL autostart_enabled;
} AppState;

static void copy_wstr(wchar_t *dst, size_t dst_count, const wchar_t *src) {
    if (dst_count == 0) {
        return;
    }
    if (!src) {
        dst[0] = L'\0';
        return;
    }
    wcsncpy(dst, src, dst_count - 1);
    dst[dst_count - 1] = L'\0';
}

static void format_wstr(wchar_t *dst, size_t dst_count, const wchar_t *fmt, ...) {
    va_list args;
    if (dst_count == 0) {
        return;
    }
    va_start(args, fmt);
    _vsnwprintf(dst, dst_count - 1, fmt, args);
    va_end(args);
    dst[dst_count - 1] = L'\0';
}

static void trim_ascii(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (unsigned char)s[len - 1] <= ' ') {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] && (unsigned char)s[start] <= ' ') {
        ++start;
    }
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

static bool contains_wcs_ci(const wchar_t *haystack, const wchar_t *needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    size_t hay_len = wcslen(haystack);
    size_t needle_len = wcslen(needle);
    if (needle_len > hay_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= hay_len; ++i) {
        if (_wcsnicmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool find_adjacent_config(wchar_t *out, size_t out_count) {
    wchar_t module_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, module_path, ARRAYSIZE(module_path));
    if (len == 0 || len >= ARRAYSIZE(module_path)) {
        copy_wstr(out, out_count, L"config.xml");
        return false;
    }

    wchar_t *slash = wcsrchr(module_path, L'\\');
    if (slash) {
        slash[1] = L'\0';
    } else {
        module_path[0] = L'\0';
    }

    copy_wstr(out, out_count, module_path);
    if (wcslen(out) + 10 < out_count) {
        wcscat(out, L"config.xml");
    }

    DWORD attr = GetFileAttributesW(out);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool get_module_path(wchar_t *out, size_t out_count) {
    DWORD len = GetModuleFileNameW(NULL, out, (DWORD)out_count);
    if (len == 0 || len >= out_count) {
        if (out_count > 0) {
            out[0] = L'\0';
        }
        return false;
    }
    return true;
}

static bool is_autostart_enabled(void) {
    HKEY key = NULL;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    wchar_t value[1024];
    DWORD size = sizeof(value);
    rc = RegQueryValueExW(key, RUN_VALUE_NAME, NULL, &type, (LPBYTE)value, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

static bool set_autostart_enabled(bool enabled) {
    if (!enabled) {
        HKEY key = NULL;
        LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_SET_VALUE, &key);
        if (rc != ERROR_SUCCESS) {
            return false;
        }
        rc = RegDeleteValueW(key, RUN_VALUE_NAME);
        RegCloseKey(key);
        return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }

    wchar_t module_path[MAX_PATH];
    wchar_t command[2 * MAX_PATH];
    if (!get_module_path(module_path, ARRAYSIZE(module_path))) {
        return false;
    }

    _snwprintf(command, ARRAYSIZE(command) - 1, L"\"%ls\"", module_path);
    command[ARRAYSIZE(command) - 1] = L'\0';

    HKEY key = NULL;
    LONG rc = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        RUN_KEY_PATH,
        0,
        NULL,
        0,
        KEY_SET_VALUE,
        NULL,
        &key,
        NULL);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    rc = RegSetValueExW(
        key,
        RUN_VALUE_NAME,
        0,
        REG_SZ,
        (const BYTE *)command,
        (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static HFONT create_menu_font(void) {
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        return CreateFontIndirectW(&ncm.lfMenuFont);
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void measure_autostart_menu_item(MEASUREITEMSTRUCT *mis) {
    HDC dc = GetDC(NULL);
    HFONT font = create_menu_font();
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE size = {0};

    GetTextExtentPoint32W(dc, AUTOSTART_MENU_TEXT, (int)wcslen(AUTOSTART_MENU_TEXT), &size);
    mis->itemWidth = (UINT)(size.cx + 44);
    mis->itemHeight = (UINT)max(size.cy + 8, 24);

    SelectObject(dc, old_font);
    if (font && font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font);
    }
    ReleaseDC(NULL, dc);
}

static void draw_autostart_menu_item(HWND hwnd, const DRAWITEMSTRUCT *dis) {
    AppState *app = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    RECT rc = dis->rcItem;
    HDC dc = dis->hDC;
    BOOL selected = (dis->itemState & ODS_SELECTED) != 0;
    BOOL enabled = app ? app->autostart_enabled : FALSE;

    COLORREF bg = GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_MENU);
    COLORREF text_color;
    if (selected) {
        text_color = GetSysColor(COLOR_HIGHLIGHTTEXT);
    } else if (enabled) {
        text_color = GetSysColor(COLOR_MENUTEXT);
    } else {
        text_color = RGB(140, 140, 140);
    }

    HBRUSH bg_brush = CreateSolidBrush(bg);
    FillRect(dc, &rc, bg_brush);
    DeleteObject(bg_brush);

    RECT check_rc = rc;
    check_rc.right = check_rc.left + 20;
    RECT text_rc = rc;
    text_rc.left += 26;

    if (enabled) {
        RECT glyph = check_rc;
        glyph.left += 2;
        glyph.top += 4;
        glyph.right -= 2;
        glyph.bottom -= 4;
        DrawFrameControl(dc, &glyph, DFC_MENU, DFCS_MENUCHECK);
    }

    HFONT font = create_menu_font();
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_color);
    DrawTextW(dc, AUTOSTART_MENU_TEXT, -1, &text_rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    SelectObject(dc, old_font);
    if (font && font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font);
    }

    if ((dis->itemState & ODS_FOCUS) != 0) {
        DrawFocusRect(dc, &rc);
    }
}

static size_t load_known_interfaces(const wchar_t *config_path, KnownInterface *items, size_t capacity) {
    FILE *fp = _wfopen(config_path, L"rb");
    if (!fp) {
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return 0;
    }

    char *content = (char *)calloc((size_t)size + 1, 1);
    if (!content) {
        fclose(fp);
        return 0;
    }
    fread(content, 1, (size_t)size, fp);
    fclose(fp);

    size_t count = 0;
    char *cursor = content;
    while ((cursor = strstr(cursor, "<mode")) != NULL) {
        char *end = strchr(cursor, '>');
        if (!end) {
            break;
        }

        bool is_24g = false;
        char *value = strstr(cursor, "value=\"");
        if (value && value < end) {
            value += 7;
            is_24g = (*value == '1' || *value == '3');
        }

        char *iface = strstr(cursor, "hid_interface=\"");
        if (is_24g && iface && iface < end) {
            iface += 15;
            char *iface_end = strchr(iface, '"');
            if (iface_end && iface_end <= end) {
                size_t len = (size_t)(iface_end - iface);
                if (len > 0 && len < 120 && count < capacity) {
                    char temp[128];
                    memcpy(temp, iface, len);
                    temp[len] = '\0';
                    trim_ascii(temp);

                    wchar_t wide[128];
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, temp, -1, wide, (int)ARRAYSIZE(wide));
                    if (wlen > 0) {
                        bool dup = false;
                        for (size_t i = 0; i < count; ++i) {
                            if (_wcsicmp(items[i].hid_interface, wide) == 0) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            copy_wstr(items[count].hid_interface, ARRAYSIZE(items[count].hid_interface), wide);
                            ++count;
                        }
                    }
                }
            }
        }

        cursor = end + 1;
    }

    free(content);
    return count;
}

static bool path_matches_known_interfaces(const wchar_t *path, const KnownInterface *known, size_t known_count) {
    if (known_count == 0) {
        return true;
    }
    for (size_t i = 0; i < known_count; ++i) {
        if (contains_wcs_ci(path, known[i].hid_interface)) {
            return true;
        }
    }
    return false;
}

static size_t enumerate_endpoints(const KnownInterface *known, size_t known_count, Endpoint *items, size_t capacity) {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO devs = SetupDiGetClassDevsW(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) {
        return 0;
    }

    size_t count = 0;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface = {0};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(devs, NULL, &hid_guid, index, &iface)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &iface, NULL, 0, &needed, NULL);
        if (!needed) {
            continue;
        }

        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
        if (!detail) {
            continue;
        }
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &iface, detail, needed, NULL, NULL)) {
            free(detail);
            continue;
        }

        if (!path_matches_known_interfaces(detail->DevicePath, known, known_count)) {
            free(detail);
            continue;
        }

        HANDLE handle = CreateFileW(
            detail->DevicePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            free(detail);
            continue;
        }

        HIDD_ATTRIBUTES attr = {0};
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(handle, &attr) || attr.VendorID != KEYCHRON_VENDOR_ID) {
            CloseHandle(handle);
            free(detail);
            continue;
        }

        PHIDP_PREPARSED_DATA prep = NULL;
        HIDP_CAPS caps;
        if (HidD_GetPreparsedData(handle, &prep) &&
            HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS &&
            caps.UsagePage == 0x008C &&
            caps.Usage == 0x0001 &&
            caps.FeatureReportByteLength >= RF_REPORT_LEN &&
            count < capacity) {
            Endpoint *ep = &items[count++];
            copy_wstr(ep->path, ARRAYSIZE(ep->path), detail->DevicePath);
            ep->vendor_id = attr.VendorID;
            ep->product_id = attr.ProductID;
            ep->usage_page = caps.UsagePage;
            ep->usage = caps.Usage;
            ep->feature_len = caps.FeatureReportByteLength;
        }

        if (prep) {
            HidD_FreePreparsedData(prep);
        }
        CloseHandle(handle);
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return count;
}

static bool get_feature_report_timeout(HANDLE handle, uint8_t *buf, DWORD len, DWORD timeout_ms) {
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        return false;
    }

    DWORD transferred = 0;
    BOOL ok = DeviceIoControl(
        handle,
        IOCTL_HID_GET_FEATURE_REPORT,
        buf,
        len,
        buf,
        len,
        &transferred,
        &ov);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait_rc = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait_rc == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, &ov, &transferred, FALSE);
        } else {
            CancelIo(handle);
            ok = FALSE;
        }
    }

    CloseHandle(ov.hEvent);
    return ok ? true : false;
}

static bool query_endpoint_battery(const Endpoint *ep, BatteryState *state) {
    HANDLE handle = CreateFileW(
        ep->path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint8_t buf[RF_REPORT_LEN] = {0};
    buf[0] = 0x51;
    buf[1] = 0x06;

    BOOL ok = HidD_SetFeature(handle, buf, (ULONG)sizeof(buf));
    if (!ok) {
        CloseHandle(handle);
        return false;
    }

    ZeroMemory(buf, sizeof(buf));
    buf[0] = 0x51;
    buf[1] = 0x06;
    if (!get_feature_report_timeout(handle, buf, (DWORD)sizeof(buf), 1500)) {
        CloseHandle(handle);
        return false;
    }

    CloseHandle(handle);

    if (buf[0] != 0x51 || buf[1] != 0x06 || buf[11] > 100) {
        return false;
    }

    ZeroMemory(state, sizeof(*state));
    memcpy(state->raw, buf, sizeof(buf));
    state->work_mode = (uint8_t)(buf[10] & 0x07);
    state->connected = (uint8_t)((buf[10] >> 3) & 0x01);
    state->percent = buf[11];
    state->charging = buf[12];
    copy_wstr(state->endpoint_path, ARRAYSIZE(state->endpoint_path), ep->path);
    return true;
}

static bool query_keychron_battery(const wchar_t *config_path, BatteryState *state, wchar_t *status, size_t status_count) {
    KnownInterface known[64];
    Endpoint endpoints[32];

    size_t known_count = load_known_interfaces(config_path, known, ARRAYSIZE(known));
    size_t endpoint_count = enumerate_endpoints(known, known_count, endpoints, ARRAYSIZE(endpoints));
    if (endpoint_count == 0) {
        format_wstr(status, status_count, L"未找到 Keychron 2.4G HID 电量接口");
        return false;
    }

    for (size_t i = 0; i < endpoint_count; ++i) {
        if (query_endpoint_battery(&endpoints[i], state)) {
            format_wstr(status, status_count, L"OK: VID=%04X PID=%04X", endpoints[i].vendor_id, endpoints[i].product_id);
            return true;
        }
    }

    format_wstr(status, status_count, L"找到 HID 接口，但读取 0x51 0x06 电量失败");
    return false;
}

static COLORREF battery_color(const AppState *app) {
    if (!app || !app->has_battery) {
        return RGB(150, 150, 150);
    }
    if (app->battery.charging != 0) {
        return RGB(80, 210, 255);
    }
    if (app->battery.percent < 10) {
        return RGB(255, 60, 60);
    }
    if (app->battery.percent <= 20) {
        return RGB(255, 210, 40);
    }
    return RGB(40, 220, 90);
}

static HICON create_status_icon(const AppState *app) {
    int base = GetSystemMetrics(SM_CXSMICON);
    int size = base > 0 ? base * 4 : 64;
    if (size < 64) {
        size = 64;
    }

    BITMAPV5HEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP color_bmp = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP mask_bmp = CreateBitmap(size, size, 1, 1, NULL);
    HDC mem_dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    if (!color_bmp || !mask_bmp || !mem_dc || !bits) {
        if (color_bmp) DeleteObject(color_bmp);
        if (mask_bmp) DeleteObject(mask_bmp);
        if (mem_dc) DeleteDC(mem_dc);
        return LoadIconW(NULL, IDI_INFORMATION);
    }

    ZeroMemory(bits, (size_t)size * (size_t)size * 4);
    HGDIOBJ old_bmp = SelectObject(mem_dc, color_bmp);

    wchar_t text[16];
    if (!app || !app->has_battery) {
        copy_wstr(text, ARRAYSIZE(text), L"--");
    } else if (app->battery.charging != 0) {
        copy_wstr(text, ARRAYSIZE(text), L"充");
    } else {
        format_wstr(text, ARRAYSIZE(text), L"%u", app->battery.percent);
    }

    COLORREF color = battery_color(app);
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 18));
    HBRUSH border = CreateSolidBrush(color);
    RECT full = {0, 0, size, size};
    FillRect(mem_dc, &full, bg);

    RECT bar = {0, size - 8, size, size};
    FillRect(mem_dc, &bar, border);

    int font_height = app && app->has_battery && app->battery.charging ? -(size * 5 / 8) : -(size * 7 / 12);
    HFONT font = CreateFontW(
        font_height,
        0,
        0,
        0,
        FW_HEAVY,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(mem_dc, font);
    SetBkMode(mem_dc, TRANSPARENT);
    SetTextColor(mem_dc, color);

    RECT text_rect = {0, 1, size, size - 7};
    DrawTextW(mem_dc, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    SelectObject(mem_dc, old_font);
    SelectObject(mem_dc, old_bmp);
    DeleteObject(font);
    DeleteObject(bg);
    DeleteObject(border);
    DeleteDC(mem_dc);

    ICONINFO icon_info;
    ZeroMemory(&icon_info, sizeof(icon_info));
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bmp;
    icon_info.hbmMask = mask_bmp;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(color_bmp);
    DeleteObject(mask_bmp);
    return icon ? icon : LoadIconW(NULL, IDI_INFORMATION);
}

static void update_tray_icon(AppState *app) {
    if (app->icon) {
        DestroyIcon(app->icon);
        app->icon = NULL;
    }
    app->icon = create_status_icon(app);

    ZeroMemory(&app->nid, sizeof(app->nid));
    app->nid.cbSize = sizeof(app->nid);
    app->nid.hWnd = app->hwnd;
    app->nid.uID = 1;
    app->nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app->nid.uCallbackMessage = WM_TRAYICON;
    app->nid.hIcon = app->icon;

    if (app->has_battery) {
        if (app->battery.charging != 0) {
            format_wstr(app->nid.szTip, ARRAYSIZE(app->nid.szTip), L"Keychron 鼠标：充电中 (%u%%)", app->battery.percent);
        } else {
            format_wstr(app->nid.szTip, ARRAYSIZE(app->nid.szTip), L"Keychron 鼠标电量：%u%%", app->battery.percent);
        }
    } else {
        copy_wstr(app->nid.szTip, ARRAYSIZE(app->nid.szTip), app->status_text);
    }

    Shell_NotifyIconW(app->icon_added ? NIM_MODIFY : NIM_ADD, &app->nid);
    app->icon_added = TRUE;
}

static void refresh_state(AppState *app) {
    BatteryState battery;
    wchar_t status[256];
    ZeroMemory(&battery, sizeof(battery));
    ZeroMemory(status, sizeof(status));

    if (query_keychron_battery(app->config_path, &battery, status, ARRAYSIZE(status))) {
        app->has_battery = TRUE;
        app->battery = battery;
        copy_wstr(app->status_text, ARRAYSIZE(app->status_text), status);
    } else {
        app->has_battery = FALSE;
        ZeroMemory(&app->battery, sizeof(app->battery));
        copy_wstr(app->status_text, ARRAYSIZE(app->status_text), status);
    }

    app->autostart_enabled = is_autostart_enabled();

    update_tray_icon(app);
}

static void show_context_menu(AppState *app) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    wchar_t line[256];
    if (app->has_battery) {
        if (app->battery.charging != 0) {
            format_wstr(line, ARRAYSIZE(line), L"充电中 (%u%%)", app->battery.percent);
        } else {
            format_wstr(line, ARRAYSIZE(line), L"电量：%u%%", app->battery.percent);
        }
    } else {
        copy_wstr(line, ARRAYSIZE(line), app->status_text[0] ? app->status_text : L"未读取到电量");
    }

    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, line);
    if (app->has_battery) {
        format_wstr(line, ARRAYSIZE(line), L"work_mode=%u connect=%u charge=%u",
            app->battery.work_mode,
            app->battery.connected,
            app->battery.charging);
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, line);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    app->autostart_enabled = is_autostart_enabled();
    AppendMenuW(menu, MF_OWNERDRAW, MENU_ID_AUTOSTART_TOGGLE, AUTOSTART_MENU_TEXT);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_ID_REFRESH, L"立即刷新");
    AppendMenuW(menu, MF_STRING, MENU_ID_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(app->hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(menu);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    AppState *app = (AppState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
            app = (AppState *)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
            app->hwnd = hwnd;
            refresh_state(app);
            SetTimer(hwnd, TIMER_ID_REFRESH, app->refresh_ms, NULL);
            return 0;
        }
        case WM_TIMER:
            if (app && wparam == TIMER_ID_REFRESH) {
                refresh_state(app);
            }
            return 0;
        case WM_COMMAND:
            if (!app) {
                return 0;
            }
            switch (LOWORD(wparam)) {
                case MENU_ID_AUTOSTART_TOGGLE:
                    if (set_autostart_enabled(app->autostart_enabled ? false : true)) {
                        app->autostart_enabled = app->autostart_enabled ? FALSE : TRUE;
                    }
                    refresh_state(app);
                    return 0;
                case MENU_ID_REFRESH:
                    refresh_state(app);
                    return 0;
                case MENU_ID_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    return 0;
            }
        case WM_TRAYICON:
            if (!app) {
                return 0;
            }
            if (lparam == WM_LBUTTONUP || lparam == NIN_SELECT) {
                refresh_state(app);
                return 0;
            }
            if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
                show_context_menu(app);
                return 0;
            }
            return 0;
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lparam;
            if (mis && mis->CtlType == ODT_MENU && mis->itemID == MENU_ID_AUTOSTART_TOGGLE) {
                measure_autostart_menu_item(mis);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lparam;
            if (dis && dis->CtlType == ODT_MENU && dis->itemID == MENU_ID_AUTOSTART_TOGGLE) {
                draw_autostart_menu_item(hwnd, dis);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            if (app) {
                KillTimer(hwnd, TIMER_ID_REFRESH);
                if (app->icon_added) {
                    Shell_NotifyIconW(NIM_DELETE, &app->nid);
                    app->icon_added = FALSE;
                }
                if (app->icon) {
                    DestroyIcon(app->icon);
                    app->icon = NULL;
                }
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int run_tray_app(HINSTANCE instance, int argc, wchar_t **argv) {
    HANDLE single_instance = CreateMutexW(NULL, TRUE, L"KeychronBatteryDisplayDirectHid");
    if (single_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single_instance);
        return 0;
    }

    AppState app;
    ZeroMemory(&app, sizeof(app));
    app.instance = instance;
    app.refresh_ms = DEFAULT_REFRESH_MS;
    find_adjacent_config(app.config_path, ARRAYSIZE(app.config_path));

    for (int i = 1; i < argc; ++i) {
        if ((_wcsicmp(argv[i], L"--interval") == 0 || _wcsicmp(argv[i], L"-i") == 0) && i + 1 < argc) {
            UINT seconds = (UINT)_wtoi(argv[++i]);
            if (seconds >= 2 && seconds <= 3600) {
                app.refresh_ms = seconds * 1000U;
            }
        } else if ((_wcsicmp(argv[i], L"--config") == 0 || _wcsicmp(argv[i], L"-c") == 0) && i + 1 < argc) {
            copy_wstr(app.config_path, ARRAYSIZE(app.config_path), argv[++i]);
        }
    }

    const wchar_t *class_name = L"KeychronBatteryDisplayWindow";
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"注册窗口类失败", L"Keychron Battery Display", MB_ICONERROR | MB_OK);
        if (single_instance) CloseHandle(single_instance);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        class_name,
        L"Keychron Battery Display",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        NULL,
        NULL,
        instance,
        &app);
    if (!hwnd) {
        MessageBoxW(NULL, L"创建隐藏窗口失败", L"Keychron Battery Display", MB_ICONERROR | MB_OK);
        if (single_instance) CloseHandle(single_instance);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (single_instance) {
        CloseHandle(single_instance);
    }
    return (int)msg.wParam;
}

static void dump_raw(const uint8_t *raw) {
    for (int i = 0; i < RF_REPORT_LEN; ++i) {
        printf("%02X", raw[i]);
        if (i + 1 < RF_REPORT_LEN) {
            putchar(' ');
        }
    }
    putchar('\n');
}

static int run_console_probe(int argc, wchar_t **argv) {
    wchar_t config_path[MAX_PATH];
    find_adjacent_config(config_path, ARRAYSIZE(config_path));

    for (int i = 1; i < argc; ++i) {
        if ((_wcsicmp(argv[i], L"--config") == 0 || _wcsicmp(argv[i], L"-c") == 0) && i + 1 < argc) {
            copy_wstr(config_path, ARRAYSIZE(config_path), argv[++i]);
        }
    }

    BatteryState state;
    wchar_t status[256];
    ZeroMemory(&state, sizeof(state));
    ZeroMemory(status, sizeof(status));

    if (!query_keychron_battery(config_path, &state, status, ARRAYSIZE(status))) {
        fwprintf(stderr, L"query failed: %ls\n", status);
        fwprintf(stderr, L"config=%ls\n", config_path);
        return 2;
    }

    wprintf(L"config=%ls\n", config_path);
    wprintf(L"endpoint=%ls\n", state.endpoint_path);
    printf("work_mode=%u\n", state.work_mode);
    printf("connect=%u\n", state.connected);
    printf("battery=%u\n", state.percent);
    printf("charge=%u\n", state.charging);
    printf("status=%s\n", state.charging ? "charging" : "discharging");
    printf("raw=");
    dump_raw(state.raw);
    return 0;
}

static void enable_console_io(void) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }

    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static bool has_console_mode_arg(int argc, wchar_t **argv) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--probe") == 0 ||
            _wcsicmp(argv[i], L"--console") == 0 ||
            _wcsicmp(argv[i], L"--once") == 0) {
            return true;
        }
    }
    return false;
}

static int get_autostart_mode_arg(int argc, wchar_t **argv) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--autostart-on") == 0) {
            return 1;
        }
        if (_wcsicmp(argv[i], L"--autostart-off") == 0) {
            return 0;
        }
    }
    return -1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR cmdline, int show_cmd) {
    (void)prev_instance;
    (void)cmdline;
    (void)show_cmd;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = 0;
    int autostart_mode = get_autostart_mode_arg(argc, argv);

    if (autostart_mode != -1) {
        enable_console_io();
        if (set_autostart_enabled(autostart_mode == 1)) {
            wprintf(L"autostart=%ls\n", autostart_mode == 1 ? L"enabled" : L"disabled");
            rc = 0;
        } else {
            fwprintf(stderr, L"autostart update failed\n");
            rc = 1;
        }
    } else if (has_console_mode_arg(argc, argv)) {
        enable_console_io();
        rc = run_console_probe(argc, argv);
    } else {
        rc = run_tray_app(instance, argc, argv);
    }

    if (argv) {
        LocalFree(argv);
    }
    return rc;
}
