// HelperComponents.cpp - 32-bit bridge, notifications, and error logging
#include "EnhancedVSTHost.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <VersionHelpers.h>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace Microsoft::WRL;

// 32-bit Plugin Bridge Implementation
PluginBridge32::PluginBridge32() {
}

PluginBridge32::~PluginBridge32() {
    shutdown();
}

bool PluginBridge32::initialize() {
    // Check if 32-bit bridge process exists
    if (!PathFileExistsW(L"VSTBridge32.exe")) {
        // 32-bit bridge not available
        return false;
    }
    
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
        
        // Wait for process to exit gracefully
        if (WaitForSingleObject(bridgeProcess, 5000) == WAIT_TIMEOUT) {
            // Force termination
            TerminateProcess(bridgeProcess, 0);
        }
        
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
    // For now, just pass through
    for (int ch = 0; ch < 2; ++ch) {
        if (inputs && inputs[ch] && outputs && outputs[ch]) {
            std::copy_n(inputs[ch], numSamples, outputs[ch]);
        }
    }
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
        // This requires Windows Runtime and additional setup
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
    nid.uFlags = NIF_INFO | NIF_ICON;
    nid.hIcon = LoadIcon(nullptr, IDI_WARNING);
    nid.dwInfoFlags = NIIF_WARNING;
    
    wcscpy_s(nid.szInfoTitle, title.substr(0, 63).c_str());
    wcscpy_s(nid.szInfo, message.substr(0, 255).c_str());
    
    // Add the icon first time
    Shell_NotifyIconW(NIM_ADD, &nid);
    
    // Update with notification
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    
    // Remove after a delay (in production, would handle this better)
    Sleep(5000);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Error Logger Implementation
ErrorLogger::ErrorLogger(const std::wstring& logPath) 
    : logFilePath(logPath) {
    
    // Open log file in append mode
    logFile.open(logPath, std::ios::app | std::ios::out);
    
    if (logFile.is_open()) {
        logFile << L"\n=== VST Host Started " << getCurrentTimestamp() << L" ===\n";
        logFile.flush();
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
    while (recentErrors.size() > 1000) {
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

std::vector<std::wstring> ErrorLogger::getRecentErrors(int count) const {
    std::lock_guard<std::mutex> lock(logMutex);
    
    std::vector<std::wstring> errors;
    
    // Create a copy of the queue to iterate
    std::queue<std::wstring> tempQueue = recentErrors;
    
    // Skip older entries if we have more than requested
    int toSkip = static_cast<int>(tempQueue.size()) - count;
    while (toSkip > 0 && !tempQueue.empty()) {
        tempQueue.pop();
        toSkip--;
    }
    
    // Collect the requested number of errors
    while (!tempQueue.empty() && errors.size() < static_cast<size_t>(count)) {
        errors.push_back(tempQueue.front());
        tempQueue.pop();
    }
    
    return errors;
}

void ErrorLogger::clearLog() {
    std::lock_guard<std::mutex> lock(logMutex);
    
    // Clear recent errors
    while (!recentErrors.empty()) {
        recentErrors.pop();
    }
    
    // Clear log file
    if (logFile.is_open()) {
        logFile.close();
    }
    
    // Reopen in truncate mode
    logFile.open(logFilePath, std::ios::trunc | std::ios::out);
    if (logFile.is_open()) {
        logFile << L"=== Log Cleared " << getCurrentTimestamp() << L" ===\n";
        logFile.flush();
    }
}

std::wstring ErrorLogger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);
    
    std::wstringstream ss;
    ss << std::put_time(&timeinfo, L"%Y-%m-%d %H:%M:%S");
    
    return ss.str();
}