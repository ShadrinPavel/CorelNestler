#define _CRT_SECURE_NO_WARNINGS 1
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#undef GetFirstChild
#undef GetNextSibling
#undef GetPrevSibling
#undef GetLastChild
#undef GetFirstSibling
#undef GetLastSibling

#include <commctrl.h>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <random>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include "clipper2/clipper.h"
#include "resource.h"
#include "svg_logo.h"

#import "VGCoreAuto.tlb" no_namespace named_guids \
    rename("GetCommandLine", "VGGetCommandLine") \
    rename("CopyFile", "VGCopyFile") \
    rename("FindWindow", "VGFindWindow") \
    rename("DrawText", "VGDrawText") \
    rename("ReplaceText", "VGReplaceText") \
    rename("GetUserName", "VGGetUserName")

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

#define WM_USER_MATH_DONE  (WM_USER + 1)
#define WM_USER_PASS_START (WM_USER + 2)
#define WM_USER_PASS_END   (WM_USER + 3)
#define TIMER_ID 1001

const double SCALE = 10.0; 
HWND g_hMainWnd = NULL;
HBRUSH g_hBrushBg = NULL;
HBRUSH g_hBrushWhite = NULL;
HFONT g_hFontReg = NULL;
HFONT g_hFontBold = NULL;
HFONT g_hFontHuge = NULL;
HANDLE g_hFontMemReg = NULL;
HANDLE g_hFontMemBold = NULL;

std::vector<HWND> g_StaticLabels;

enum AppState { STATE_READY, STATE_RUNNING, STATE_DONE };
std::atomic<AppState> g_AppState{STATE_READY};

struct ShapeData {
    IVGShapePtr shape;
    double orig_area;
    double ref_cx, ref_cy;
    Clipper2Lib::Paths64 orig_paths;
};

struct ShapeResult {
    bool placed = false;
    double target_x = 0, target_y = 0;
    double math_cx_offset = 0, math_cy_offset = 0;
    int angle = 0;
};

struct PassResult {
    int placed_count = 0;
    double max_x = 0;
    std::vector<ShapeResult> results;
};

struct PlacedShape {
    double min_x, max_x, min_y, max_y;
    Clipper2Lib::Paths64 inflated_paths;
};

std::vector<ShapeData> g_Shapes;
std::vector<PassResult> g_AllPasses;
int g_TargetPasses = 40;
double g_TargetMargin = 2.0;
double g_PageW = 0, g_PageH = 0;
double g_PageLeft = 0, g_PageBottom = 0;

std::atomic<int> g_PassesStarted{0};
std::atomic<int> g_PassesCompleted{0};
std::atomic<int> g_ProgressPercent{0};
std::atomic<bool> is_running{false};
ULONGLONG g_StartTime = 0;

std::mutex g_SeqMutex;
std::set<std::vector<int>> g_GeneratedSequences;
std::atomic<bool> g_IsExhausted{false};

HANDLE LoadFontFromResource(int resId) {
    HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) return NULL;
    HGLOBAL hMem = LoadResource(GetModuleHandleW(NULL), hRes);
    if (!hMem) return NULL;
    void* pData = LockResource(hMem);
    DWORD len = SizeofResource(GetModuleHandleW(NULL), hRes);
    DWORD numFonts = 0;
    return AddFontMemResourceEx(pData, len, NULL, &numFonts);
}

void SaveSettings() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(NULL, iniPath, MAX_PATH);
    wchar_t* dot = wcsrchr(iniPath, L'.');
    if (dot) wcscpy(dot, L".ini");
    WritePrivateProfileStringW(L"Settings", L"Margin", std::to_wstring((int)g_TargetMargin).c_str(), iniPath);
    WritePrivateProfileStringW(L"Settings", L"Passes", std::to_wstring(g_TargetPasses).c_str(), iniPath);
}

void LoadSettings(HWND hDlg) {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(NULL, iniPath, MAX_PATH);
    wchar_t* dot = wcsrchr(iniPath, L'.');
    if (dot) wcscpy(dot, L".ini");
    int m = GetPrivateProfileIntW(L"Settings", L"Margin", 140, iniPath);
    int p = GetPrivateProfileIntW(L"Settings", L"Passes", 40, iniPath);
    SetDlgItemInt(hDlg, IDC_MARGIN_EDIT, m, FALSE);
    SetDlgItemInt(hDlg, IDC_PASSES_EDIT, p, FALSE);
}

void LogMessage(HWND hDlg, const wchar_t* msg) {
    HWND hList = GetDlgItem(hDlg, IDC_LOG_LIST);
    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)msg);
    int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
    SendMessageW(hList, LB_SETCURSEL, count - 1, 0);
    UpdateWindow(hList);
}

float ParseFloat(const char*& ptr) {
    while(*ptr && (isspace(*ptr) || *ptr==',')) ptr++;
    char* end;
    float val = strtof(ptr, &end);
    ptr = end;
    return val;
}

void AddSvgToPath(const char* d, Gdiplus::GraphicsPath& path) {
    const char* ptr = d;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    while(*ptr) {
        while(*ptr && (isspace(*ptr) || *ptr==',')) ptr++;
        if (!*ptr) break;
        if (isalpha(*ptr)) { cmd = *ptr++; }
        bool relative = islower(cmd);
        char ucmd = toupper(cmd);
        if (ucmd == 'M') {
            float x = ParseFloat(ptr), y = ParseFloat(ptr);
            if (relative) { x+=cx; y+=cy; }
            path.StartFigure();
            cx = x; cy = y; sx = x; sy = y;
            cmd = relative ? 'l' : 'L';
        } else if (ucmd == 'L') {
            float x = ParseFloat(ptr), y = ParseFloat(ptr);
            if (relative) { x+=cx; y+=cy; }
            path.AddLine(cx, cy, x, y);
            cx = x; cy = y;
        } else if (ucmd == 'H') {
            float x = ParseFloat(ptr);
            if (relative) x+=cx;
            path.AddLine(cx, cy, x, cy);
            cx = x;
        } else if (ucmd == 'V') {
            float y = ParseFloat(ptr);
            if (relative) y+=cy;
            path.AddLine(cx, cy, cx, y);
            cy = y;
        } else if (ucmd == 'C') {
            float x1 = ParseFloat(ptr), y1 = ParseFloat(ptr);
            float x2 = ParseFloat(ptr), y2 = ParseFloat(ptr);
            float x = ParseFloat(ptr), y = ParseFloat(ptr);
            if (relative) { x1+=cx; y1+=cy; x2+=cx; y2+=cy; x+=cx; y+=cy; }
            path.AddBezier(cx, cy, x1, y1, x2, y2, x, y);
            cx = x; cy = y;
        } else if (ucmd == 'Z') {
            path.CloseFigure();
            cx = sx; cy = sy;
        } else {
            ParseFloat(ptr); 
        }
    }
}

Clipper2Lib::Paths64 RotatePaths(const Clipper2Lib::Paths64& paths, int angle) {
    if (angle == 0) return paths;
    Clipper2Lib::Paths64 res = paths;
    for (auto& path : res) {
        for (auto& pt : path) {
            int64_t x = pt.x, y = pt.y;
            if (angle == 90)  { pt.x = -y; pt.y = x; } 
            else if (angle == 180) { pt.x = -x; pt.y = -y; }
            else if (angle == 270) { pt.x = y; pt.y = -x; }
        }
    }
    return res;
}

__declspec(noinline) bool IsCollisionAndGetJump(double cand_x, double cand_y, double w, double h, const Clipper2Lib::Paths64& norm_paths, const std::vector<PlacedShape>& placed, double pageWidth, double pageHeight, double& jump_y) {
    if (cand_x < 0 || cand_y < 0 || cand_x + w > pageWidth || cand_y + h > pageHeight) return true;
    for (const auto& p : placed) {
        if (cand_x + w <= p.min_x || cand_x >= p.max_x || cand_y + h <= p.min_y || cand_y >= p.max_y) continue;
        int64_t cx64 = static_cast<int64_t>(cand_x * SCALE);
        int64_t cy64 = static_cast<int64_t>(cand_y * SCALE);
        Clipper2Lib::Paths64 cand_paths = Clipper2Lib::TranslatePaths(norm_paths, cx64, cy64);
        if (!Clipper2Lib::Intersect(cand_paths, p.inflated_paths, Clipper2Lib::FillRule::NonZero).empty()) {
            jump_y = p.max_y;
            return true;
        }
    }
    return false;
}

PassResult RunSinglePass(int pass, const std::vector<ShapeData>& shapes, double pageWidth, double pageHeight, double margin, const std::vector<int>& indices) {
    PostMessage(g_hMainWnd, WM_USER_PASS_START, pass + 1, 0);

    PassResult current_pass;
    current_pass.results.resize(shapes.size());
    double pass_max_x = 0;
    std::vector<PlacedShape> placed;

    for (int idx : indices) {
        auto& sd = shapes[idx];
        double absolute_best_score = 1e15;
        double best_target_x = -1, best_target_y = -1;
        int best_angle = 0;
        double best_w = 0;
        double best_math_cx = 0, best_math_cy = 0;
        Clipper2Lib::Paths64 best_paths;

        int angles[] = {0, 180, 90, 270}; 
        for (int angle : angles) {
            Clipper2Lib::Paths64 rot = RotatePaths(sd.orig_paths, angle);
            
            int64_t min_x = 9223372036854775807LL, min_y = 9223372036854775807LL;
            int64_t max_x = -9223372036854775807LL, max_y = -9223372036854775807LL;
            for (const auto& path : rot) {
                for (const auto& pt : path) {
                    if (pt.x < min_x) min_x = pt.x;
                    if (pt.y < min_y) min_y = pt.y;
                    if (pt.x > max_x) max_x = pt.x;
                    if (pt.y > max_y) max_y = pt.y;
                }
            }
            
            double w = (max_x - min_x) / SCALE;
            double h = (max_y - min_y) / SCALE;
            if (w > pageWidth || h > pageHeight) continue; 
            
            Clipper2Lib::Paths64 norm = rot;
            for (auto& path : norm) for (auto& pt : path) { pt.x -= min_x; pt.y -= min_y; }

            double limit_x = pageWidth - w;
            double limit_y = pageHeight - h;
            double max_scan_x = std::min(limit_x, pass_max_x + margin + 10.0); 

            for (double cand_x = 0; cand_x <= max_scan_x + 0.1; cand_x += 2.0) {
                if (cand_x * 100.0 >= absolute_best_score) break; 
                double cand_y = 0;
                while (cand_y <= limit_y + 0.1) {
                    double jump_y = 0;
                    if (!IsCollisionAndGetJump(cand_x, cand_y, w, h, norm, placed, pageWidth, pageHeight, jump_y)) {
                        double fscore = cand_x * 100.0 + cand_y;
                        if (fscore < absolute_best_score) {
                            absolute_best_score = fscore;
                            best_target_x = cand_x; 
                            best_target_y = cand_y;
                            best_angle = angle;
                            best_w = w;
                            
                            double orig_cx = 0.0, orig_cy = 0.0;
                            double rot_cx = orig_cx, rot_cy = orig_cy;
                            if (angle == 90) { rot_cx = -orig_cy; rot_cy = orig_cx; }
                            else if (angle == 180) { rot_cx = -orig_cx; rot_cy = -orig_cy; }
                            else if (angle == 270) { rot_cx = orig_cy; rot_cy = -orig_cx; }
                            
                            best_math_cx = rot_cx - (min_x / SCALE);
                            best_math_cy = rot_cy - (min_y / SCALE);
                            
                            int64_t cx64 = static_cast<int64_t>(cand_x * SCALE);
                            int64_t cy64 = static_cast<int64_t>(cand_y * SCALE);
                            best_paths = Clipper2Lib::TranslatePaths(norm, cx64, cy64);
                        }
                        break;
                    } else {
                        cand_y = std::max(cand_y + 2.0, jump_y);
                    }
                }
            }
        } 

        if (best_target_x != -1) {
            PlacedShape ps;
            ps.inflated_paths = Clipper2Lib::InflatePaths(best_paths, margin * SCALE, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon, 2.0);
            
            int64_t b_min_x = 9223372036854775807LL, b_min_y = 9223372036854775807LL;
            int64_t b_max_x = -9223372036854775807LL, b_max_y = -9223372036854775807LL;
            for (const auto& path : ps.inflated_paths) {
                for (const auto& pt : path) {
                    if (pt.x < b_min_x) b_min_x = pt.x;
                    if (pt.y < b_min_y) b_min_y = pt.y;
                    if (pt.x > b_max_x) b_max_x = pt.x;
                    if (pt.y > b_max_y) b_max_y = pt.y;
                }
            }
            ps.min_x = b_min_x / SCALE;
            ps.max_x = b_max_x / SCALE;
            ps.min_y = b_min_y / SCALE;
            ps.max_y = b_max_y / SCALE;
            placed.push_back(ps);

            current_pass.results[idx].placed = true;
            current_pass.results[idx].target_x = best_target_x;
            current_pass.results[idx].target_y = best_target_y;
            current_pass.results[idx].angle = best_angle;
            current_pass.results[idx].math_cx_offset = best_math_cx;
            current_pass.results[idx].math_cy_offset = best_math_cy;
            current_pass.placed_count++;
            if (best_target_x + best_w > pass_max_x) pass_max_x = best_target_x + best_w;
        }
    }
    current_pass.max_x = pass_max_x;
    PostMessage(g_hMainWnd, WM_USER_PASS_END, pass + 1, current_pass.placed_count);
    return current_pass;
}

void BackgroundMathThread() {
    unsigned int max_cores = std::thread::hardware_concurrency();
    unsigned int allowed_threads = std::max(1u, (unsigned int)(max_cores * 0.8)); 

    std::mutex results_mutex;
    std::vector<std::thread> threads;
    
    for (unsigned int i = 0; i < allowed_threads; ++i) {
        threads.emplace_back([&]() {
            while (g_AppState == STATE_RUNNING && !g_IsExhausted.load()) {
                int pass = g_PassesStarted.fetch_add(1);
                if (pass >= g_TargetPasses) break;
                
                std::vector<int> indices(g_Shapes.size());
                for(size_t j=0; j<indices.size(); ++j) indices[j] = static_cast<int>(j);
                
                bool unique = false;
                int attempts = 0;
                while (!unique && attempts < 1000) {
                    if (pass == 0 && attempts == 0) {
                        std::sort(indices.begin(), indices.end(), [&](int a, int b) { return g_Shapes[a].orig_area > g_Shapes[b].orig_area; });
                    } else {
                        std::mt19937 g(pass * 19937 + attempts);
                        std::shuffle(indices.begin(), indices.end(), g);
                    }
                    std::lock_guard<std::mutex> lk(g_SeqMutex);
                    if (g_GeneratedSequences.find(indices) == g_GeneratedSequences.end()) {
                        g_GeneratedSequences.insert(indices);
                        unique = true;
                    }
                    attempts++;
                }

                if (!unique) {
                    g_IsExhausted = true;
                    LogMessage(g_hMainWnd, L"Все уникальные варианты исчерпаны. Остановка.");
                    break;
                }
                
                PassResult res = RunSinglePass(pass, g_Shapes, g_PageW, g_PageH, g_TargetMargin, indices);
                
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    g_AllPasses.push_back(res);
                }
                g_PassesCompleted++;
            }
        });
    }

    for (auto& t : threads) t.join();
    if (g_AppState == STATE_RUNNING) PostMessage(g_hMainWnd, WM_USER_MATH_DONE, 0, 0);
}

void CreateStaticLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int align, int id = -1) {
    HWND hCtrl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | align, x, y, w, h, parent, (HMENU)(intptr_t)id, NULL, NULL);
    g_StaticLabels.push_back(hCtrl);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g_hMainWnd = hWnd;
            
            CreateStaticLabel(hWnd, L"Укажите", 13, 40, 159, 15, SS_CENTER);
            CreateStaticLabel(hWnd, L"размер отступа", 13, 55, 159, 15, SS_CENTER);
            CreateStaticLabel(hWnd, L"между объектами", 13, 70, 159, 15, SS_CENTER);
            
            CreateStaticLabel(hWnd, L"Укажите", 185, 40, 159, 15, SS_CENTER);
            CreateStaticLabel(hWnd, L"количество вариантов", 185, 55, 159, 15, SS_CENTER);
            CreateStaticLabel(hWnd, L"для компоновки", 185, 70, 159, 15, SS_CENTER);
            
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"140", WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER, 13, 89, 159, 35, hWnd, (HMENU)IDC_MARGIN_EDIT, NULL, NULL);
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"40", WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER, 185, 89, 159, 35, hWnd, (HMENU)IDC_PASSES_EDIT, NULL, NULL);
            
            CreateWindowW(L"BUTTON", L"НАЧАТЬ КОМПОНОВКУ", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 13, 135, 330, 30, hWnd, (HMENU)IDC_START, NULL, NULL);
            
            CreateStaticLabel(hWnd, L"Индикатор прогресса выполнения компоновки", 13, 175, 330, 15, SS_CENTER);
            CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 13, 190, 330, 35, hWnd, (HMENU)IDC_PROGRESS_DRAW, NULL, NULL);
            
            CreateStaticLabel(hWnd, L"Процесс выполнения компоновки", 13, 235, 330, 15, SS_CENTER);
            
            CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 13, 255, 330, 404, hWnd, (HMENU)IDC_LOG_LIST, NULL, NULL);
            
            CreateStaticLabel(hWnd, L"Время работы: 00:00", 13, 670, 150, 15, SS_RIGHT, IDC_TIME_ELAPSED);
            CreateStaticLabel(hWnd, L"Осталось: 00:00", 193, 670, 150, 15, SS_LEFT, IDC_TIME_LEFT);
            
            CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 13, 690, 330, 24, hWnd, (HMENU)IDOK, NULL, NULL);

            for(HWND hc : g_StaticLabels) { SendMessage(hc, WM_SETFONT, (WPARAM)g_hFontReg, TRUE); }
            SendMessage(GetDlgItem(hWnd, IDC_LOG_LIST), WM_SETFONT, (WPARAM)g_hFontReg, TRUE);
            SendMessage(GetDlgItem(hWnd, IDC_MARGIN_EDIT), WM_SETFONT, (WPARAM)g_hFontHuge, TRUE);
            SendMessage(GetDlgItem(hWnd, IDC_PASSES_EDIT), WM_SETFONT, (WPARAM)g_hFontHuge, TRUE);
            
            LoadSettings(hWnd);
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rcWindow; GetWindowRect(hWnd, &rcWindow);
            if (pt.y >= rcWindow.top && pt.y < rcWindow.top + 27) {
                if (pt.x < rcWindow.right - 60) return HTCAPTION;
            }
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.y < 27) {
                RECT rcClient; GetClientRect(hWnd, &rcClient);
                if (pt.x >= rcClient.right - 30) SendMessage(hWnd, WM_COMMAND, IDCANCEL, 0); 
                else if (pt.x >= rcClient.right - 60) ShowWindow(hWnd, SW_MINIMIZE); 
            }
            break;
        }
        case WM_MEASUREITEM: {
            LPMEASUREITEMSTRUCT lpm = (LPMEASUREITEMSTRUCT)lParam;
            if (lpm->CtlID == IDC_LOG_LIST) lpm->itemHeight = 16;
            return TRUE;
        }
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0));
            return (INT_PTR)g_hBrushBg;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
            if (pdis->CtlID == IDC_PROGRESS_DRAW) {
                HDC hdc = pdis->hDC;
                RECT rc = pdis->rcItem;
                FillRect(hdc, &rc, g_hBrushBg);
                
                int total_ticks = 40;
                int active_ticks = (g_ProgressPercent.load() * total_ticks) / 100;
                float start_x = 0, w = 4.0f, gap = 8.25f; 
                
                for (int i = 0; i < total_ticks; ++i) {
                    RECT trc = { (int)(start_x + i * gap), rc.top, (int)(start_x + i * gap + w), rc.bottom };
                    HBRUSH tb;
                    if (i < active_ticks) tb = CreateSolidBrush(RGB(128, 128, 128));
                    else if (i == active_ticks && g_AppState == STATE_RUNNING) tb = CreateSolidBrush(RGB(204, 204, 204));
                    else tb = CreateSolidBrush(RGB(255, 255, 255));
                    FillRect(hdc, &trc, tb);
                    DeleteObject(tb);
                }
                return TRUE;
            }
            if (pdis->CtlID == IDC_LOG_LIST) {
                if (pdis->itemID == -1) return TRUE;
                HDC hdc = pdis->hDC;
                RECT rc = pdis->rcItem;
                FillRect(hdc, &rc, g_hBrushWhite);
                
                wchar_t buf[256];
                SendMessage((HWND)pdis->hwndItem, LB_GETTEXT, pdis->itemID, (LPARAM)buf);
                
                SetBkMode(hdc, TRANSPARENT);
                if (wcsstr(buf, L"успешно завершена")) {
                    SetTextColor(hdc, RGB(255, 128, 0)); 
                } else if (wcsstr(buf, L"Производится")) {
                    SetTextColor(hdc, RGB(128, 128, 128));
                } else {
                    SetTextColor(hdc, RGB(0, 0, 0));
                }
                
                rc.left += 5;
                DrawTextW(hdc, buf, -1, &rc, DT_SINGLELINE | DT_VCENTER);
                return TRUE;
            }
            if (pdis->CtlID == IDC_START) {
                HDC hdc = pdis->hDC;
                RECT rc = pdis->rcItem;
                bool isActive = (g_AppState == STATE_READY);
                
                HBRUSH hBtnBrush = CreateSolidBrush(isActive ? RGB(0, 0, 0) : RGB(128, 128, 128)); 
                FillRect(hdc, &rc, hBtnBrush);
                DeleteObject(hBtnBrush);

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, isActive ? RGB(255, 255, 255) : RGB(200, 200, 200));
                SelectObject(hdc, g_hFontBold);
                wchar_t text[64]; GetWindowTextW(pdis->hwndItem, text, 64);
                DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            if (pdis->CtlID == IDOK) {
                HDC hdc = pdis->hDC;
                RECT rc = pdis->rcItem;
                bool isActive = (g_AppState == STATE_DONE);
                
                HBRUSH hBtnBrush = CreateSolidBrush(isActive ? RGB(0, 0, 0) : RGB(128, 128, 128)); 
                HPEN hBtnPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                SelectObject(hdc, hBtnBrush); SelectObject(hdc, hBtnPen);
                Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
                DeleteObject(hBtnBrush); DeleteObject(hBtnPen);

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, isActive ? RGB(255, 255, 255) : RGB(200, 200, 200));
                SelectObject(hdc, g_hFontBold);
                wchar_t text[64]; GetWindowTextW(pdis->hwndItem, text, 64);
                DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rcClient; GetClientRect(hWnd, &rcClient);
            
            HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HPEN hOldP = (HPEN)SelectObject(hdc, hBorderPen);
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, 0, 0, rcClient.right, rcClient.bottom);
            SelectObject(hdc, hOldP);
            DeleteObject(hBorderPen);

            RECT rcHeader = {1, 1, rcClient.right - 1, 27};
            HBRUSH hBrHeader = CreateSolidBrush(RGB(128, 128, 128));
            FillRect(hdc, &rcHeader, hBrHeader);
            DeleteObject(hBrHeader);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0));
            SelectObject(hdc, g_hFontBold);
            TextOutW(hdc, 34, 4, L"Corel Nestler v4.3.4", 20);

            HPEN hPenW = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HPEN hOld = (HPEN)SelectObject(hdc, hPenW);
            
            int mx = rcClient.right - 45;
            MoveToEx(hdc, mx, 16, NULL); LineTo(hdc, mx + 10, 16); 
            
            int cx = rcClient.right - 20;
            MoveToEx(hdc, cx - 5, 8, NULL); LineTo(hdc, cx + 5, 18); 
            MoveToEx(hdc, cx + 5, 8, NULL); LineTo(hdc, cx - 5, 18);
            
            SelectObject(hdc, hOld);
            DeleteObject(hPenW);

            Gdiplus::Graphics graphics(hdc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath gp;
            for (const auto& d : SVG_PATHS) { AddSvgToPath(d.c_str(), gp); }
            Gdiplus::RectF bounds;
            gp.GetBounds(&bounds);
            if (bounds.Width > 0 && bounds.Height > 0) {
                float scale = std::min(16.0f / bounds.Width, 16.0f / bounds.Height);
                Gdiplus::Matrix matrix;
                matrix.Translate(10.0f - bounds.X * scale, 5.0f - bounds.Y * scale);
                matrix.Scale(scale, scale);
                gp.Transform(&matrix);
            }
            Gdiplus::SolidBrush brWhite(Gdiplus::Color(255, 255, 255, 255));
            graphics.FillPath(&brWhite, &gp);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_START) {
                if (g_AppState != STATE_READY) return 0;
                
                g_StartTime = GetTickCount64();
                
                wchar_t buf[64];
                GetDlgItemTextW(hWnd, IDC_MARGIN_EDIT, buf, 64);
                g_TargetMargin = _wtof(buf);
                GetDlgItemTextW(hWnd, IDC_PASSES_EDIT, buf, 64);
                g_TargetPasses = _wtoi(buf);
                if (g_TargetPasses < 1) g_TargetPasses = 1;
                
                SaveSettings();
                g_AppState = STATE_RUNNING;
                InvalidateRect(GetDlgItem(hWnd, IDC_START), NULL, FALSE);
                InvalidateRect(GetDlgItem(hWnd, IDOK), NULL, FALSE);
                SetDlgItemTextW(hWnd, IDC_TIME_ELAPSED, L"Время работы: 00:00");
                SendMessage(GetDlgItem(hWnd, IDC_LOG_LIST), LB_RESETCONTENT, 0, 0);
                LogMessage(hWnd, L"Синхронизация геометрии с CorelDRAW...");
                
                SetTimer(hWnd, TIMER_ID, 100, NULL);
                UpdateWindow(hWnd);
                
                HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
                if (FAILED(hr)) hr = CoInitialize(NULL);
                
                try {
                    IVGApplicationPtr pApp(L"CorelDRAW.Application");
                    IVGDocumentPtr pDoc = pApp->ActiveDocument;
                    if (pDoc == nullptr) { MessageBoxW(hWnd, L"Нет открытого документа!", L"Ошибка", MB_OK); goto L_ERROR; }
                    pDoc->Unit = cdrMillimeter;
                    IVGShapeRangePtr pSel = pApp->ActiveSelectionRange;
                    if (pSel->Count == 0) { MessageBoxW(hWnd, L"Ни один объект не выделен!", L"Ошибка", MB_OK); goto L_ERROR; }

                    IVGPagePtr pPage = pApp->ActivePage;
                    g_PageW = pPage->SizeWidth;
                    g_PageH = pPage->SizeHeight;
                    g_PageLeft = pPage->LeftX;
                    g_PageBottom = pPage->BottomY;

                    g_Shapes.clear();
                    for (long i = 1; i <= pSel->Count; i++) {
                        IVGShapePtr pShape = pSel->Item[i];
                        ShapeData sd;
                        sd.shape = pShape;
                        
                        double cx = pShape->CenterX;
                        double cy = pShape->CenterY;
                        sd.ref_cx = cx;
                        sd.ref_cy = cy;
                        
                        Clipper2Lib::Paths64 local_paths;
                        std::function<void(IVGShapePtr)> extractShape = [&](IVGShapePtr s) {
                            if (s == nullptr) return;
                            if (s->Type == 7) { 
                                for(long idx = 1; idx <= s->Shapes->Count; ++idx) extractShape(s->Shapes->Item[idx]);
                                return;
                            }
                            bool hasCurve = false;
                            try {
                                IVGCurvePtr curve = s->DisplayCurve;
                                if (curve != nullptr && curve->SubPaths->Count > 0) {
                                    hasCurve = true;
                                    for (long sp = 1; sp <= curve->SubPaths->Count; sp++) {
                                        Clipper2Lib::Path64 path;
                                        IVGSubPathPtr subPath = curve->SubPaths->Item[sp];
                                        for (long sg = 1; sg <= subPath->Segments->Count; sg++) {
                                            IVGSegmentPtr seg = subPath->Segments->Item[sg];
                                            if (seg->Type == 1) { 
                                                for (int k = 0; k <= 10; k++) {
                                                    double t = (double)k / 10.0;
                                                    double px = 0, py = 0;
                                                    if (SUCCEEDED(seg->raw_GetPointPositionAt(&px, &py, t, (cdrSegmentOffsetType)1))) {
                                                        path.push_back(Clipper2Lib::Point64(static_cast<int64_t>((px - cx) * SCALE), static_cast<int64_t>((py - cy) * SCALE)));
                                                    }
                                                }
                                            } else {
                                                IVGNodePtr node = seg->StartNode;
                                                path.push_back(Clipper2Lib::Point64(static_cast<int64_t>((node->PositionX - cx) * SCALE), static_cast<int64_t>((node->PositionY - cy) * SCALE)));
                                            }
                                        }
                                        IVGNodePtr lastNode = subPath->Nodes->Last;
                                        path.push_back(Clipper2Lib::Point64(static_cast<int64_t>((lastNode->PositionX - cx) * SCALE), static_cast<int64_t>((lastNode->PositionY - cy) * SCALE)));
                                        if (path.size() > 2) local_paths.push_back(path);
                                    }
                                }
                            } catch (...) {}
                            
                            if (!hasCurve) {
                                double left = s->LeftX;
                                double right = s->RightX;
                                double top = s->TopY;
                                double bottom = s->BottomY;
                                Clipper2Lib::Path64 bbox;
                                bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>((left - cx) * SCALE), static_cast<int64_t>((bottom - cy) * SCALE)));
                                bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>((right - cx) * SCALE), static_cast<int64_t>((bottom - cy) * SCALE)));
                                bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>((right - cx) * SCALE), static_cast<int64_t>((top - cy) * SCALE)));
                                bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>((left - cx) * SCALE), static_cast<int64_t>((top - cy) * SCALE)));
                                local_paths.push_back(bbox);
                            }
                        };

                        extractShape(pShape);
                        
                        if (local_paths.empty()) {
                            double w = pShape->SizeWidth;
                            double h = pShape->SizeHeight;
                            Clipper2Lib::Path64 bbox;
                            bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>(-w/2 * SCALE), static_cast<int64_t>(-h/2 * SCALE)));
                            bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>(w/2 * SCALE), static_cast<int64_t>(-h/2 * SCALE)));
                            bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>(w/2 * SCALE), static_cast<int64_t>(h/2 * SCALE)));
                            bbox.push_back(Clipper2Lib::Point64(static_cast<int64_t>(-w/2 * SCALE), static_cast<int64_t>(h/2 * SCALE)));
                            local_paths.push_back(bbox);
                        }

                        int64_t min_x = 9223372036854775807LL, min_y = 9223372036854775807LL;
                        for (const auto& path : local_paths) {
                            for (const auto& pt : path) {
                                if (pt.x < min_x) min_x = pt.x;
                                if (pt.y < min_y) min_y = pt.y;
                            }
                        }
                        for (auto& path : local_paths) {
                            for (auto& pt : path) { pt.x -= min_x; pt.y -= min_y; }
                        }

                        local_paths = Clipper2Lib::SimplifyPaths(local_paths, 1.0 * SCALE);
                        sd.orig_paths = Clipper2Lib::Union(local_paths, Clipper2Lib::FillRule::NonZero);
                        sd.orig_area = Clipper2Lib::Area(sd.orig_paths);
                        g_Shapes.push_back(sd);
                    }
                    
                    g_AllPasses.clear();
                    g_GeneratedSequences.clear();
                    g_IsExhausted = false;
                    g_PassesStarted = 0;
                    g_PassesCompleted = 0;
                    g_ProgressPercent = 0;
                    
                    std::thread(BackgroundMathThread).detach();
                    return 0;
                } catch (...) { MessageBoxW(hWnd, L"Ошибка чтения CorelDraw", L"Сбой", MB_OK); }
            L_ERROR:
                KillTimer(hWnd, TIMER_ID);
                CoUninitialize();
                g_AppState = STATE_READY;
                InvalidateRect(GetDlgItem(hWnd, IDC_START), NULL, FALSE);
                return 0;
            } else if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                if (g_AppState == STATE_RUNNING) return 0;
                if (LOWORD(wParam) == IDOK && g_AppState != STATE_DONE) return 0;
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        case WM_USER_PASS_START: {
            wchar_t b[128]; swprintf(b, 128, L"Производится расчёт варианта # %d...", static_cast<int>(wParam));
            LogMessage(hWnd, b);
            break;
        }
        case WM_USER_PASS_END: {
            wchar_t b[128]; swprintf(b, 128, L"Вариант # %d: размещено %d объектов.", static_cast<int>(wParam), static_cast<int>(lParam));
            LogMessage(hWnd, b);
            break;
        }
        case WM_TIMER:
            if (wParam == TIMER_ID) {
                int done = g_PassesCompleted.load();
                g_ProgressPercent = g_TargetPasses > 0 ? (done * 100) / g_TargetPasses : 0;
                InvalidateRect(GetDlgItem(hWnd, IDC_PROGRESS_DRAW), NULL, FALSE);
                
                ULONGLONG now = GetTickCount64();
                ULONGLONG elapsed_ms = now - g_StartTime;
                int elapsed_s = (int)((elapsed_ms + 500) / 1000); 
                
                int rem_s = 0;
                if (done > 0 && done < g_TargetPasses) {
                    double ms_per_pass = (double)elapsed_ms / done;
                    rem_s = (int)(((ms_per_pass * (g_TargetPasses - done)) + 500) / 1000.0);
                }
                if (rem_s < 0) rem_s = 0;
                
                wchar_t tb[64];
                swprintf(tb, 64, L"Время работы: %02d:%02d", elapsed_s / 60, elapsed_s % 60);
                SetDlgItemTextW(hWnd, IDC_TIME_ELAPSED, tb);
                swprintf(tb, 64, L"Осталось: %02d:%02d", rem_s / 60, rem_s % 60);
                SetDlgItemTextW(hWnd, IDC_TIME_LEFT, tb);
            }
            break;
        case WM_USER_MATH_DONE: {
            KillTimer(hWnd, TIMER_ID);
            g_ProgressPercent = 100;
            g_AppState = STATE_DONE;
            
            ULONGLONG pre_now = GetTickCount64();
            ULONGLONG pre_elapsed = pre_now - g_StartTime;
            int pre_s = (int)((pre_elapsed + 500) / 1000);
            wchar_t tb_pre[64];
            swprintf(tb_pre, 64, L"Время работы: %02d:%02d", pre_s / 60, pre_s % 60);
            SetDlgItemTextW(hWnd, IDC_TIME_ELAPSED, tb_pre);
            SetDlgItemTextW(hWnd, IDC_TIME_LEFT, L"Осталось: 00:00 (Перенос...)");

            InvalidateRect(GetDlgItem(hWnd, IDC_PROGRESS_DRAW), NULL, FALSE);
            InvalidateRect(GetDlgItem(hWnd, IDC_START), NULL, FALSE);
            InvalidateRect(GetDlgItem(hWnd, IDOK), NULL, FALSE);
            UpdateWindow(hWnd); 

            size_t best_pass = 0;
            for(size_t i=1; i<g_AllPasses.size(); ++i) {
                if (g_AllPasses[i].placed_count > g_AllPasses[best_pass].placed_count) {
                    best_pass = i;
                } else if (g_AllPasses[i].placed_count == g_AllPasses[best_pass].placed_count) {
                    if (g_AllPasses[i].max_x < g_AllPasses[best_pass].max_x) best_pass = i;
                }
            }

            try {
                IVGApplicationPtr pApp(L"CorelDRAW.Application");
                IVGDocumentPtr pDoc = pApp->ActiveDocument;
                pDoc->BeginCommandGroup(L"Nesting Plugin");

                double out_R_x = g_PageW + g_TargetMargin; double out_R_y = 0; double out_R_maxW = 0;

                for (size_t i = 0; i < g_Shapes.size(); i++) {
                    auto& res = g_AllPasses[best_pass].results[i];
                    IVGShapePtr pShape = g_Shapes[i].shape;
                    
                    if (res.angle != 0) {
                        pShape->RotationCenterX = g_Shapes[i].ref_cx;
                        pShape->RotationCenterY = g_Shapes[i].ref_cy;
                        try { pShape->Rotate(res.angle); } catch(...) {}
                    }
                    
                    if (res.placed) {
                        double target_left = g_PageLeft + res.target_x;
                        double target_bottom = g_PageBottom + res.target_y;
                        double current_left = pShape->LeftX;
                        double current_bottom = pShape->BottomY;
                        pShape->Move(target_left - current_left, target_bottom - current_bottom);
                    } else {
                        double w = pShape->SizeWidth;
                        double h = pShape->SizeHeight;
                        double final_x = out_R_x; 
                        double final_y = out_R_y;
                        out_R_y += (h + g_TargetMargin);
                        if (w > out_R_maxW) out_R_maxW = w;
                        if (out_R_y > g_PageH) { out_R_y = 0; out_R_x += out_R_maxW + g_TargetMargin; out_R_maxW = 0; }
                        
                        double target_left = g_PageLeft + final_x;
                        double target_bottom = g_PageBottom + final_y;
                        double current_left = pShape->LeftX;
                        double current_bottom = pShape->BottomY;
                        pShape->Move(target_left - current_left, target_bottom - current_bottom);
                    }
                }
                pDoc->EndCommandGroup();
                LogMessage(hWnd, L"=== Компоновка успешно завершена ===");
            } catch (...) { MessageBoxW(hWnd, L"Ошибка применения координат!", L"Сбой", MB_OK | MB_ICONERROR); }
            
            CoUninitialize();

            ULONGLONG now = GetTickCount64();
            ULONGLONG elapsed_ms = now - g_StartTime;
            int elapsed_s = (int)((elapsed_ms + 500) / 1000);
            
            wchar_t tb[64];
            swprintf(tb, 64, L"Время работы: %02d:%02d", elapsed_s / 60, elapsed_s % 60);
            SetDlgItemTextW(hWnd, IDC_TIME_ELAPSED, tb);
            SetDlgItemTextW(hWnd, IDC_TIME_LEFT, L"Осталось: 00:00");
            break;
        }
        case WM_DESTROY:
            for(HWND hc : g_StaticLabels) DestroyWindow(hc);
            g_StaticLabels.clear();
            if (g_hBrushBg) DeleteObject(g_hBrushBg);
            if (g_hBrushWhite) DeleteObject(g_hBrushWhite);
            if (g_hFontReg) DeleteObject(g_hFontReg);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            if (g_hFontHuge) DeleteObject(g_hFontHuge);
            if (g_hFontMemReg) RemoveFontMemResourceEx(g_hFontMemReg);
            if (g_hFontMemBold) RemoveFontMemResourceEx(g_hFontMemBold);
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    
    InitCommonControls();
    
    g_hFontMemReg = LoadFontFromResource(IDR_FONT_REGULAR);
    g_hFontMemBold = LoadFontFromResource(IDR_FONT_BOLD);
    
    g_hFontHuge = CreateFontW(38, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Azbuka");
    g_hFontBold = CreateFontW(17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Azbuka");
    g_hFontReg = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Azbuka");
    
    g_hBrushBg = CreateSolidBrush(RGB(230, 230, 230)); 
    g_hBrushWhite = CreateSolidBrush(RGB(255, 255, 255));

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    // ИСПРАВЛЕНИЕ: Интеграция сгенерированной иконки в окно Windows
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_hBrushBg;
    wc.lpszClassName = L"CorelNesterClass";
    RegisterClassW(&wc);
    
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    
    HWND hWnd = CreateWindowExW(0, L"CorelNesterClass", L"Corel Nestler v4.3.4", WS_POPUP, (screenW - 356) / 2, (screenH - 725) / 2, 356, 725, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return (int)msg.wParam;
}