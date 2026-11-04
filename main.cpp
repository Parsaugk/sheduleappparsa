// main.cpp - Schedule & Progress Tracker (pure Win32, Unicode, x64)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "resource.h"
#include "Task.h"

// Common Controls v6 (visual styles) without needing a .manifest/.rc file.
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
    const wchar_t  kClassName[] = L"ScheduleProgressTrackerWindow";
    const wchar_t  kAppTitle[]  = L"Schedule & Progress Tracker";
    const UINT_PTR kTimerId     = 1;

    struct AppState
    {
        HWND hwnd = nullptr;
        HWND lblTitle = nullptr, lblDue = nullptr, lblPct = nullptr;
        HWND edtTitle = nullptr, dtp = nullptr, track = nullptr, edtPct = nullptr;
        HWND btnAdd = nullptr, btnUpdate = nullptr, btnDelete = nullptr, btnComplete = nullptr;
        HWND list = nullptr;
        HWND lblSel = nullptr, progSel = nullptr, lblAll = nullptr, progAll = nullptr;
        HFONT font = nullptr;

        std::vector<Task> tasks;
        std::wstring dataPath;

        bool suppressListNotify = false; // true while we rebuild the ListView
        bool syncingPercent = false;     // true while we update trackbar/edit programmatically
    } g;

    // ---------------------------------------------------------------- helpers
    std::wstring GetText(HWND h)
    {
        int len = GetWindowTextLengthW(h);
        std::wstring s((size_t)len + 1, L'\0');
        GetWindowTextW(h, &s[0], len + 1);
        s.resize((size_t)len);
        return s;
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && iswspace(s[b])) ++b;
        while (e > b && iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    // Returns false if text is not an integer. Out-of-range values are clamped.
    bool ParseProgress(const std::wstring& text, int& out)
    {
        std::wstring s = Trim(text);
        if (s.empty()) return false;
        wchar_t* end = nullptr;
        long v = wcstol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != L'\0') return false;
        out = ClampProgress(v);
        return true;
    }

    int GetSelectedIndex()
    {
        int idx = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
        return (idx >= 0 && idx < (int)g.tasks.size()) ? idx : -1;
    }

    void SetPercentControls(int p)
    {
        g.syncingPercent = true;
        SendMessageW(g.track, TBM_SETPOS, TRUE, p);
        wchar_t buf[16];
        swprintf_s(buf, L"%d", p);
        SetWindowTextW(g.edtPct, buf);
        g.syncingPercent = false;
    }

    void SetDueControl(const SYSTEMTIME& st)
    {
        DateTime_SetSystemtime(g.dtp, GDT_VALID, &st);
    }

    void ResetInputs()
    {
        SetWindowTextW(g.edtTitle, L"");
        SYSTEMTIME now;
        GetLocalTime(&now);
        now.wSecond = 0;
        now.wMilliseconds = 0;
        SetDueControl(now);
        SetPercentControls(0);
    }

    void PersistOrWarn()
    {
        if (!SaveTasks(g.tasks, g.dataPath))
        {
            std::wstring msg = L"Could not save tasks to:\n" + g.dataPath;
            MessageBoxW(g.hwnd, msg.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
        }
    }

    // ------------------------------------------------------------- UI updates
    void UpdateProgressUI()
    {
        int idx = GetSelectedIndex();
        int selPct = (idx >= 0) ? g.tasks[idx].progress : 0;
        SendMessageW(g.progSel, PBM_SETPOS, selPct, 0);

        wchar_t buf[160];
        if (idx >= 0) swprintf_s(buf, L"Selected task: %d%%", selPct);
        else          swprintf_s(buf, L"Selected task: (none)");
        SetWindowTextW(g.lblSel, buf);

        int n = (int)g.tasks.size(), sum = 0, done = 0;
        for (const Task& t : g.tasks) { sum += t.progress; if (t.progress >= 100) ++done; }
        int avg = (n > 0) ? sum / n : 0;
        SendMessageW(g.progAll, PBM_SETPOS, avg, 0);
        swprintf_s(buf, L"Overall: %d%% (%d of %d complete)", avg, done, n);
        SetWindowTextW(g.lblAll, buf);
    }

    // Rebuilds the ListView. Does NOT touch the input controls.
    void RefreshList(int selectIndex)
    {
        g.suppressListNotify = true;
        ListView_DeleteAllItems(g.list);

        for (size_t i = 0; i < g.tasks.size(); ++i)
        {
            const Task& t = g.tasks[i];

            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = (int)i;
            it.iSubItem = 0;
            it.pszText = const_cast<LPWSTR>(t.title.c_str());
            ListView_InsertItem(g.list, &it);

            std::wstring due = FormatDue(t.due);
            std::wstring pct = std::to_wstring(t.progress) + L"%";
            ListView_SetItemText(g.list, (int)i, 1, const_cast<LPWSTR>(due.c_str()));
            ListView_SetItemText(g.list, (int)i, 2, const_cast<LPWSTR>(pct.c_str()));
            ListView_SetItemText(g.list, (int)i, 3, const_cast<LPWSTR>(StatusText(ComputeStatus(t))));
        }

        if (selectIndex >= 0 && selectIndex < (int)g.tasks.size())
        {
            ListView_SetItemState(g.list, selectIndex, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g.list, selectIndex, FALSE);
        }
        g.suppressListNotify = false;

        UpdateProgressUI();
    }

    void LoadSelectedIntoControls()
    {
        int idx = GetSelectedIndex();
        if (idx < 0) return;
        const Task& t = g.tasks[idx];
        SetWindowTextW(g.edtTitle, t.title.c_str());
        SetDueControl(t.due);
        SetPercentControls(t.progress);
    }

    // ----------------------------------------------------------------- input
    // Validates the input controls. Shows a MessageBox and returns false on bad input.
    bool ReadInputs(Task& out)
    {
        std::wstring title = Trim(GetText(g.edtTitle));
        if (title.empty())
        {
            MessageBoxW(g.hwnd, L"Please enter a task title.", kAppTitle, MB_OK | MB_ICONWARNING);
            SetFocus(g.edtTitle);
            return false;
        }

        int pct = 0;
        if (!ParseProgress(GetText(g.edtPct), pct))
        {
            MessageBoxW(g.hwnd, L"Progress must be a whole number from 0 to 100.",
                        kAppTitle, MB_OK | MB_ICONWARNING);
            SetFocus(g.edtPct);
            return false;
        }
        SetPercentControls(pct); // shows the clamped value

        SYSTEMTIME st{};
        if (DateTime_GetSystemtime(g.dtp, &st) != GDT_VALID)
        {
            MessageBoxW(g.hwnd, L"Please choose a valid due date.", kAppTitle, MB_OK | MB_ICONWARNING);
            SetFocus(g.dtp);
            return false;
        }
        st.wSecond = 0;
        st.wMilliseconds = 0;

        out.title = title;
        out.due = st;
        out.progress = pct;
        return true;
    }

    bool RequireSelection(int& idx)
    {
        idx = GetSelectedIndex();
        if (idx < 0)
        {
            MessageBoxW(g.hwnd, L"Please select a task in the list first.",
                        kAppTitle, MB_OK | MB_ICONINFORMATION);
            return false;
        }
        return true;
    }

    // --------------------------------------------------------------- actions
    void OnAdd()
    {
        Task t;
        if (!ReadInputs(t)) return;
        g.tasks.push_back(t);
        PersistOrWarn();
        RefreshList((int)g.tasks.size() - 1);
    }

    void OnUpdate()
    {
        int idx;
        if (!RequireSelection(idx)) return;
        Task t;
        if (!ReadInputs(t)) return;
        g.tasks[idx] = t;
        PersistOrWarn();
        RefreshList(idx);
    }

    void OnDelete()
    {
        int idx;
        if (!RequireSelection(idx)) return;
        if (MessageBoxW(g.hwnd, L"Delete the selected task?", kAppTitle,
                        MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        g.tasks.erase(g.tasks.begin() + idx);
        PersistOrWarn();
        RefreshList(-1);
        ResetInputs();
    }

    void OnComplete()
    {
        int idx;
        if (!RequireSelection(idx)) return;
        g.tasks[idx].progress = 100;
        PersistOrWarn();
        RefreshList(idx);
        LoadSelectedIntoControls();
    }

    void OnPercentEdited()
    {
        if (g.syncingPercent) return;
        int v;
        if (ParseProgress(GetText(g.edtPct), v))
            SendMessageW(g.track, TBM_SETPOS, TRUE, v);
    }

    // ---------------------------------------------------------------- layout
    void LayoutControls(int w, int h)
    {
        if (!g.list) return;

        const int M = 10, G = 8, LBL_W = 72, ROW_H = 24;
        int y = M;

        // Row 1: title
        MoveWindow(g.lblTitle, M, y + 4, LBL_W, 18, TRUE);
        MoveWindow(g.edtTitle, M + LBL_W, y, std::max(50, w - 2 * M - LBL_W), ROW_H, TRUE);
        y += ROW_H + G;

        // Row 2: due date, progress trackbar + edit
        const int dtpX = M + LBL_W, dtpW = 190;
        const int pLblX = dtpX + dtpW + 20, pLblW = 72;
        const int trackX = pLblX + pLblW;
        const int pctW = 50;
        const int pctX = std::max(trackX + 60 + G, w - M - pctW);

        MoveWindow(g.lblDue, M, y + 4, LBL_W, 18, TRUE);
        MoveWindow(g.dtp, dtpX, y + 2, dtpW, ROW_H, TRUE);
        MoveWindow(g.lblPct, pLblX, y + 4, pLblW, 18, TRUE);
        MoveWindow(g.track, trackX, y, pctX - G - trackX, 32, TRUE);
        MoveWindow(g.edtPct, pctX, y + 2, pctW, ROW_H, TRUE);
        y += 32 + G;

        // Row 3: buttons
        HWND btns[] = { g.btnAdd, g.btnUpdate, g.btnDelete, g.btnComplete };
        const int btnW = 125, btnH = 28;
        for (int i = 0; i < 4; ++i)
            MoveWindow(btns[i], M + i * (btnW + G), y, btnW, btnH, TRUE);
        y += btnH + G;

        // Bottom: two progress rows
        const int rowH = 22;
        const int yb2 = h - M - rowH;
        const int yb1 = yb2 - rowH - 6;
        const int lblW = 300;
        const int barX = M + lblW + G;
        const int barW = std::max(50, w - M - barX);

        MoveWindow(g.lblSel, M, yb1 + 3, lblW, 18, TRUE);
        MoveWindow(g.progSel, barX, yb1, barW, rowH, TRUE);
        MoveWindow(g.lblAll, M, yb2 + 3, lblW, 18, TRUE);
        MoveWindow(g.progAll, barX, yb2, barW, rowH, TRUE);

        // ListView fills the middle
        int listH = std::max(60, yb1 - G - y);
        MoveWindow(g.list, M, y, std::max(100, w - 2 * M), listH, TRUE);

        // Task-title column absorbs the extra width.
        RECT rc;
        GetClientRect(g.list, &rc);
        int fixed = 150 + 90 + 110;
        ListView_SetColumnWidth(g.list, 0, std::max(150, (int)(rc.right - rc.left) - fixed));
    }

    BOOL CALLBACK SetFontProc(HWND child, LPARAM font)
    {
        SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        return TRUE;
    }

    void CreateControls(HWND hwnd, HINSTANCE hInst)
    {
        auto make = [&](DWORD ex, LPCWSTR cls, LPCWSTR text, DWORD style, int id) -> HWND
        {
            return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        };

        g.lblTitle = make(0, L"STATIC", L"Task title:", SS_LEFT, IDC_LABEL);
        g.edtTitle = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_EDIT_TITLE);
        SendMessageW(g.edtTitle, EM_LIMITTEXT, 200, 0);

        g.lblDue = make(0, L"STATIC", L"Due date:", SS_LEFT, IDC_LABEL);
        g.dtp = make(0, DATETIMEPICK_CLASSW, L"", WS_TABSTOP, IDC_DTP_DUE);
        DateTime_SetFormat(g.dtp, L"yyyy-MM-dd  HH:mm");

        g.lblPct = make(0, L"STATIC", L"Progress %:", SS_LEFT, IDC_LABEL);
        g.track = make(0, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_AUTOTICKS | TBS_TOOLTIPS, IDC_TRACK_PROGRESS);
        SendMessageW(g.track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(g.track, TBM_SETTICFREQ, 10, 0);
        SendMessageW(g.track, TBM_SETPAGESIZE, 0, 10);
        g.edtPct = make(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL | ES_RIGHT, IDC_EDIT_PERCENT);
        SendMessageW(g.edtPct, EM_LIMITTEXT, 3, 0);

        g.btnAdd      = make(0, L"BUTTON", L"Add Task",        WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_ADD);
        g.btnUpdate   = make(0, L"BUTTON", L"Update Selected", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_UPDATE);
        g.btnDelete   = make(0, L"BUTTON", L"Delete Selected", WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_DELETE);
        g.btnComplete = make(0, L"BUTTON", L"Mark Complete",   WS_TABSTOP | BS_PUSHBUTTON, IDC_BTN_COMPLETE);

        g.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                      WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, IDC_LIST_TASKS);
        ListView_SetExtendedListViewStyle(g.list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        const wchar_t* names[] = { L"Task title", L"Due date", L"Progress %", L"Status" };
        const int widths[]     = { 300, 150, 90, 110 };
        for (int i = 0; i < 4; ++i)
        {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
            col.fmt = LVCFMT_LEFT;
            col.cx = widths[i];
            col.iSubItem = i;
            col.pszText = const_cast<LPWSTR>(names[i]);
            ListView_InsertColumn(g.list, i, &col);
        }

        g.lblSel = make(0, L"STATIC", L"Selected task: (none)", SS_LEFT, IDC_LABEL);
        g.progSel = make(0, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROG_SELECTED);
        SendMessageW(g.progSel, PBM_SETRANGE32, 0, 100);

        g.lblAll = make(0, L"STATIC", L"Overall: 0%", SS_LEFT, IDC_LABEL);
        g.progAll = make(0, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROG_OVERALL);
        SendMessageW(g.progAll, PBM_SETRANGE32, 0, 100);

        // Use the standard UI font on every control.
        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g.font = CreateFontIndirectW(&ncm.lfMessageFont);
        EnumChildWindows(hwnd, SetFontProc, (LPARAM)g.font);
    }

    // ------------------------------------------------------------- window proc
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            g.hwnd = hwnd;
            CreateControls(hwnd, ((LPCREATESTRUCTW)lp)->hInstance);

            RECT rc;
            GetClientRect(hwnd, &rc);
            LayoutControls(rc.right, rc.bottom);

            g.dataPath = GetDataFilePath();
            if (!LoadTasks(g.tasks, g.dataPath))
            {
                std::wstring m = L"Could not read the task file:\n" + g.dataPath;
                MessageBoxW(hwnd, m.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
            }
            ResetInputs();
            RefreshList(-1);
            SetTimer(hwnd, kTimerId, 60 * 1000, nullptr); // refresh "Overdue" status as time passes
            return 0;
        }

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED)
                LayoutControls(LOWORD(lp), HIWORD(lp));
            return 0;

        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 720;
            mmi->ptMinTrackSize.y = 480;
            return 0;
        }

        case WM_TIMER:
            if (wp == kTimerId) RefreshList(GetSelectedIndex());
            return 0;

        case WM_HSCROLL:
            if ((HWND)lp == g.track)
            {
                int pos = (int)SendMessageW(g.track, TBM_GETPOS, 0, 0);
                SetPercentControls(pos);
            }
            return 0;

        case WM_COMMAND:
        {
            WORD id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_EDIT_PERCENT && code == EN_CHANGE)
            {
                OnPercentEdited();
            }
            else if (code == BN_CLICKED)
            {
                switch (id)
                {
                case IDC_BTN_ADD:      OnAdd();      break;
                case IDC_BTN_UPDATE:   OnUpdate();   break;
                case IDC_BTN_DELETE:   OnDelete();   break;
                case IDC_BTN_COMPLETE: OnComplete(); break;
                default: break; // IDOK / IDCANCEL from IsDialogMessage are ignored
                }
            }
            return 0;
        }

        case WM_NOTIFY:
        {
            NMHDR* hdr = (NMHDR*)lp;
            if (hdr->idFrom == IDC_LIST_TASKS && hdr->code == LVN_ITEMCHANGED && !g.suppressListNotify)
            {
                NMLISTVIEW* nm = (NMLISTVIEW*)lp;
                if ((nm->uChanged & LVIF_STATE) && ((nm->uOldState ^ nm->uNewState) & LVIS_SELECTED))
                {
                    LoadSelectedIntoControls(); // no-op when nothing is selected
                    UpdateProgressUI();
                }
            }
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            SaveTasks(g.tasks, g.dataPath); // final save on exit
            if (g.font) DeleteObject(g.font);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES | ICC_BAR_CLASSES |
                ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kClassName, kAppTitle,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 880, 620,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg)) // Tab / arrow-key navigation between controls
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
