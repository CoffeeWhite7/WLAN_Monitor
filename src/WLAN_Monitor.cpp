// wifi-channel-tool.cpp
// 校园网 5GHz 信道切换器 —— 用于把 STA 钉到非 DFS 的 5GHz BSSID，
// 从而让 Windows 移动热点可以以 5GHz 频段启动（软 AP 不能落在 DFS 信道）。
//
// 构建（MSVC）:
//   cl /std:c++17 /EHsc /O2 /utf-8 wifi-channel-tool.cpp /link wlanapi.lib comctl32.lib shell32.lib user32.lib gdi32.lib
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <wlanapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------- 控件 ID ----------------
#define MYIDC_INFO     1001
#define MYIDC_LIST     1002
#define MYIDC_REFRESH  1003
#define MYIDC_PIN      1004
#define MYIDC_AUTO     1005
#define MYIDC_HOTSPOT  1006
#define MYIDC_STATUS   1007

// ---------------- 全局状态 ----------------
static HANDLE          g_hClient = nullptr;
static GUID            g_ifGuid   = {};
static std::wstring    g_ifDesc;
static DOT11_SSID      g_ssid     = {};
static std::wstring    g_profile;
static HWND            g_hInfo = nullptr, g_hList = nullptr, g_hStatus = nullptr;

struct BssRow {
    std::wstring ssid;
    std::wstring bssid;
    ULONG        freq = 0;      // kHz
    int          channel = 0;
    LONG         rssi = 0;
    ULONG        quality = 0;
    bool         dfs = false;
    bool         nonDfs5G = false;
    bool         current = false;
    BYTE         mac[6] = {};
};

static std::vector<BssRow> g_rows;

// ---------------- 工具函数 ----------------
static int ChannelFromFreq(ULONG freqKHz)
{
    if (freqKHz >= 2412000 && freqKHz <= 2484000) {
        if (freqKHz == 2484000) return 14;
        return (int)((freqKHz - 2407000) / 5000);
    }
    if (freqKHz >= 5000000 && freqKHz <= 5925000)
        return (int)((freqKHz - 5000000) / 5000);
    if (freqKHz >= 5955000 && freqKHz <= 7125000)
        return (int)((freqKHz - 5950000) / 5000);
    return 0;
}

static bool IsBand5G(ULONG freqKHz) { return freqKHz >= 5000000 && freqKHz <= 5925000; }

// 5GHz DFS: 52-64 (UNII-2A) / 100-144 (UNII-2C)
static bool IsDfsChannel(int ch, ULONG freqKHz)
{
    if (!IsBand5G(freqKHz)) return false;
    return (ch >= 52 && ch <= 64) || (ch >= 100 && ch <= 144);
}

static bool IsNonDfs5G(int ch, ULONG freqKHz)
{
    if (!IsBand5G(freqKHz)) return false;
    return (ch >= 36 && ch <= 48) || (ch >= 149 && ch <= 165);
}

static const wchar_t* BandName(ULONG freqKHz)
{
    if (freqKHz >= 2412000 && freqKHz <= 2484000) return L"2.4G";
    if (IsBand5G(freqKHz))                        return L"5G";
    if (freqKHz >= 5955000)                       return L"6G";
    return L"?";
}

static std::wstring FormatBssid(const BYTE* m)
{
    wchar_t b[32];
    swprintf_s(b, L"%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static std::wstring SsidToText(const DOT11_SSID& s)
{
    if (s.uSSIDLength == 0) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, s.uSSIDLength, nullptr, 0);
    if (n > 0) {
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, s.uSSIDLength, &w[0], n);
        return w;
    }
    return L"";
}

static void SetStatus(const std::wstring& s)
{
    if (g_hStatus) SetWindowTextW(g_hStatus, s.c_str());
}

// ---------------- WLAN 封装 ----------------
static bool WlanInit()
{
    if (g_hClient) return true;
    DWORD neg = 0;
    DWORD rc = WlanOpenHandle(2, nullptr, &neg, &g_hClient);
    if (rc != ERROR_SUCCESS) { g_hClient = nullptr; return false; }

    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    rc = WlanEnumInterfaces(g_hClient, nullptr, &list);
    if (rc != ERROR_SUCCESS || !list) return false;

    // 热点开启时枚举结果里会多出 Wi-Fi Direct 的 AP 接口，对它的 current_connection
    // 查询会返回 ERROR_ACCESS_DENIED。因此逐个探测，挑「真正有 SSID 关联」的那个。
    bool have = false;
    GUID    fallbackGuid = {};
    std::wstring fallbackDesc;
    bool    haveFallback = false;

    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const auto& it = list->InterfaceInfo[i];
        GUID g = it.InterfaceGuid;
        std::wstring desc = it.strInterfaceDescription;

        DWORD sz = 0;
        PWLAN_CONNECTION_ATTRIBUTES a = nullptr;
        DWORD qrc = WlanQueryInterface(g_hClient, &g, wlan_intf_opcode_current_connection,
                                      nullptr, &sz, (PVOID*)&a, nullptr);
        if (qrc == ERROR_SUCCESS && a) {
            bool hasSsid = a->wlanAssociationAttributes.dot11Ssid.uSSIDLength > 0;
            WlanFreeMemory(a);
            if (hasSsid) { g_ifGuid = g; g_ifDesc = desc; have = true; break; }
        }
        if (!haveFallback && it.isState == wlan_interface_state_connected) {
            fallbackGuid = g; fallbackDesc = desc; haveFallback = true;
        }
    }

    if (!have) {
        if (haveFallback) { g_ifGuid = fallbackGuid; g_ifDesc = fallbackDesc; have = true; }
        else if (list->dwNumberOfItems > 0) {
            g_ifGuid = list->InterfaceInfo[0].InterfaceGuid;
            g_ifDesc = list->InterfaceInfo[0].strInterfaceDescription;
            have = true;
        }
    }
    WlanFreeMemory(list);
    return have;
}

static void WlanUninit()
{
    if (g_hClient) { WlanCloseHandle(g_hClient, nullptr); g_hClient = nullptr; }
}

// 读取当前关联信息（SSID / BSSID / 信道 / 频段）
static DWORD g_rcCurrent = 0;

static bool QueryCurrentOnce(DOT11_SSID* outSsid, BYTE* outBssid, int* outCh, ULONG* outFreq, LONG* outRssi)
{
    if (!g_hClient) return false;

    DWORD size = 0;
    PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
    DWORD rc = WlanQueryInterface(g_hClient, &g_ifGuid, wlan_intf_opcode_current_connection,
                                  nullptr, &size, (PVOID*)&attrs, nullptr);
    g_rcCurrent = rc;
    if (rc != ERROR_SUCCESS || !attrs) return false;

    if (outSsid)  *outSsid = attrs->wlanAssociationAttributes.dot11Ssid;
    if (outBssid) memcpy(outBssid, attrs->wlanAssociationAttributes.dot11Bssid, 6);
    if (outRssi)  *outRssi = (LONG)attrs->wlanAssociationAttributes.wlanSignalQuality;

    // wlan_intf_opcode_channel_number 返回的是「信道号」而不是中心频率；
    // 该 API 的输出参数是 PVOID*（由 WlanFreeMemory 释放），不能直接传值指针。
    PVOID pCh = nullptr;
    DWORD chSize = 0;
    if (WlanQueryInterface(g_hClient, &g_ifGuid, wlan_intf_opcode_channel_number,
                           nullptr, &chSize, &pCh, nullptr) == ERROR_SUCCESS && pCh) {
        ULONG chNum = *(ULONG*)pCh;
        if (outCh) *outCh = (int)chNum;
        if (outFreq) {
            if (chNum >= 1 && chNum <= 14)        *outFreq = 2407000 + chNum * 5000;
            else if (chNum >= 32 && chNum <= 196) *outFreq = 5000000 + chNum * 5000;
            else                                  *outFreq = 5950000 + chNum * 5000;
        }
        WlanFreeMemory(pCh);
    }
    WlanFreeMemory(attrs);
    return true;
}

// 带重试的封装：扫描/漫游瞬间 current_connection 可能短暂返回失败
static bool QueryCurrent(DOT11_SSID* outSsid, BYTE* outBssid, int* outCh, ULONG* outFreq, LONG* outRssi)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (QueryCurrentOnce(outSsid, outBssid, outCh, outFreq, outRssi)) return true;
        Sleep(300);
    }
    return false;
}

static bool FindProfileForSsid(const std::wstring& ssid, std::wstring& profileOut)
{
    PWLAN_PROFILE_INFO_LIST pl = nullptr;
    if (WlanGetProfileList(g_hClient, &g_ifGuid, nullptr, &pl) != ERROR_SUCCESS || !pl) return false;

    bool ok = false;
    for (DWORD i = 0; i < pl->dwNumberOfItems; ++i) {
        const wchar_t* name = pl->ProfileInfo[i].strProfileName;
        if (ssid == name) { profileOut = name; ok = true; break; }
    }
    // 名字不一致时退回到「用 SSID 当配置名」
    if (!ok && !ssid.empty()) { profileOut = ssid; ok = true; }
    WlanFreeMemory(pl);
    return ok;
}

static void PopulateList(bool rescan);

// 触发扫描并等待，然后刷新列表
static void ScanAndRefresh(HWND owner)
{
    if (!WlanInit()) { SetStatus(L"无线网卡不可用。"); return; }

    SetStatus(L"扫描中…");
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    SetCursor(old);

    DOT11_SSID ssid = {};
    BYTE bssid[6] = {};
    int ch = 0; ULONG freq = 0; LONG rssi = 0;
    if (QueryCurrent(&ssid, bssid, &ch, &freq, &rssi)) g_ssid = ssid;

    WlanScan(g_hClient, &g_ifGuid, nullptr, nullptr, nullptr);
    Sleep(4000);

    PopulateList(true);
    SetStatus(L"完成。选中一行后点「切换到选中」。");
    (void)owner;
}

static void PopulateList(bool /*rescan*/)
{
    if (g_hList) { ListView_DeleteAllItems(g_hList); g_rows.clear(); }
    if (!WlanInit()) return;

    DOT11_SSID curSsid = {};
    BYTE curBssid[6] = {};
    int  curCh = 0; ULONG curFreq = 0; LONG curRssi = 0;
    bool have = QueryCurrent(&curSsid, curBssid, &curCh, &curFreq, &curRssi);
    if (!have) {
        wchar_t diag[512];
        if (g_rcCurrent == ERROR_ACCESS_DENIED) {
            swprintf_s(diag, L"没有权限读取 Wi-Fi 状态，请用管理员身份运行。接口：%s",
                       g_ifDesc.c_str());
        } else {
            swprintf_s(diag, L"读取 Wi-Fi 状态失败（错误 %lu，接口 %s），点「刷新」重试。",
                       g_rcCurrent, g_ifDesc.c_str());
        }
        if (g_hInfo) SetWindowTextW(g_hInfo, diag);
        SetStatus(diag);
        return;
    }
    g_ssid = curSsid;
    FindProfileForSsid(SsidToText(curSsid), g_profile);

    bool curIsDfs = IsDfsChannel(curCh, curFreq);

    // 顶部两行：一行现状，一行提示
    wchar_t info[512];
    swprintf_s(info, L"%s · 信道 %d · %s · %s\r\n%s",
        SsidToText(curSsid).c_str(), curCh, BandName(curFreq),
        curIsDfs ? L"热点不可用" : L"热点可用",
        curIsDfs ? L"当前是 DFS 信道，5 GHz 热点起不来。切到「热点」列显示可用的那一行即可。"
                 : L"可以直接开 5 GHz 热点。");
    if (g_hInfo) SetWindowTextW(g_hInfo, info);

    // 取 BSS 列表
    DOT11_SSID* pSsid = curSsid.uSSIDLength ? &curSsid : nullptr;
    PWLAN_BSS_LIST list = nullptr;
    DWORD rc = WlanGetNetworkBssList(g_hClient, &g_ifGuid, pSsid, dot11_BSS_type_infrastructure,
                                     FALSE, nullptr, &list);
    if (rc != ERROR_SUCCESS || !list) return;

    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_BSS_ENTRY& e = list->wlanBssEntries[i];

        BssRow r;
        r.ssid    = SsidToText(e.dot11Ssid);
        if (curSsid.uSSIDLength &&
            (e.dot11Ssid.uSSIDLength != curSsid.uSSIDLength ||
             memcmp(e.dot11Ssid.ucSSID, curSsid.ucSSID, curSsid.uSSIDLength) != 0))
            continue;

        r.freq    = e.ulChCenterFrequency;
        r.channel = ChannelFromFreq(r.freq);
        r.rssi    = e.lRssi;
        r.quality = e.uLinkQuality;
        r.dfs     = IsDfsChannel(r.channel, r.freq);
        r.nonDfs5G = IsNonDfs5G(r.channel, r.freq);
        memcpy(r.mac, e.dot11Bssid, 6);
        r.bssid   = FormatBssid(e.dot11Bssid);
        r.current = (memcmp(e.dot11Bssid, curBssid, 6) == 0);
        g_rows.push_back(r);
    }
    WlanFreeMemory(list);

    // 按 BSSID 去重
    std::vector<BssRow> uniq;
    for (auto& r : g_rows) {
        bool dup = false;
        for (auto& u : uniq) if (u.bssid == r.bssid) { dup = true; break; }
        if (!dup) uniq.push_back(r);
    }
    g_rows.swap(uniq);

    if (!g_hList) return;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const auto& r = g_rows[i];
        wchar_t chbuf[16]; swprintf_s(chbuf, L"%d", r.channel);
        wchar_t qbuf[16];  swprintf_s(qbuf, L"%u%%", r.quality);

        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.iSubItem = 0;
        it.pszText = (LPWSTR)r.ssid.c_str();
        it.lParam = (LPARAM)i;
        int idx = ListView_InsertItem(g_hList, &it);

        ListView_SetItemText(g_hList, idx, 1, chbuf);
        ListView_SetItemText(g_hList, idx, 2, (LPWSTR)BandName(r.freq));
        ListView_SetItemText(g_hList, idx, 3, (LPWSTR)(r.dfs ? L"不可用（DFS）" : L"可用"));
        ListView_SetItemText(g_hList, idx, 4, qbuf);
        ListView_SetItemText(g_hList, idx, 5, (LPWSTR)r.bssid.c_str());
        ListView_SetItemText(g_hList, idx, 6, (LPWSTR)(r.current ? L"正在使用" : L""));
    }
}

static int GetSelectedRow()
{
    if (!g_hList) return -1;
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0 || (size_t)sel >= g_rows.size()) return -1;
    return sel;
}

static bool ConnectToBssid(const BssRow& r)
{
    if (!WlanInit()) return false;

    std::wstring profile;
    if (!FindProfileForSsid(r.ssid, profile)) {
        SetStatus(L"找不到这个网络的配置，请先在系统里连一次。");
        return false;
    }

    DOT11_SSID ssid = {};
    ssid.uSSIDLength = (ULONG)r.ssid.size();
    if (ssid.uSSIDLength > 32) ssid.uSSIDLength = 32;
    if (g_ssid.uSSIDLength) ssid = g_ssid;
    else memcpy(ssid.ucSSID, r.ssid.c_str(), ssid.uSSIDLength);

    DOT11_BSSID_LIST bl = {};
    bl.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    bl.Header.Revision = DOT11_BSSID_LIST_REVISION_1;
    bl.Header.Size = sizeof(DOT11_BSSID_LIST);
    bl.uNumOfEntries = 1;
    bl.uTotalNumOfEntries = 1;
    memcpy(bl.BSSIDs, r.mac, 6);

    WLAN_CONNECTION_PARAMETERS cp = {};
    cp.wlanConnectionMode = wlan_connection_mode_profile;
    cp.strProfile = profile.c_str();
    cp.pDot11Ssid = &ssid;
    cp.pDesiredBssidList = &bl;
    cp.dot11BssType = dot11_BSS_type_infrastructure;
    cp.dwFlags = 0;

    DWORD rc = WlanConnect(g_hClient, &g_ifGuid, &cp, nullptr);
    if (rc != ERROR_SUCCESS) {
        wchar_t msg[256];
        swprintf_s(msg, L"切换失败，错误 %lu。", rc);
        SetStatus(msg);
        return false;
    }

    SetStatus(L"切换中…");
    for (int i = 0; i < 15; ++i) {
        Sleep(700);
        BYTE cur[6] = {};
        int ch = 0; ULONG freq = 0;
        if (QueryCurrent(nullptr, cur, &ch, &freq, nullptr) && memcmp(cur, r.mac, 6) == 0) {
            wchar_t msg[256];
            swprintf_s(msg, L"已切换到 %s · 信道 %d（%s）%s",
                       r.bssid.c_str(), ch, BandName(freq),
                       IsDfsChannel(ch, freq) ? L"，仍是 DFS，热点还是起不来" : L"，现在可以开 5 GHz 热点了");
            SetStatus(msg);
            PopulateList(false);
            return true;
        }
    }
    SetStatus(L"切换超时，目标可能已不可见，刷新后再试。");
    PopulateList(false);
    return false;
}

static void AutoPickNonDfs()
{
    if (g_rows.empty()) { SetStatus(L"先点「刷新」。"); return; }

    int best = -1; ULONG bestQ = 0;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        if (!g_rows[i].nonDfs5G) continue;
        if (g_rows[i].current) continue;
        if (g_rows[i].quality > bestQ) { bestQ = g_rows[i].quality; best = (int)i; }
    }
    if (best < 0) {
        SetStatus(L"没有可切换的可用信道。");
        return;
    }
    if (g_hList) {
        ListView_SetItemState(g_hList, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(g_hList, best, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_hList, best, FALSE);
    }
    ConnectToBssid(g_rows[best]);
}

// ---------------- 列表自绘：DFS 行标灰 ----------------
static LRESULT HandleCustomDraw(LPNMLVCUSTOMDRAW cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        size_t idx = (size_t)cd->nmcd.dwItemSpec;
        if (idx < g_rows.size() && g_rows[idx].dfs) {
            cd->clrText = RGB(150, 150, 150);
        } else if (idx < g_rows.size() && g_rows[idx].current) {
            cd->clrText = RGB(0, 100, 0);
        }
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

// ---------------- 窗口过程 ----------------
static void CreateChildControls(HWND hwnd)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

    g_hInfo = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              12, 8, 900, 54, hwnd, (HMENU)MYIDC_INFO, hInst, nullptr);

    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              12, 66, 900, 300, hwnd, (HMENU)MYIDC_LIST, hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    struct { const wchar_t* t; int w; } cols[] = {
        { L"网络", 190 }, { L"信道", 56 }, { L"频段", 56 }, { L"热点", 112 },
        { L"信号", 60 },  { L"BSSID", 150 }, { L"状态", 80 }
    };
    for (int i = 0; i < 7; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = (LPWSTR)cols[i].t;
        c.cx = cols[i].w;
        c.iSubItem = i;
        ListView_InsertColumn(g_hList, i, &c);
    }

    CreateWindowExW(0, L"BUTTON", L"刷新 (F5)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    12, 388, 120, 30, hwnd, (HMENU)MYIDC_REFRESH, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"切换到选中", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                    140, 388, 130, 30, hwnd, (HMENU)MYIDC_PIN, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"自动选可用", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    278, 388, 130, 30, hwnd, (HMENU)MYIDC_AUTO, hInst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"热点设置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    416, 388, 110, 30, hwnd, (HMENU)MYIDC_HOTSPOT, hInst, nullptr);

    g_hStatus = CreateWindowExW(0, L"STATIC", L"正在扫描…", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                12, 428, 900, 32, hwnd, (HMENU)MYIDC_STATUS, hInst, nullptr);

    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    for (HWND h : { g_hInfo, g_hList, g_hStatus }) SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    EnumChildWindows(hwnd, [](HWND h, LPARAM p) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        (void)p; return TRUE;
    }, 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateChildControls(hwnd);
        SetTimer(hwnd, 1, 300, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            ScanAndRefresh(hwnd);
        }
        return 0;

    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (g_hInfo)  MoveWindow(g_hInfo, 12, 8, w - 24, 54, TRUE);
        if (g_hList)  MoveWindow(g_hList, 12, 66, w - 24, h - 66 - 92, TRUE);
        HWND b1 = GetDlgItem(hwnd, MYIDC_REFRESH);
        HWND b2 = GetDlgItem(hwnd, MYIDC_PIN);
        HWND b3 = GetDlgItem(hwnd, MYIDC_AUTO);
        HWND b4 = GetDlgItem(hwnd, MYIDC_HOTSPOT);
        int by = h - 84;
        MoveWindow(b1, 12, by, 120, 30, TRUE);
        MoveWindow(b2, 140, by, 130, 30, TRUE);
        MoveWindow(b3, 278, by, 130, 30, TRUE);
        MoveWindow(b4, 416, by, 110, 30, TRUE);
        if (g_hStatus) MoveWindow(g_hStatus, 12, h - 42, w - 24, 32, TRUE);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MYIDC_REFRESH:  ScanAndRefresh(hwnd); return 0;
        case MYIDC_PIN: {
            int sel = GetSelectedRow();
            if (sel < 0) { SetStatus(L"先选中一行。"); return 0; }
            if (g_rows[sel].dfs)
                SetStatus(L"这是 DFS 信道，切过去热点仍然起不来。");
            ConnectToBssid(g_rows[sel]);
            return 0;
        }
        case MYIDC_AUTO:   AutoPickNonDfs(); return 0;
        case MYIDC_HOTSPOT:
            ShellExecuteW(hwnd, L"open", L"ms-settings:network-mobilehotspot", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == MYIDC_LIST && nh->code == NM_CUSTOMDRAW)
            return HandleCustomDraw((LPNMLVCUSTOMDRAW)lp);
        if (nh->idFrom == MYIDC_LIST && nh->code == NM_DBLCLK) {
            int sel = GetSelectedRow();
            if (sel >= 0) ConnectToBssid(g_rows[sel]);
            return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wp == VK_F5) { ScanAndRefresh(hwnd); return 0; }
        break;

    case WM_DESTROY:
        WlanUninit();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 无界面自检（--selftest 输出到 selftest.txt） ----------------
static void RunSelfTest(const std::wstring& outPath)
{
    std::wstring rep;
    wchar_t buf[1024];

    rep += L"=== WLAN 自检 ===\r\n";
    if (!WlanInit()) {
        rep += L"WlanInit 失败\r\n";
    } else {
        swprintf_s(buf, L"接口: %s\r\n", g_ifDesc.c_str());
        rep += buf;

        DOT11_SSID ssid = {};
        BYTE bssid[6] = {};
        int ch = 0; ULONG freq = 0; LONG q = 0;
        if (QueryCurrent(&ssid, bssid, &ch, &freq, &q)) {
            swprintf_s(buf, L"当前: SSID=%s 信道=%d 频段=%s DFS=%s 质量=%ld%% BSSID=%s\r\n",
                       SsidToText(ssid).c_str(), ch, BandName(freq),
                       IsDfsChannel(ch, freq) ? L"是" : L"否", q, FormatBssid(bssid).c_str());
            rep += buf;
        } else {
            rep += L"当前未连接\r\n";
        }

        g_ssid = ssid;
        PopulateList(false);

        swprintf_s(buf, L"发现的 BSS 数量: %zu\r\n", g_rows.size());
        rep += buf;        for (const auto& r : g_rows) {
            swprintf_s(buf, L"  %-16s ch=%-4d %-4s DFS=%-3s 质量=%3lu%% RSSI=%-4ld %s%s%s\r\n",
                       r.ssid.c_str(), r.channel, BandName(r.freq),
                       r.dfs ? L"是" : L"否", r.quality, r.rssi, r.bssid.c_str(),
                       r.current ? L"  ★当前" : L"",
                       r.nonDfs5G ? L"  推荐" : L"");
            rep += buf;
        }

        // 模拟 GUI 刷新路径：先扫描，再查当前连接
        WlanScan(g_hClient, &g_ifGuid, nullptr, nullptr, nullptr);
        Sleep(4000);
        DOT11_SSID ssid2 = {}; BYTE b2[6] = {}; int ch2 = 0; ULONG f2 = 0; LONG q2 = 0;
        bool ok2 = QueryCurrentOnce(&ssid2, b2, &ch2, &f2, &q2);
        swprintf_s(buf, L"扫描后复查: ok=%d rc=%lu ch=%d bssid=%s\r\n",
                   ok2 ? 1 : 0, g_rcCurrent, ch2, FormatBssid(b2).c_str());
        rep += buf;
    }

    // 写出 UTF-8
    int n = WideCharToMultiByte(CP_UTF8, 0, rep.c_str(), (int)rep.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, rep.c_str(), (int)rep.size(), &utf8[0], n, nullptr, nullptr);

    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);
    }
    WlanUninit();
}

// ---------------- 命令行固定 BSSID（--pin XX:XX:XX:XX:XX:XX） ----------------
static bool ParseMac(const wchar_t* s, BYTE out[6])
{
    int v[6] = {};
    if (swscanf_s(s, L"%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; ++i) out[i] = (BYTE)v[i];
    return true;
}

static void RunPin(const std::wstring& macText, const std::wstring& outPath)
{
    std::wstring rep;
    wchar_t buf[512];

    if (!WlanInit()) { rep = L"WlanInit 失败\r\n"; }
    else {
        DOT11_SSID ssid = {};
        BYTE cur[6] = {};
        int ch = 0; ULONG freq = 0; LONG q = 0;
        if (!QueryCurrent(&ssid, cur, &ch, &freq, &q)) {
            swprintf_s(buf, L"无法读取当前连接 rc=%lu\r\n", g_rcCurrent);
            rep = buf;
        } else {
            g_ssid = ssid;
            BssRow r;
            r.ssid = SsidToText(ssid);
            if (!ParseMac(macText.c_str(), r.mac)) { rep = L"BSSID 解析失败\r\n"; }
            else {
                r.bssid = FormatBssid(r.mac);
                swprintf_s(buf, L"目标 BSSID: %s（当前 ch=%d）\r\n", r.bssid.c_str(), ch);
                rep = buf;
                bool ok = ConnectToBssid(r);
                int ch2 = 0; ULONG f2 = 0;
                QueryCurrent(nullptr, nullptr, &ch2, &f2, nullptr);
                swprintf_s(buf, L"pin ok=%d 结果信道=%d 频段=%s\r\n", ok ? 1 : 0, ch2, BandName(f2));
                rep += buf;
            }
        }
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, rep.c_str(), (int)rep.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, rep.c_str(), (int)rep.size(), &utf8[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(h);
    }
    WlanUninit();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (lpCmdLine && wcsstr(lpCmdLine, L"--selftest")) {
        RunSelfTest(L"selftest.txt");
        return 0;
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--pin")) {
        const wchar_t* p = wcsstr(lpCmdLine, L"--pin") + 5;
        while (*p == L' ' || *p == L'=') ++p;
        RunPin(p, L"pin.txt");
        return 0;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WifiChannelToolWnd";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wi-Fi 信道切换",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 980, 520,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_F5 && IsWindow(hwnd)) {
            SendMessageW(hwnd, WM_COMMAND, MYIDC_REFRESH, 0);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
