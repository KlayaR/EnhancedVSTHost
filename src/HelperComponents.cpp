// HelperComponents.cpp - 32-bit bridge, notifications, and error logging
#include "EnhancedVSTHost.h"
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <shlobj.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <NotificationActivationCallback.h>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "runtimeobject.lib")

using namespace Microsoft::WRL;

// 32-bit Plugin Bridge Implementation
PluginBridge32::PluginBridge32() {
}

PluginBridge32::~PluginBridge32() {
    shutdown();
}

bool PluginBridge32::initialize() {
    // Create bridge process
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    
    // Create command pipe
    HANDLE hReadCmd, hWriteCmd;
    if (!CreatePipe(&hReadCmd, &hWriteCmd, &sa, 0)) {
        return false;
    }
    SetHandleInformation(hReadCmd, HANDLE_FLAG_INHERIT, 0);
    
    // Create data pipe
    HANDLE hReadData, hWriteData;
    if (!CreatePipe(&hReadData, &hWriteData, &sa, 0)) {
        CloseHandle(hReadCmd);
        CloseHandle(hWriteCmd);
        return false;
    }
    SetHandleInformation(hWriteData, HANDLE_FLAG_INHERIT, 0);
    
    // Start bridge process
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = hWriteCmd;
    si.hStdOutput = hReadData;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.wShowWindow = SW_HIDE;
    
    PROCESS_INFORMATION pi;
    std::wstring cmdLine = L"VSTBridge32.exe";
    
    BOOL success = CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmdLine.c_str()),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    
    // Close handles we don't need
    CloseHandle(hWriteCmd);
    CloseHandle(hReadData);
    
    if (!success) {
        CloseHandle(hReadCmd);
        CloseHandle(hWriteData);
        return false;
    }
    
    // Store handles
    bridgeProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    commandPipe = hWriteData;
    dataPipe = hReadCmd;
    
    // Send initialization command
    return sendCommand("INIT");
}

void PluginBridge32::shutdown() {
    if (bridgeProcess != nullptr) {
        sendCommand("EXIT");
        WaitForSingleObject(bridgeProcess, 5000);
        TerminateProcess(bridgeProcess, 0);
        CloseHandle(bridgeProcess);
        bridgeProcess = nullptr;
    }
    
    if (commandPipe != nullptr) {
        CloseHandle(commandPipe);
        commandPipe = nullptr;
    }
    
    if (dataPipe != nullptr) {
        CloseHandle(dataPipe);
        dataPipe = nullptr;
    }
}

bool PluginBridge32::loadPlugin32(const std::wstring& path, EVH::PluginInfo& info) {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    
    // Convert path to UTF-8
    int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Path(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &utf8Path[0], size, nullptr, nullptr);
    
    // Send load command
    std::string cmd = "LOAD " + utf8Path;
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // Read response
    std::string response;
    if (!receiveResponse(response)) {
        return false;
    }
    
    // Parse response
    return response == "OK";
}

void PluginBridge32::unloadPlugin32(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    
    // Convert path to UTF-8
    int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Path(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &utf8Path[0], size, nullptr, nullptr);
    
    // Send unload command
    std::string cmd = "UNLOAD " + utf8Path;
    sendCommand(cmd);
}

void PluginBridge32::process32(const std::wstring& pluginPath,
                               const float** inputs, float** outputs,
                               int numSamples) {
    std::lock_guard<std::mutex> lock(bridgeMutex);
    
    // This would send audio data to the 32-bit process for processing
    // Implementation would involve shared memory or pipe-based audio transfer
}

bool PluginBridge32::sendCommand(const std::string& cmd) {
    if (!commandPipe) {
        return false;
    }
    
    DWORD bytesWritten;
    std::string cmdWithNewline = cmd + "\n";
    return WriteFile(commandPipe, cmdWithNewline.c_str(), 
                    static_cast<DWORD>(cmdWithNewline.size()), 
                    &bytesWritten, nullptr) != 0;
}

bool PluginBridge32::receiveResponse(std::string& response) {
    if (!dataPipe) {
        return false;
    }
    
    char buffer[4096];
    DWORD bytesRead;
    response.clear();
    
    while (ReadFile(dataPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
        
        // Check for newline
        if (response.find('\n') != std::string::npos) {
            response.erase(response.find('\n'));
            return true;
        }
    }
    
    return false;
}

// Windows Notification Manager Implementation
NotificationManager::NotificationManager(HWND parentWindow) 
    : parentWindow(parentWindow), useToastNotifications(false) {
    
    // Check if we can use Windows 10/11 toast notifications
    if (IsWindows10OrGreater()) {
        initializeToastNotifications();
    }
}

NotificationManager::~NotificationManager() {
}

void NotificationManager::showNotification(const std::wstring& title, const std::wstring& message) {
    if (useToastNotifications) {
        // Use Windows 10/11 toast notifications
        // This requires Windows Runtime and would need additional setup
        // For now, fall back to legacy
    }
    
    showLegacyNotification(title, message);
}

void NotificationManager::showErrorNotification(const std::wstring& error) {
    showNotification(L"VST Host Error", error);
}

void NotificationManager::showPluginCrashNotification(const std::wstring& pluginName) {
    std::wstring message = L"Plugin '" + pluginName + L"' has crashed and been disabled.";
    showNotification(L"Plugin Crash", message);
}

void NotificationManager::initializeToastNotifications() {
    // Initialize Windows Runtime for toast notifications
    // This would require Windows Runtime initialization
    // For this example, we'll use legacy notifications
    useToastNotifications = false;
}

void NotificationManager::showLegacyNotification(const std::wstring& title, const std::wstring& message) {
    // Use system tray notification
    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = parentWindow ? parentWindow : GetDesktopWindow();
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    
    wcscpy_s(nid.szInfoTitle, title.substr(0, 63).c_str());
    wcscpy_s(nid.szInfo, message.substr(0, 255).c_str());
    
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Error Logger Implementation
ErrorLogger::ErrorLogger(const std::wstring& logPath) 
    : logFilePath(logPath) {
    
    // Open log file in append mode
    logFile.open(logPath, std::ios::app | std::ios::out);
    
    if (logFile.is_open()) {
        logFile << L"\n=== VST Host Started " << getCurrentTimestamp() << L" ===\n";
    }
}

ErrorLogger::~ErrorLogger() {
    if (logFile.is_open()) {
        logFile << L"=== VST Host Stopped " << getCurrentTimestamp() << L" ===\n";
        logFile.close();
    }
}

void ErrorLogger::logError(const std::wstring& error) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    std::wstring timestampedError = L"[" + getCurrentTimestamp() + L"] ERROR: " + error;
    
    // Add to recent errors
    recentErrors.push(timestampedError);
    if (recentErrors.size() > 1000) {
        recentErrors.pop();
    }
    
    // Write to file
    if (logFile.is_open()) {
        logFile << timestampedError << std::endl;
        logFile.flush();
    }
}

void ErrorLogger::logPluginCrash(const std::wstring& pluginName, const std::wstring& details) {
    std::wstring error = L"PLUGIN CRASH: " + pluginName + L" - " + details;
    logError(error);
}

void ErrorLogger::logAudioError(const std::wstring& error) {
    std::wstring audioError = L"AUDIO: " + error;
    logError(audioError);
}

std::vector