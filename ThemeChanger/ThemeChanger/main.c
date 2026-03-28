#define IDI_MYICON 103
#define IDB_LOGO 102
#define IDB_INSTALL_BG 104
#define IDB_BG_WALLPAPER 105 
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <wchar.h>
#include <shellapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <time.h>
#include <winuser.h>
#include <stdio.h>
#include <winreg.h>
#include <stdlib.h>
#pragma comment(lib, "advapi32.lib")

BOOL ShowLicenseDialog(HWND hParent);
LRESULT CALLBACK LicenseWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#ifndef _WIN32_WINNT_WIN10
#define _WIN32_WINNT_WIN10 0x0A00
#endif
#ifndef _WIN32_WINNT_WIN11
#define _WIN32_WINNT_WIN11 0x0A00
#endif

#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")

#define WINDOW_WIDTH  450
#define WINDOW_HEIGHT 275
#ifndef SS_WORDBREAK
#define SS_WORDBREAK 0x00000010L
#endif
#ifndef SS_EDITCONTROL
#define SS_EDITCONTROL 0x00000020L
#endif

HWND hRadioVista, hRadio7, hRadio8, hBtnNext, hBtnCancel;
HWND hCheckIconPack;
BOOL bInstallIconPack = FALSE;
int nSelectedTheme = 0;
int g_dpi = 96;
HWND hMainWnd = NULL;
HWND hInstallWnd = NULL;
HWND hLogoStatic = NULL;
HBITMAP hLogoBmp = NULL;
HANDLE hBatThread = NULL;
HWND hCancelBtn = NULL;
HWND hTaskLabels[2] = { NULL };
UINT_PTR g_timerID = 0;
HWND hErrorText = NULL;

// 步骤文字（两项）
const WCHAR* batSteps[] = {
    L"正在解压主题文件...",
    L"正在安装美化软件..."
};
int currentTaskIndex = 0;

#define SCALE(x) MulDiv(x, g_dpi, 96)

#define WM_BAT_COMPLETE (WM_USER + 1)
#define WM_BAT_FAILED (WM_USER + 2)
#define WM_SHOW_ERROR (WM_USER + 3)
#define WM_UPDATE_TASK_STEP (WM_USER + 10)

// 重启系统（需要管理员权限）
BOOL RebootSystem()
{
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;

    // 获取当前进程令牌
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    // 获取关机特权 LUID
    LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // 启用关机特权
    AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, 0);
    CloseHandle(hToken);

    // 执行重启（强制关闭所有程序，不等待）
    if (ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER))
        return TRUE;

    return FALSE;
}

// 系统版本检测（保留原有逻辑）
BOOL CheckWindowsVersion()
{
    WCHAR szOSVersion[256] = { 0 };
    DWORD dwSize = sizeof(szOSVersion);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"ProductName",
        RRF_RT_REG_SZ,
        NULL,
        szOSVersion,
        &dwSize) != ERROR_SUCCESS)
    {
        OSVERSIONINFOEXW osvi = { 0 };
        osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXW);
#pragma warning(push)
#pragma warning(disable:4996)
        GetVersionExW((OSVERSIONINFOW*)&osvi);
#pragma warning(pop)
        if (osvi.dwMajorVersion != 10)
        {
            MessageBox(NULL, L"该工具仅支持Windows 10/11系统！", L"系统版本错误", MB_OK | MB_ICONERROR);
            return FALSE;
        }
    }
    if (wcsstr(szOSVersion, L"Windows 11") != NULL)
    {
        MessageBox(NULL, L"该工具不适用于Windows 11系统，程序即将退出！", L"系统版本错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    else if (wcsstr(szOSVersion, L"Windows 10") != NULL)
    {
        return TRUE;
    }
    else
    {
        int ret = MessageBox(NULL, L"未检测到Windows 10/11系统，是否强制运行？", L"版本提示", MB_YESNO | MB_ICONQUESTION);
        return (ret == IDYES);
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK InstallWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
DWORD WINAPI BatExecutionThread(LPVOID lpParam);
void CancelDeployment(HWND hwnd);
void ForceCloseInstallWindow();
BOOL CheckLogForErrors();
BOOL RunIconPackExe();
BOOL ExecuteBatFile(const WCHAR* szBatPath, int* nErrorType, DWORD* pExitCode);

BOOL IsRunningAsAdmin()
{
    BOOL bIsAdmin = FALSE;
    PSID pAdminSID = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0,
        &pAdminSID))
    {
        CheckTokenMembership(NULL, pAdminSID, &bIsAdmin);
        FreeSid(pAdminSID);
    }
    return bIsAdmin;
}

BOOL KillProcessByPID(DWORD dwPID)
{
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, dwPID);
    if (hProcess == NULL) return FALSE;
    BOOL bResult = TerminateProcess(hProcess, 0);
    CloseHandle(hProcess);
    return bResult;
}

void RestartAsAdminWithTheme(int themeType)
{
    WCHAR szExePath[MAX_PATH] = { 0 };
    WCHAR szCmdLine[MAX_PATH] = { 0 };
    DWORD dwCurrentPID = GetCurrentProcessId();
    GetModuleFileName(NULL, szExePath, MAX_PATH);
    wsprintf(szCmdLine, L"\"%s\" /theme:%d /pid:%d /iconpack:%d", szExePath, themeType, dwCurrentPID, bInstallIconPack);
    SHELLEXECUTEINFO sei = { 0 };
    sei.cbSize = sizeof(SHELLEXECUTEINFO);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = szExePath;
    sei.lpParameters = szCmdLine + wcslen(szExePath) + 2;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteEx(&sei))
    {
        MessageBox(NULL, L"无法获取管理员权限！无法应用主题。", L"运行错误", MB_OK | MB_ICONWARNING);
    }
    else
    {
        Sleep(500);
        PostQuitMessage(0);
    }
}

void ParseCmdLine(LPSTR lpCmdLine, int* pTheme, DWORD* pPID, BOOL* pIconPack)
{
    *pTheme = 0;
    *pPID = 0;
    *pIconPack = FALSE;
    if (lpCmdLine == NULL || strlen(lpCmdLine) == 0) return;
    char* pThemeFlag = strstr(lpCmdLine, "/theme:");
    if (pThemeFlag != NULL) *pTheme = atoi(pThemeFlag + 7);
    char* pPIDFlag = strstr(lpCmdLine, "/pid:");
    if (pPIDFlag != NULL) *pPID = atoi(pPIDFlag + 5);
    char* pIconPackFlag = strstr(lpCmdLine, "/iconpack:");
    if (pIconPackFlag != NULL) *pIconPack = (atoi(pIconPackFlag + 9) == 1) ? TRUE : FALSE;
}

void ForceCloseInstallWindow()
{
    if (g_timerID != 0)
    {
        KillTimer(NULL, g_timerID);
        g_timerID = 0;
    }
    if (hBatThread != NULL)
    {
        CloseHandle(hBatThread);
        hBatThread = NULL;
    }
}

void CreateInstallWindow(int themeType)
{
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = InstallWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 255)); // 蓝色背景
    wc.lpszClassName = L"InstallThemeWindow";
    HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_MYICON));
    if (hIcon == NULL) hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIcon = hIcon;
    wc.hIconSm = hIcon;
    RegisterClassEx(&wc);

    HDC hdcScreen = GetDC(NULL);
    int screenWidth = GetDeviceCaps(hdcScreen, HORZRES);
    int screenHeight = GetDeviceCaps(hdcScreen, VERTRES);
    ReleaseDC(NULL, hdcScreen);

    hInstallWnd = CreateWindowEx(
        0,
        L"InstallThemeWindow",
        L"主题安装中...",
        WS_POPUP | WS_VISIBLE,
        0, 0, screenWidth, screenHeight,
        NULL, NULL, GetModuleHandle(NULL),
        (LPVOID)(INT_PTR)themeType
    );
    UpdateWindow(hInstallWnd);
}

BOOL CheckLogForErrors()
{
    const WCHAR* logPath = L"C:\\Windows\\vizynrun.log";
    FILE* fp = NULL;
    errno_t err = _wfopen_s(&fp, logPath, L"r");
    if (err != 0 || fp == NULL) return FALSE;
    WCHAR buffer[256] = { 0 };
    BOOL hasError = FALSE;
    const WCHAR* errorKeywords[] = { L"错误", L"失败", L"解压", L"Error", L"Fail", L"unzip" };
    int keywordCount = sizeof(errorKeywords) / sizeof(WCHAR*);
    while (fgetws(buffer, 256, fp) != NULL)
    {
        for (int i = 0; i < keywordCount; i++)
        {
            if (wcsstr(buffer, errorKeywords[i]) != NULL)
            {
                hasError = TRUE;
                break;
            }
        }
        if (hasError) break;
    }
    fclose(fp);
    return hasError;
}

BOOL ExecuteBatFile(const WCHAR* szBatPath, int* nErrorType, DWORD* pExitCode)
{
    if (pExitCode != NULL) *pExitCode = 0;
    if (GetFileAttributes(szBatPath) == INVALID_FILE_ATTRIBUTES)
    {
        *nErrorType = 2;
        return FALSE;
    }
    WCHAR szCmdLine[MAX_PATH] = { 0 };
    wsprintf(szCmdLine, L"cmd.exe /c \"%s\"", szBatPath);
    STARTUPINFO si = { 0 };
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcess(NULL, szCmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        *nErrorType = 3;
        return FALSE;
    }
    DWORD dwWait = WaitForSingleObject(pi.hProcess, INFINITE);
    if (dwWait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, pExitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (CheckLogForErrors())
    {
        *nErrorType = 5;
        return FALSE;
    }
    if (dwWait == WAIT_OBJECT_0 && *pExitCode == 0)
    {
        *nErrorType = 0;
        return TRUE;
    }
    else
    {
        if (*pExitCode != 0) *nErrorType = 4;
        return FALSE;
    }
}

BOOL RunIconPackExe()
{
    WCHAR szExePath[MAX_PATH] = { 0 };
    GetModuleFileName(NULL, szExePath, MAX_PATH);
    wchar_t* pSlash = wcsrchr(szExePath, L'\\');
    if (pSlash) *pSlash = L'\0';
    wsprintf(szExePath, L"%s\\data\\Windows7.exe", szExePath);
    if (GetFileAttributes(szExePath) == INVALID_FILE_ATTRIBUTES) return FALSE;
    STARTUPINFO si = { 0 };
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcess(szExePath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    Sleep(1000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

//判断脚本执行
DWORD WINAPI BatExecutionThread(LPVOID lpParam)
{
    int themeType = (int)(INT_PTR)lpParam;
    WCHAR szBatPath[MAX_PATH] = { 0 };
    WCHAR szBatName[MAX_PATH] = { 0 };
    BOOL bSuccess = TRUE;  // 默认成功，遇到错误则置为 FALSE

    GetModuleFileName(NULL, szBatPath, MAX_PATH);
    wchar_t* pSlash = wcsrchr(szBatPath, L'\\');
    if (pSlash) *pSlash = L'\0';

    // 1. 执行主题主批处理
    switch (themeType)
    {
    case 1: wcscpy_s(szBatName, MAX_PATH, L"WinVd.bat"); break;
    case 2: wcscpy_s(szBatName, MAX_PATH, L"win7d.bat"); break;
    case 3: wcscpy_s(szBatName, MAX_PATH, L"win8d.bat"); break;
    default: break;
    }

    if (themeType >= 1 && themeType <= 3)
    {
        wsprintf(szBatPath, L"%s\\%s", szBatPath, szBatName);
        WCHAR szCmdLine[MAX_PATH] = { 0 };
        wsprintf(szCmdLine, L"cmd.exe /c \"%s\"", szBatPath);

        STARTUPINFO si = { 0 };
        si.cb = sizeof(STARTUPINFO);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = { 0 };
        if (CreateProcess(NULL, szCmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD dwExitCode = 0;
            GetExitCodeProcess(pi.hProcess, &dwExitCode);
            if (dwExitCode != 0) bSuccess = FALSE;
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        else
        {
            bSuccess = FALSE; // 进程创建失败
        }
    }

    // 3. 安装图标包（如果用户选择）
    if (bInstallIconPack)
    {
        if (!RunIconPackExe()) bSuccess = FALSE;
    }

    // 4. 发送完成消息（不在此处关闭窗口，由消息处理关闭）
    if (bSuccess)
        PostMessage(hMainWnd, WM_BAT_COMPLETE, 0, 0);
    else
        PostMessage(hMainWnd, WM_BAT_FAILED, 0, 0);

    return 0;
}

void CancelDeployment(HWND hwnd)
{
    if (g_timerID != 0)
    {
        KillTimer(hwnd, g_timerID);
        g_timerID = 0;
    }
    if (hBatThread != NULL)
    {
        TerminateThread(hBatThread, 0);
        CloseHandle(hBatThread);
        hBatThread = NULL;
    }
    HWND hWnd = FindWindow(L"InstallThemeWindow", L"主题安装中...");
    if (hWnd != NULL && IsWindow(hWnd))
    {
        SendMessage(hWnd, WM_CLOSE, 0, 0);
        Sleep(100);
        if (IsWindow(hWnd)) DestroyWindow(hWnd);
    }
    hInstallWnd = NULL;
    MessageBox(hMainWnd, L"已取消主题部署操作！", L"取消成功", MB_OK | MB_ICONINFORMATION);
}

void RunThemeBat(int themeType)
{
    if (!IsRunningAsAdmin())
    {
        int ret = MessageBox(hMainWnd,
            L"应用主题需要管理员权限！是否立即获取管理员权限并继续？",
            L"权限不足", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES)
        {
            RestartAsAdminWithTheme(themeType);
            return;
        }
        else
        {
            MessageBox(hMainWnd, L"无管理员权限！", L"提示", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    int confirmRet = MessageBox(hMainWnd,
        L"您即将应用所选的系统主题，请确保已保存所有重要数据。请勿重复部署，这会导致脚本错误\n您是否确认执行部署操作？",
        L"主题部署确认", MB_YESNO | MB_ICONQUESTION);
    if (confirmRet != IDYES)
    {
        MessageBox(hMainWnd, L"你取消了应用", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    CreateInstallWindow(themeType);
    hBatThread = CreateThread(NULL, 0, BatExecutionThread, (LPVOID)(INT_PTR)themeType, 0, NULL);
    if (!hBatThread)
    {
        ForceCloseInstallWindow();
        MessageBox(hMainWnd, L"创建线程失败！", L"错误", MB_OK | MB_ICONERROR);
    }
}

// 主窗口Logo：随DPI缩放，保证物理大小一致
void LoadAndShowLogo(HWND hwnd, HINSTANCE hInstance)
{
    if (hLogoBmp) DeleteObject(hLogoBmp);
    // 加载原始位图
    HBITMAP hOriginal = (HBITMAP)LoadImage(hInstance, MAKEINTRESOURCE(IDB_LOGO),
        IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
    if (hOriginal == NULL) return;

    BITMAP bm;
    GetObject(hOriginal, sizeof(BITMAP), &bm);
    int originalWidth = bm.bmWidth;
    int originalHeight = bm.bmHeight;

    // 设定目标宽度（逻辑像素，经DPI缩放后）
    int targetWidth = SCALE(120);
    int targetHeight = (int)((float)originalHeight / originalWidth * targetWidth);

    // 创建兼容DC和内存位图，进行缩放绘制
    HDC hdc = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hScaled = CreateCompatibleBitmap(hdc, targetWidth, targetHeight);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, hScaled);

    HDC hdcOriginal = CreateCompatibleDC(hdc);
    HGDIOBJ hOldOriginal = SelectObject(hdcOriginal, hOriginal);
    StretchBlt(hdcMem, 0, 0, targetWidth, targetHeight,
        hdcOriginal, 0, 0, originalWidth, originalHeight, SRCCOPY);
    SelectObject(hdcOriginal, hOldOriginal);
    DeleteDC(hdcOriginal);

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdc);

    // 保存缩放后的位图，并释放原始位图
    hLogoBmp = hScaled;
    DeleteObject(hOriginal);

    // 定位到窗口右上角（右侧留出20像素边距，顶部20像素）
    int logoX = SCALE(WINDOW_WIDTH) - targetWidth - SCALE(20);
    int logoY = SCALE(20);

    if (hLogoStatic) DestroyWindow(hLogoStatic);
    hLogoStatic = CreateWindowEx(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_BITMAP,
        logoX, logoY, targetWidth, targetHeight,
        hwnd, (HMENU)IDB_LOGO, hInstance, NULL);
    SendMessage(hLogoStatic, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hLogoBmp);
}

void CreateScaledControls(HWND hwnd, HINSTANCE hInstance)
{
    const int LEFT_OFFSET = SCALE(20);
    int fontHeight = SCALE(-14);
    HFONT hFont = CreateFont(
        fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HWND hTitle = CreateWindowEx(0, L"STATIC", L"欢迎使用Vizyn一键仿历代Win主题包！",
        WS_CHILD | WS_VISIBLE, LEFT_OFFSET, SCALE(20), SCALE(410), SCALE(30),
        hwnd, (HMENU)1001, hInstance, NULL);
    SendMessage(hTitle, WM_SETFONT, (WPARAM)hFont, TRUE);
    HWND hSubTitle2 = CreateWindowEx(0, L"STATIC", L"请选择您想要的类型：",
        WS_CHILD | WS_VISIBLE, LEFT_OFFSET, SCALE(55), SCALE(410), SCALE(25),
        hwnd, (HMENU)1007, hInstance, NULL);
    SendMessage(hSubTitle2, WM_SETFONT, (WPARAM)hFont, TRUE);
    hRadioVista = CreateWindowEx(0, L"BUTTON", L"仿 Vista 主题",
        WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON,
        SCALE(80), SCALE(85), SCALE(150), SCALE(30),
        hwnd, (HMENU)1002, hInstance, NULL);
    SendMessage(hRadioVista, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTheme(hRadioVista, L"Explorer", NULL);
    hRadio7 = CreateWindowEx(0, L"BUTTON", L"仿 Win7 主题",
        WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON,
        SCALE(80), SCALE(125), SCALE(150), SCALE(30),
        hwnd, (HMENU)1003, hInstance, NULL);
    SendMessage(hRadio7, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTheme(hRadio7, L"Explorer", NULL);
    hRadio8 = CreateWindowEx(0, L"BUTTON", L"仿 Win8 主题",
        WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON,
        SCALE(80), SCALE(165), SCALE(150), SCALE(30),
        hwnd, (HMENU)1004, hInstance, NULL);
    SendMessage(hRadio8, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTheme(hRadio8, L"Explorer", NULL);
    hCheckIconPack = CreateWindowEx(
        0, L"BUTTON", L"部署Win7图标包（可选）",
        WS_CHILD | WS_VISIBLE | BS_CHECKBOX | WS_TABSTOP,
        SCALE(80), SCALE(200), SCALE(200), SCALE(25),
        hwnd, (HMENU)1009, hInstance, NULL);
    SendMessage(hCheckIconPack, WM_SETFONT, (WPARAM)hFont, TRUE);
    int RIGHT_OFFSET = SCALE(20);
    hBtnNext = CreateWindowEx(0, L"BUTTON", L"应用主题",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        SCALE(WINDOW_WIDTH) - RIGHT_OFFSET - SCALE(90) - SCALE(90) - SCALE(10),
        SCALE(230), SCALE(90), SCALE(35),
        hwnd, (HMENU)1005, hInstance, NULL);
    SendMessage(hBtnNext, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTheme(hBtnNext, L"Explorer", NULL);
    hBtnCancel = CreateWindowEx(0, L"BUTTON", L"退出",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        SCALE(WINDOW_WIDTH) - RIGHT_OFFSET - SCALE(90),
        SCALE(230), SCALE(90), SCALE(35),
        hwnd, (HMENU)1006, hInstance, NULL);
    SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTheme(hBtnCancel, L"Explorer", NULL);
    HWND hVersion = CreateWindowEx(0, L"STATIC", L"版本:1.0 By 栖渊Kaelen",
        WS_CHILD | WS_VISIBLE, LEFT_OFFSET, SCALE(270), SCALE(200), SCALE(20),
        hwnd, (HMENU)1008, hInstance, NULL);
    HFONT hVersionFont = CreateFont(
        SCALE(-12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessage(hVersion, WM_SETFONT, (WPARAM)hVersionFont, TRUE);
    LoadAndShowLogo(hwnd, hInstance);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{   
    // 初始化公共控件（许可协议窗口需要编辑框等控件）
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (!CheckWindowsVersion()) return 0;
    if (!CheckWindowsVersion()) return 0;
    int themeFromCmd = 0;
    DWORD dwOldPID = 0;
    BOOL iconPackFromCmd = FALSE;
    ParseCmdLine(lpCmdLine, &themeFromCmd, &dwOldPID, &iconPackFromCmd);
    if (dwOldPID > 0 && IsRunningAsAdmin()) KillProcessByPID(dwOldPID);
    if (themeFromCmd > 0) nSelectedTheme = themeFromCmd;
    if (iconPackFromCmd) bInstallIconPack = iconPackFromCmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // 启动时显示许可协议弹窗
    if (!ShowLicenseDialog(NULL))
    {
        // 用户拒绝或关闭弹窗，直接退出
        return 1;
    }
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"VizynTheme";
    HICON hAppIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MYICON));
    if (hAppIcon == NULL) hAppIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIcon = hAppIcon;
    wc.hIconSm = hAppIcon;
    RegisterClassEx(&wc);
    g_dpi = GetDpiForSystem();
    int actualWidth = SCALE(WINDOW_WIDTH);
    int actualHeight = SCALE(WINDOW_HEIGHT + 50);
    hMainWnd = CreateWindowEx(
        0, L"VizynTheme", L"Vizyn一键仿历代Win主题包",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, actualWidth, actualHeight,
        NULL, NULL, hInstance, NULL);
    SendMessage(hMainWnd, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
    SendMessage(hMainWnd, WM_SETICON, ICON_SMALL, (LPARAM)hAppIcon);
    SetWindowTheme(hMainWnd, L"Explorer", NULL);
    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);
    if (themeFromCmd > 0)
    {
        PostMessage(hMainWnd, WM_COMMAND,
            MAKEWPARAM(themeFromCmd == 1 ? 1002 : (themeFromCmd == 2 ? 1003 : 1004), 0), 0);
    }
    if (bInstallIconPack) CheckDlgButton(hMainWnd, 1009, BST_CHECKED);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (hLogoBmp != NULL) { DeleteObject(hLogoBmp); hLogoBmp = NULL; }
    if (hBatThread != NULL) { CloseHandle(hBatThread); hBatThread = NULL; }
    return 0;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        CreateScaledControls(hwnd, ((LPCREATESTRUCT)lParam)->hInstance);
        break;
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        int id = GetDlgCtrlID((HWND)lParam);
        if (id == 1001)      SetTextColor(hdc, RGB(0, 102, 204));
        else if (id == 1007) SetTextColor(hdc, RGB(0, 0, 0));
        else if (id == 1008) SetTextColor(hdc, RGB(102, 102, 102));
        else                 SetTextColor(hdc, RGB(33, 33, 33));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wParam, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_DPICHANGED:
        g_dpi = HIWORD(wParam);
        SetWindowPos(hwnd, NULL,
            ((RECT*)lParam)->left, ((RECT*)lParam)->top,
            SCALE(WINDOW_WIDTH), SCALE(WINDOW_HEIGHT + 50), SWP_NOZORDER | SWP_NOACTIVATE);
        if (hLogoStatic) { DestroyWindow(hLogoStatic); hLogoStatic = NULL; }
        if (hLogoBmp) { DeleteObject(hLogoBmp); hLogoBmp = NULL; }
        LoadAndShowLogo(hwnd, GetModuleHandle(NULL));
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* p = (MINMAXINFO*)lParam;
        p->ptMinTrackSize.x = SCALE(WINDOW_WIDTH);
        p->ptMinTrackSize.y = SCALE(WINDOW_HEIGHT + 50);
        p->ptMaxTrackSize.x = SCALE(WINDOW_WIDTH);
        p->ptMaxTrackSize.y = SCALE(WINDOW_HEIGHT + 50);
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case 1002: nSelectedTheme = 1; CheckRadioButton(hwnd, 1002, 1004, 1002); break;
        case 1003: nSelectedTheme = 2; CheckRadioButton(hwnd, 1002, 1004, 1003); break;
        case 1004: nSelectedTheme = 3; CheckRadioButton(hwnd, 1002, 1004, 1004); break;
        case 1009: bInstallIconPack = IsDlgButtonChecked(hwnd, 1009) == BST_CHECKED; break;
        case 1005:
            if (nSelectedTheme == 0) MessageBox(hwnd, L"请先选择主题类型！", L"提示", MB_OK | MB_ICONINFORMATION);
            else RunThemeBat(nSelectedTheme);

            break;
        case 1006: DestroyWindow(hwnd); break;
        }
        break;
    case WM_BAT_COMPLETE:
    {
        HWND hInstall = FindWindow(L"InstallThemeWindow", L"主题安装中...");
        if (hInstall != NULL && IsWindow(hInstall))
        {
            // 隐藏第一个标签
            if (hTaskLabels[0] != NULL && IsWindow(hTaskLabels[0]))
                ShowWindow(hTaskLabels[0], SW_HIDE);

            // 获取第一个标签的位置，用于将第二个标签移动到该位置
            RECT rect;
            if (hTaskLabels[0] != NULL && IsWindow(hTaskLabels[0]))
            {
                GetWindowRect(hTaskLabels[0], &rect);
                ScreenToClient(hInstall, (LPPOINT)&rect.left);
                // 将第二个标签移动到第一个标签的位置
                if (hTaskLabels[1] != NULL && IsWindow(hTaskLabels[1]))
                {
                    SetWindowPos(hTaskLabels[1], NULL, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
            }

            // 修改第二个标签的文字并加粗
            if (hTaskLabels[1] != NULL && IsWindow(hTaskLabels[1]))
            {
                SetWindowText(hTaskLabels[1], L"主题安装完成！");
                HFONT hBold = CreateFont(SCALE(-20), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"微软雅黑");
                SendMessage(hTaskLabels[1], WM_SETFONT, (WPARAM)hBold, TRUE);
                // 字体将在窗口销毁时释放，这里不释放以免影响显示
            }

            // 将取消按钮改为“关闭”
            if (hCancelBtn != NULL && IsWindow(hCancelBtn))
            {
                SetWindowText(hCancelBtn, L"关闭");
            }

            // 停止定时器
            if (g_timerID != 0)
            {
                KillTimer(hInstall, g_timerID);
                g_timerID = 0;
            }
        }

        // 弹出重启确认对话框
        int ret = MessageBox(hMainWnd,
            L"主题安装完成！需要重启系统以应用所有更改。\n是否立即重启？",
            L"安装完成", MB_YESNO | MB_ICONQUESTION);
        if (ret == IDYES)
        {
            if (!RebootSystem())
            {
                MessageBox(hMainWnd, L"重启失败，请手动重启系统。", L"错误", MB_OK | MB_ICONERROR);
            }
        }
        break;
    }

    case WM_BAT_FAILED:
    {
        HWND hInstall = FindWindow(L"InstallThemeWindow", L"主题安装中...");
        if (hInstall != NULL && IsWindow(hInstall))
        {
            // 隐藏第一个标签
            if (hTaskLabels[0] != NULL && IsWindow(hTaskLabels[0]))
                ShowWindow(hTaskLabels[0], SW_HIDE);

            // 获取第一个标签的位置，用于将第二个标签移动到该位置
            RECT rect;
            if (hTaskLabels[0] != NULL && IsWindow(hTaskLabels[0]))
            {
                GetWindowRect(hTaskLabels[0], &rect);
                ScreenToClient(hInstall, (LPPOINT)&rect.left);
                // 将第二个标签移动到第一个标签的位置
                if (hTaskLabels[1] != NULL && IsWindow(hTaskLabels[1]))
                {
                    SetWindowPos(hTaskLabels[1], NULL, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
            }

            // 修改第二个标签的文字并加粗
            if (hTaskLabels[1] != NULL && IsWindow(hTaskLabels[1]))
            {
                SetWindowText(hTaskLabels[1], L"安装失败，请查看日志。");
                HFONT hBold = CreateFont(SCALE(-20), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"微软雅黑");
                SendMessage(hTaskLabels[1], WM_SETFONT, (WPARAM)hBold, TRUE);
            }

            // 将取消按钮改为“关闭”
            if (hCancelBtn != NULL && IsWindow(hCancelBtn))
            {
                SetWindowText(hCancelBtn, L"关闭");
            }

            // 停止定时器
            if (g_timerID != 0)
            {
                KillTimer(hInstall, g_timerID);
                g_timerID = 0;
            }
        }
        break;
    }

    case WM_DESTROY:
        if (hLogoStatic) { DestroyWindow(hLogoStatic); hLogoStatic = NULL; }
        if (hLogoBmp) { DeleteObject(hLogoBmp); hLogoBmp = NULL; }
        PostQuitMessage(0);
        break;
    default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// 辅助函数：计算图片绘制位置（占满高度，宽度自适应，居中）
static void CalcImagePos(HWND hWnd, HBITMAP hBmp, int* outX, int* outY, int* outW, int* outH)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    int winWidth = rc.right - rc.left;
    int winHeight = rc.bottom - rc.top;
    if (hBmp == NULL) {
        *outX = *outY = *outW = *outH = 0;
        return;
    }
    BITMAP bm;
    GetObject(hBmp, sizeof(BITMAP), &bm);
    int imgWidth = bm.bmWidth;
    int imgHeight = bm.bmHeight;

    int targetHeight = winHeight;
    int targetWidth = (int)((float)imgWidth / imgHeight * targetHeight);
    if (targetWidth > winWidth) {
        targetWidth = winWidth;
        targetHeight = (int)((float)imgHeight / imgWidth * targetWidth);
    }
    *outX = (winWidth - targetWidth) / 2;
    *outY = (winHeight - targetHeight) / 2;
    *outW = targetWidth;
    *outH = targetHeight;
}

// 辅助函数：更新控件位置（使用百分比相对定位，保证在不同DPI下相对位置不变）
static void UpdateControlsPosition(HWND hWnd, int imgX, int imgY, int imgW, int imgH)
{
    if (hTaskLabels[0] == NULL || hCancelBtn == NULL) return;

    // 百分比定位（范围 0~1），相对于图片左上角
    const float label1XPercent = 0.07f;      // 标签 X 起点占图片宽度的 7%
    const float label1YPercent = 0.25f;      // 第一个标签 Y 占图片高度的 25%
    const float label2YPercent = 0.30f;      // 第二个标签 Y 占图片高度的 28%
    const float btnXPercent = 0.95f;         // 按钮 X 起点占图片宽度的 95%（右对齐）
    const float btnYPercent = 0.95f;         // 按钮 Y 起点占图片高度的 95%（底部对齐）

    // 计算实际像素坐标
    int labelX = imgX + (int)(imgW * label1XPercent);
    int label1Y = imgY + (int)(imgH * label1YPercent);
    int label2Y = imgY + (int)(imgH * label2YPercent);
    int btnX = imgX + (int)(imgW * btnXPercent) - SCALE(100);  // 减去按钮宽度，使右边缘对齐
    int btnY = imgY + (int)(imgH * btnYPercent) - SCALE(30);   // 减去按钮高度，使下边缘对齐

    int btnWidth = SCALE(100);
    int btnHeight = SCALE(30);

    // 设置两个标签的位置
    for (int i = 0; i < 2; i++)
    {
        int y = (i == 0) ? label1Y : label2Y;
        int width = imgW - (int)(imgW * label1XPercent) * 2;  // 标签宽度自适应，左右留相同边距
        if (width < 0) width = 0;
        SetWindowPos(hTaskLabels[i], NULL, labelX, y, width, SCALE(40), SWP_NOZORDER);
    }

    // 设置取消按钮位置
    SetWindowPos(hCancelBtn, NULL, btnX, btnY, btnWidth, btnHeight, SWP_NOZORDER);
}



// 显示许可协议对话框，返回 TRUE 表示同意，FALSE 表示拒绝
BOOL ShowLicenseDialog(HWND hParent)
{
    // 协议文本（使用您原有的完整文本，这里仅做示例，请替换为您的实际内容）
    const WCHAR* content =
        L"Vizyn 一键仿历代Win主题包 ——Windows软件须知\r\n\r\n"
        L"尊敬的用户：\r\n"
        L"感谢您选择使用 Vizyn 一键仿历代 Windows 主题包。在使用软件并安装主题前，请您仔细阅读以下全部内容。您的使用行为将视为您已了解并同意本须知的全部条款。\r\n\r\n"
        L"1. 关于主题来源与版权\r\n"
        L"本软件仅为一键应用主题工具，软件内所包含的所有主题、图标及系统美化资源均收集于网络，本软件作者不是相关美化资源的作者。相关主题资源的版权归原作者所有！！\r\n"
        L"软件作者未对资源进行任何形式的二次修改或版权主张，若资源涉及版权问题，请相关权利人及时联系我们，我们将予以处理。\r\n\r\n"
        L"本软件包完全免费且开源，若您是通过付费方式获取的本软件，请立即报警！\r\n\r\n"
        L"2. 使用风险声明\r\n"
        L"修改系统主题（特别是主题，图标包）涉及对系统文件的修改。\r\n"
        L"注意：软件仍在开发中，部分功能可能受限，不保证其稳定性，您运行本软件即视为您已知晓并自愿承担因使用本主题包可能导致的任何系统不稳定、文件丢失或数据损坏的风险。\r\n"
        L"系统文件风险： 由于第三方主题的机制，在少数情况下可能会导致系统显示异常、任务栏无法响应或“开始菜单”失效。\r\n"
        L"商业环境使用风险： 若用户在办公、生产、经营等商业环境中使用本软件，因主题异常导致的系统崩溃、工作效率中断、业务数据丢失或其它商业利益损失，软件作者不承担任何赔偿责任！使用前若您没有看本条款的导致损失的，软件作者同样不承担后果！\r\n\r\n"
        L"3. 使用前建议\r\n"
        L"为了确保您的数据安全及系统可恢复性，请在安装本主题包前务必执行以下操作：\r\n"
        L"  创建系统还原点： 建议在 Windows 设置中手动创建一个还原点，以便在出现问题时一键恢复。\r\n"
        L"  备份重要数据： 虽然主题修改通常不涉及个人文件，但建议您养成定期备份重要数据的习惯。\r\n\r\n"
        L"4.关于商业用途与商业利益的特别声明！！\r\n"
        L"严禁商业用途： 本软件及其中集成的第三方主题资源仅供个人学习、研究与桌面美化交流使用。任何将本软件或相关资源用于商业目的（包括但不限于植入商业软件、捆绑销售、在企业或经营场所内以盈利为目的提供安装服务等）均属侵权行为，所产生的一切法律与经济责任由使用者自行承担。\r\n"
        L"题外话：本软件研发初衷是为了让用户动动手指即可完成美化安装，而并非为商业竞争而生，制作本美化包是软件作者的权利，请某些付费软件包作者不要将其视为潜在的竞争对手！\r\n\r\n"
        L"5. 兼容性说明\r\n"
        L"本主题包主要针对 Windows 10 / （可能适用）Windows 11 系统\r\n"
        L"由于不同系统版本的内核差异，部分效果（如 Aero主题）可能无法完美呈现。\r\n\r\n"
        L"5. 免责条款\r\n"
        L"软件作者仅负责提供主题的一键安装与切换功能，不承担因用户使用本软件而导致的任何直接或间接损失（包括但不限于电脑故障、数据丢失、商业利益损失等）。\r\n\r\n"
        L"本软件仅供个人学习、研究与交流美化技术使用，请勿用于商业用途。请在下载后 24 小时内删除本软件所集成的第三方主题资源，若您喜欢某款主题，建议支持正版或联系原作者。\r\n\r\n"
        L"6. 接受条款\r\n"
        L"若您不同意上述任一条款，请立即停止使用本软件，并删除已下载的安装包。\r\n\r\n"
        L"再次提醒：使用本软件即代表您已同意上述所有条款。\r\n\r\n"
        L"Vizyn 一键仿历代Win主题包-V1.0";

    // 配置 TaskDialog
    TASKDIALOGCONFIG config = { 0 };
    config.cbSize = sizeof(TASKDIALOGCONFIG);
    config.hwndParent = hParent;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"Vizyn一键仿历代Win主题包—用户许可协议";
    config.pszMainInstruction = L"请仔细阅读以下许可协议,您的使用行为将视为您已了解并同意本须知的全部条款";
    config.pszContent = content;
    config.dwCommonButtons = 0;  // 不使用标准按钮，使用自定义
    TASKDIALOG_BUTTON buttons[] = {
        { 100, L"同意" },
        { 101, L"拒绝" }
    };
    config.pButtons = buttons;
    config.cButtons = 2;
    config.nDefaultButton = 100; // 默认拒绝

    int nButton = 0;
    HRESULT hr = TaskDialogIndirect(&config, &nButton, NULL, NULL);
    if (FAILED(hr)) return FALSE;
    return (nButton == 100);
}

// 许可协议窗口过程
LRESULT CALLBACK LicenseWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HWND hEdit = NULL;
    static HWND hBtnAgree = NULL;
    static HWND hBtnDisagree = NULL;
    static HFONT hFont = NULL;

    switch (msg)
    {
    case WM_CREATE:
    {
        hFont = CreateFont(-26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"微软雅黑");

        hEdit = CreateWindowEx(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
            WS_VSCROLL | ES_AUTOVSCROLL,
            10, 10, 560, 350,
            hwnd, (HMENU)(INT_PTR)1001, GetModuleHandle(NULL), NULL);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 设置协议文本
        const WCHAR* licenseText =
            L"Vizyn 一键仿历代Win主题包 ——Windows软件须知\r\n"
            L"\r\n"
            L"尊敬的用户：\r\n"
            L"感谢您选择使用 Vizyn 一键仿历代 Windows 主题包。在使用软件并安装主题前，请您务必仔细阅读以下全部内容！！您的使用行为将视为您已了解并同意本须知的全部条款。\r\n"
            L"\r\n"
            L"1. 关于主题来源与版权\r\n"
            L"本软件仅为一键应用主题工具，软件内所包含的所有主题、图标及系统美化资源均收集于网络，本软件作者不是相关美化资源的作者。相关主题资源的版权归原作者所有！！\r\n"
            L"软件作者未对资源进行任何形式的二次修改或版权主张，若资源涉及版权问题，请相关权利人及时联系我们，我们将予以处理。\r\n"
            L"\r\n"
            L"本软件包完全免费且开源，若您是通过付费方式获取的本软件，请立即报警！\r\n"
            L"\r\n"
            L"2. 使用风险声明\r\n"
            L"修改系统主题（特别是主题，图标包）涉及对系统文件的修改。\r\n"
            L"注意：软件仍在开发中，部分功能可能受限，不保证其稳定性，您运行本软件即视为您已知晓并自愿承担因使用本主题包可能导致的任何系统不稳定、文件丢失或数据损坏的风险。\r\n"
            L"系统文件风险： 由于第三方主题的机制，在少数情况下可能会导致系统显示异常、任务栏无法响应或“开始菜单”失效。\r\n"
            L"商业环境使用风险： 若用户在办公、生产、经营等商业环境中使用本软件，因主题异常导致的系统崩溃、工作效率中断、业务数据丢失或其它商业利益损失，软件作者不承担任何赔偿责任！使用前若您没有看本条款的导致损失的，软件作者同样不承担后果！\r\n"
            L"\r\n"
            L"3. 使用前建议\r\n"
            L"为了确保您的数据安全及系统可恢复性，请在安装本主题包前务必执行以下操作：\r\n"
            L"  创建系统还原点： 建议在 Windows 设置中手动创建一个还原点，以便在出现问题时一键恢复。\r\n"
            L"  备份重要数据： 虽然主题修改通常不涉及个人文件，但建议您养成定期备份重要数据的习惯。\r\n"
            L"\r\n"
            L"4.关于商业用途与商业利益的特别声明！！\r\n"
            L"严禁商业用途： 本软件及其中集成的第三方主题资源仅供个人学习、研究与桌面美化交流使用。任何将本软件或相关资源用于商业目的（包括但不限于植入商业软件、捆绑销售、在企业或经营场所内以盈利为目的提供安装服务等）均属侵权行为，所产生的一切法律与经济责任由使用者自行承担。\r\n"
            L"题外话：本软件研发初衷是为了让用户动动手指即可完成美化安装，而并非为商业竞争而生，制作本美化包是软件作者的权利，请某些付费软件包作者不要将其视为潜在的竞争对手！\r\n"
            L"\r\n"
            L"5. 兼容性说明\r\n"
            L"本主题包主要针对 Windows 10 / （可能适用）Windows 11 系统\r\n"
            L"由于不同系统版本（如 22H2, 23H2, 24H2 等）的内核差异，部分效果（如 Aero 毛玻璃、资源管理器背景）可能无法完美呈现，或以替代效果呈现。\r\n"
            L"\r\n"
            L"5. 免责条款\r\n"
            L"软件作者仅负责提供主题的一键安装与切换功能，不承担因用户使用本软件而导致的任何直接或间接损失（包括但不限于电脑故障、数据丢失、商业利益损失等）。\r\n"
            L"\r\n"
            L"本软件仅供个人学习、研究与交流美化技术使用，请勿用于商业用途。请在下载后 24 小时内删除本软件所集成的第三方主题资源，若您喜欢某款主题，建议支持正版或联系原作者。\r\n"
            L"\r\n"
            L"6. 接受条款\r\n"
            L"若您不同意上述任一条款，请立即停止使用本软件，并删除已下载的安装包。\r\n"
            L"\r\n"
            L"再次提醒：使用本软件即代表您已同意上述所有条款。\r\n"
            L"\r\n"
            L"Vizyn 一键仿历代主题包";

        SetWindowText(hEdit, licenseText);

        // 创建“同意”按钮
        hBtnAgree = CreateWindow(
            L"BUTTON", L"同意",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            10, 370, 100, 30,
            hwnd, (HMENU)(INT_PTR)1002, GetModuleHandle(NULL), NULL);
        SendMessage(hBtnAgree, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 创建“拒绝”按钮
        hBtnDisagree = CreateWindow(
            L"BUTTON", L"拒绝",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            470, 370, 100, 30,
            hwnd, (HMENU)(INT_PTR)1003, GetModuleHandle(NULL), NULL);
        SendMessage(hBtnDisagree, WM_SETFONT, (WPARAM)hFont, TRUE);

        SetFocus(hBtnDisagree);
        break;
    }

    case WM_SIZE:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;
        if (hEdit)
            SetWindowPos(hEdit, NULL, 10, 10, width - 20, height - 60, SWP_NOZORDER);
        if (hBtnAgree)
            SetWindowPos(hBtnAgree, NULL, 10, height - 40, 100, 30, SWP_NOZORDER);
        if (hBtnDisagree)
            SetWindowPos(hBtnDisagree, NULL, width - 110, height - 40, 100, 30, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case 1002: // 同意
            SetProp(hwnd, L"Result", (HANDLE)TRUE);
            DestroyWindow(hwnd);
            break;
        case 1003: // 拒绝
            SetProp(hwnd, L"Result", (HANDLE)FALSE);
            DestroyWindow(hwnd);
            break;
        }
        break;

    case WM_CLOSE:
        // 用户点击右上角关闭按钮时，视为拒绝
        SetProp(hwnd, L"Result", (HANDLE)FALSE);
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        if (hFont) DeleteObject(hFont);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// 安装窗口过程
LRESULT CALLBACK InstallWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static int themeType = 0;
    static int g_installDpi = 96;
    static HBITMAP hBgBmp = NULL;               // 居中图片 (install.bmp)
    static HBITMAP hBgWallpaper = NULL;         // 背景墙纸 (wallpaper.bmp)
    static HFONT hNormalFont = NULL;
    static HFONT hBoldFont = NULL;
    static HFONT hBtnFont = NULL;
    static int currentStep = 0;
    static int imgX = 0, imgY = 0, imgW = 0, imgH = 0; // 图片绘制位置

    switch (msg)
    {
    case WM_CREATE:
    {
        themeType = (int)(INT_PTR)((LPCREATESTRUCT)lParam)->lpCreateParams;
        g_installDpi = GetDpiForWindow(hwnd);
        if (g_installDpi == 0) g_installDpi = GetDpiForSystem();

        // 加载图片
        hBgBmp = (HBITMAP)LoadImage(GetModuleHandle(NULL),
            MAKEINTRESOURCE(IDB_INSTALL_BG), IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
        hBgWallpaper = (HBITMAP)LoadImage(GetModuleHandle(NULL),
            MAKEINTRESOURCE(IDB_BG_WALLPAPER), IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);

        // 计算图片位置
        CalcImagePos(hwnd, hBgBmp, &imgX, &imgY, &imgW, &imgH);

        // 创建字体
        hNormalFont = CreateFont(SCALE(-20), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"微软雅黑");
        hBoldFont = CreateFont(SCALE(-20), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"微软雅黑");
        hBtnFont = CreateFont(SCALE(-16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"微软雅黑");

        // 创建两个步骤标签（先创建，位置由UpdateControlsPosition稍后设置）
        for (int i = 0; i < 2; i++)
        {
            hTaskLabels[i] = CreateWindowEx(0, L"STATIC", batSteps[i],
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0, 0, 1, 1,  // 临时尺寸
                hwnd, (HMENU)(3000 + i), GetModuleHandle(NULL), NULL);
            SendMessage(hTaskLabels[i], WM_SETFONT, (WPARAM)hNormalFont, TRUE);
        }
        SendMessage(hTaskLabels[0], WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        currentStep = 0;

        // 创建取消按钮
        hCancelBtn = CreateWindowEx(0, L"BUTTON", L"取消部署",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
            0, 0, 1, 1,
            hwnd, (HMENU)2002, GetModuleHandle(NULL), NULL);
        SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)hBtnFont, TRUE);
        SetWindowTheme(hCancelBtn, L"Explorer", NULL);

        // 根据图片位置设置控件位置
        UpdateControlsPosition(hwnd, imgX, imgY, imgW, imgH);

        // 启动定时器
        g_timerID = SetTimer(hwnd, 1001, 6000, NULL);
        break;
    }

    case WM_TIMER:
        if (wParam == 1001)
        {
            if (currentStep < 1)
            {
                currentStep++;
                SendMessage(hwnd, WM_UPDATE_TASK_STEP, currentStep, 0);
            }
            else
            {
                KillTimer(hwnd, g_timerID);
                g_timerID = 0;
            }
        }
        break;

    case WM_UPDATE_TASK_STEP:
    {
        int stepIndex = (int)wParam;
        for (int i = 0; i < 2; i++)
            SendMessage(hTaskLabels[i], WM_SETFONT, (WPARAM)hNormalFont, TRUE);
        SendMessage(hTaskLabels[stepIndex], WM_SETFONT, (WPARAM)hBoldFont, TRUE);
        break;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        int winWidth = rc.right - rc.left;
        int winHeight = rc.bottom - rc.top;

        // 1. 绘制背景墙纸（拉伸铺满）
        if (hBgWallpaper != NULL)
        {
            BITMAP bm;
            GetObject(hBgWallpaper, sizeof(BITMAP), &bm);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HGDIOBJ hOldBmp = SelectObject(hdcMem, hBgWallpaper);
            StretchBlt(hdc, 0, 0, winWidth, winHeight,
                hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(hdcMem, hOldBmp);
            DeleteDC(hdcMem);
        }
        else
        {
            HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 255));
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
        }

        // 2. 绘制居中图片
        if (hBgBmp != NULL && imgW > 0 && imgH > 0)
        {
            BITMAP bm;
            GetObject(hBgBmp, sizeof(BITMAP), &bm);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HGDIOBJ hOldBmp = SelectObject(hdcMem, hBgBmp);
            StretchBlt(hdc, imgX, imgY, imgW, imgH,
                hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(hdcMem, hOldBmp);
            DeleteDC(hdcMem);
        }

        return 1;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 2002)
        {
            // 获取按钮当前文字
            WCHAR szText[64];
            GetWindowText(hCancelBtn, szText, 64);
            if (wcscmp(szText, L"关闭") == 0)
            {
                // 如果是“关闭”按钮，直接销毁窗口
                DestroyWindow(hwnd);
            }
            else
            {
                int r = MessageBox(hwnd, L"确定取消部署吗？", L"确认", MB_YESNO | MB_ICONQUESTION);
                if (r == IDYES) CancelDeployment(hwnd);
            }
        }
        break;

    case WM_DPICHANGED:
    {
        g_installDpi = HIWORD(wParam);
        RECT* prc = (RECT*)lParam;
        SetWindowPos(hwnd, NULL, prc->left, prc->top,
            prc->right - prc->left, prc->bottom - prc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        // 重新计算图片位置
        CalcImagePos(hwnd, hBgBmp, &imgX, &imgY, &imgW, &imgH);

        // 更新控件位置
        UpdateControlsPosition(hwnd, imgX, imgY, imgW, imgH);

        // 强制重绘
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }

    case WM_DESTROY:
        if (g_timerID) { KillTimer(hwnd, g_timerID); g_timerID = 0; }
        if (hBgBmp) { DeleteObject(hBgBmp); hBgBmp = NULL; }
        if (hBgWallpaper) { DeleteObject(hBgWallpaper); hBgWallpaper = NULL; }
        if (hCancelBtn) { DestroyWindow(hCancelBtn); hCancelBtn = NULL; }
        for (int i = 0; i < 2; i++)
            if (hTaskLabels[i]) { DestroyWindow(hTaskLabels[i]); hTaskLabels[i] = NULL; }
        if (hNormalFont) { DeleteObject(hNormalFont); hNormalFont = NULL; }
        if (hBoldFont) { DeleteObject(hBoldFont); hBoldFont = NULL; }
        if (hBtnFont) { DeleteObject(hBtnFont); hBtnFont = NULL; }
        hInstallWnd = NULL;
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}