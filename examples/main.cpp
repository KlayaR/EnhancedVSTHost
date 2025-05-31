// main.cpp - Example VST Host Application
#include "EnhancedVSTHost.h"
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Window class name
const wchar_t* WINDOW_CLASS_NAME = L"EnhancedVSTHostWindow";

// Control IDs
enum {
    ID_SCAN_BUTTON = 1001,
    ID_LOAD_BUTTON,
    ID_UNLOAD_BUTTON,
    ID_START_AUDIO_BUTTON,
    ID_STOP_AUDIO_BUTTON,
    ID_PLUGIN_LIST,
    ID_LOG_VIEW,
    ID_BYPASS_CHECK,
    ID_DRIVER_COMBO,
    ID_SAMPLE_RATE_COMBO,
    ID_BUFFER_SIZE_COMBO,
    ID_STATUS_BAR
};

// Global variables
HWND g_hWnd = nullptr;
HWND g_hPluginList = nullptr;
HWND g_hLogView = nullptr;
HWND g_hStatusBar = nullptr;
std::unique_ptr<EnhancedVSTHost> g_vstHost;
std::vector<EVH::PluginInfo> g_availablePlugins;
int g_selectedPluginId = -1;

// Function declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CreateControls(HWND hwnd);
void OnScanPlugins();
void OnLoadPlugin();
void OnUnloadPlugin();
void OnStartAudio();
void OnStopAudio();
void UpdatePluginList();
void UpdateLog();
void SetStatusText(const std::wstring& text);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                    LPWSTR lpCmdLine, int nCmdShow) {
    
    // Initialize Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);
    
    // Register window class
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    
    RegisterClassExW(&wc);
    
    // Create main window
    g_hWnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        L"Enhanced VST Host",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );
    
    if (!g_hWnd) {
        return 0;
    }
    
    // Create VST host
    g_vstHost = std::make_unique<EnhancedVSTHost>();
    if (!g_vstHost->initialize(g_hWnd)) {
        MessageBoxW(g_hWnd, L"Failed to initialize VST Host", L"Error", MB_OK | MB_ICONERROR);
        return 0;
    }
    
    // Set callbacks
    g_vstHost->setScanProgressCallback([](int current, int total, const std::wstring& plugin) {
        std::wstring status = L"Scanning: " + std::to_wstring(current) + L"/" + 
                             std::to_wstring(total) + L" - " + plugin;
        SetStatusText(status);
    });
    
    g_vstHost->setErrorCallback([](const std::wstring& error) {
        UpdateLog();
    });
    
    g_vstHost->setCrashCallback([](int pluginId, const std::wstring& pluginName) {
        std::wstring msg = L"Plugin crashed: " + pluginName;
        MessageBoxW(g_hWnd, msg.c_str(), L"Plugin Crash", MB_OK | MB_ICONWARNING);
        UpdatePluginList();
    });
    
    // Show window
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    
    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    g_vstHost.reset();
    
    return (int)msg.wParam;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            CreateControls(hwnd);
            return 0;
            
        case WM_SIZE:
            // Resize controls
            if (g_hStatusBar) {
                SendMessage(g_hStatusBar, WM_SIZE, 0, 0);
            }
            return 0;
            
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_SCAN_BUTTON:
                    OnScanPlugins();
                    break;
                    
                case ID_LOAD_BUTTON:
                    OnLoadPlugin();
                    break;
                    
                case ID_UNLOAD_BUTTON:
                    OnUnloadPlugin();
                    break;
                    
                case ID_START_AUDIO_BUTTON:
                    OnStartAudio();
                    break;
                    
                case ID_STOP_AUDIO_BUTTON:
                    OnStopAudio();
                    break;
                    
                case ID_BYPASS_CHECK:
                    if (g_selectedPluginId >= 0) {
                        bool bypass = SendMessage((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        g_vstHost->bypassPlugin(g_selectedPluginId, bypass);
                    }
                    break;
            }
            return 0;
            
        case WM_NOTIFY:
            {
                LPNMHDR pnmh = (LPNMHDR)lParam;
                if (pnmh->idFrom == ID_PLUGIN_LIST && pnmh->code == LVN_ITEMCHANGED) {
                    LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
                    if (pnmv->uNewState & LVIS_SELECTED) {
                        g_selectedPluginId = (int)pnmv->lParam;
                    }
                }
            }
            return 0;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void CreateControls(HWND hwnd) {
    // Create toolbar area
    HWND hToolbar = CreateWindowW(
        L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        0, 0, 1024, 60,
        hwnd, nullptr, nullptr, nullptr
    );
    
    // Scan button
    CreateWindowW(
        L"BUTTON", L"Scan Plugins",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 10, 100, 30,
        hwnd, (HMENU)ID_SCAN_BUTTON, nullptr, nullptr
    );
    
    // Load button
    CreateWindowW(
        L"BUTTON", L"Load Plugin",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        120, 10, 100, 30,
        hwnd, (HMENU)ID_LOAD_BUTTON, nullptr, nullptr
    );
    
    // Unload button
    CreateWindowW(
        L"BUTTON", L"Unload Plugin",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        230, 10, 100, 30,
        hwnd, (HMENU)ID_UNLOAD_BUTTON, nullptr, nullptr
    );
    
    // Start Audio button
    CreateWindowW(
        L"BUTTON", L"Start Audio",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        350, 10, 100, 30,
        hwnd, (HMENU)ID_START_AUDIO_BUTTON, nullptr, nullptr
    );
    
    // Stop Audio button
    CreateWindowW(
        L"BUTTON", L"Stop Audio",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        460, 10, 100, 30,
        hwnd, (HMENU)ID_STOP_AUDIO_BUTTON, nullptr, nullptr
    );
    
    // Bypass checkbox
    CreateWindowW(
        L"BUTTON", L"Bypass",
        WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
        580, 15, 80, 20,
        hwnd, (HMENU)ID_BYPASS_CHECK, nullptr, nullptr
    );
    
    // Driver selection
    CreateWindowW(
        L"STATIC", L"Audio Driver:",
        WS_CHILD | WS_VISIBLE,
        680, 15, 80, 20,
        hwnd, nullptr, nullptr, nullptr
    );
    
    HWND hDriverCombo = CreateWindowW(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        770, 10, 100, 200,
        hwnd, (HMENU)ID_DRIVER_COMBO, nullptr, nullptr
    );
    
    SendMessageW(hDriverCombo, CB_ADDSTRING, 0, (LPARAM)L"WASAPI");
    SendMessageW(hDriverCombo, CB_SETCURSEL, 0, 0);
    
    // Plugin list
    g_hPluginList = CreateWindowW(
        WC_LISTVIEW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
        10, 70, 1000, 300,
        hwnd, (HMENU)ID_PLUGIN_LIST, nullptr, nullptr
    );
    
    // Setup list columns
    LVCOLUMN lvc;
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    
    lvc.iSubItem = 0;
    lvc.pszText = (LPWSTR)L"Plugin Name";
    lvc.cx = 300;
    ListView_InsertColumn(g_hPluginList, 0, &lvc);
    
    lvc.iSubItem = 1;
    lvc.pszText = (LPWSTR)L"Vendor";
    lvc.cx = 200;
    ListView_InsertColumn(g_hPluginList, 1, &lvc);
    
    lvc.iSubItem = 2;
    lvc.pszText = (LPWSTR)L"Type";
    lvc.cx = 100;
    ListView_InsertColumn(g_hPluginList, 2, &lvc);
    
    lvc.iSubItem = 3;
    lvc.pszText = (LPWSTR)L"Status";
    lvc.cx = 100;
    ListView_InsertColumn(g_hPluginList, 3, &lvc);
    
    // Log view
    CreateWindowW(
        L"STATIC", L"Log:",
        WS_CHILD | WS_VISIBLE,
        10, 380, 50, 20,
        hwnd, nullptr, nullptr, nullptr
    );
    
    g_hLogView = CreateWindowW(
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | 
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        10, 400, 1000, 200,
        hwnd, (HMENU)ID_LOG_VIEW, nullptr, nullptr
    );
    
    // Status bar
    g_hStatusBar = CreateWindowW(
        STATUSCLASSNAME, L"Ready",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        hwnd, (HMENU)ID_STATUS_BAR, nullptr, nullptr
    );
}

void OnScanPlugins() {
    // Get VST plugin paths
    std::vector<std::wstring> searchPaths;
    
    // Common VST3 paths
    wchar_t programFiles[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, programFiles) == S_OK) {
        searchPaths.push_back(std::wstring(programFiles) + L"\\Common Files\\VST3");
    }
    
    wchar_t programFilesX86[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, programFilesX86) == S_OK) {
        searchPaths.push_back(std::wstring(programFilesX86) + L"\\Common Files\\VST3");
    }
    
    // User VST3 folder
    wchar_t appData[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData) == S_OK) {
        searchPaths.push_back(std::wstring(appData) + L"\\VST3");
    }
    
    // Start scanning in a separate thread
    std::thread scanThread([searchPaths]() {
        g_vstHost->scanPlugins(searchPaths);
        
        // Update UI on main thread
        PostMessage(g_hWnd, WM_COMMAND, MAKEWPARAM(0, 0), 0);
    });
    
    scanThread.detach();
}

void OnLoadPlugin() {
    // Simple file open dialog
    wchar_t filename[MAX_PATH] = L"";
    
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L"VST3 Plugins\0*.vst3\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    
    if (GetOpenFileNameW(&ofn)) {
        int pluginId = g_vstHost->loadPlugin(filename);
        if (pluginId > 0) {
            g_selectedPluginId = pluginId;
            g_vstHost->addPluginToChain(pluginId);
            SetStatusText(L"Plugin loaded successfully");
            UpdatePluginList();
        } else {
            MessageBoxW(g_hWnd, L"Failed to load plugin", L"Error", MB_OK | MB_ICONERROR);
        }
    }
}

void OnUnloadPlugin() {
    if (g_selectedPluginId >= 0) {
        g_vstHost->unloadPlugin(g_selectedPluginId);
        g_selectedPluginId = -1;
        UpdatePluginList();
        SetStatusText(L"Plugin unloaded");
    }
}

void OnStartAudio() {
    if (g_vstHost->startAudio(EVH::AudioDriverType::WASAPI)) {
        SetStatusText(L"Audio started");
    } else {
        MessageBoxW(g_hWnd, L"Failed to start audio", L"Error", MB_OK | MB_ICONERROR);
    }
}

void OnStopAudio() {
    g_vstHost->stopAudio();
    SetStatusText(L"Audio stopped");
}

void UpdatePluginList() {
    ListView_DeleteAllItems(g_hPluginList);
    
    // Get available plugins
    g_availablePlugins = g_vstHost->getAvailablePlugins();
    
    // Add each plugin to the list
    for (size_t i = 0; i < g_availablePlugins.size(); ++i) {
        const auto& plugin = g_availablePlugins[i];
        
        LVITEM lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = static_cast<int>(i);
        lvi.lParam = static_cast<LPARAM>(i);
        lvi.pszText = const_cast<LPWSTR>(plugin.name.c_str());
        
        int index = ListView_InsertItem(g_hPluginList, &lvi);
        
        ListView_SetItemText(g_hPluginList, index, 1, const_cast<LPWSTR>(plugin.vendor.c_str()));
        
        const wchar_t* typeStr = plugin.type == EVH::PluginType::VST3 ? L"VST3" : L"Unknown";
        ListView_SetItemText(g_hPluginList, index, 2, const_cast<LPWSTR>(typeStr));
        
        const wchar_t* statusStr = plugin.validated ? L"OK" : L"Error";
        ListView_SetItemText(g_hPluginList, index, 3, const_cast<LPWSTR>(statusStr));
    }
}

void UpdateLog() {
    auto errors = g_vstHost->getRecentErrors();
    
    std::wstring logText;
    for (const auto& error : errors) {
        logText += error + L"\r\n";
    }
    
    SetWindowTextW(g_hLogView, logText.c_str());
    
    // Scroll to bottom
    int textLength = GetWindowTextLengthW(g_hLogView);
    SendMessageW(g_hLogView, EM_SETSEL, textLength, textLength);
    SendMessageW(g_hLogView, EM_SCROLLCARET, 0, 0);
}

void SetStatusText(const std::wstring& text) {
    if (g_hStatusBar) {
        SendMessageW(g_hStatusBar, SB_SETTEXT, 0, (LPARAM)text.c_str());
    }
}