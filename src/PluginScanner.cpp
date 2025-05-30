// PluginScanner.cpp - Plugin scanner with crash isolation
#include "EnhancedVSTHost.h"
#include <windows.h>
#include <shlwapi.h>
#include <filesystem>
#include <chrono>
#include <sstream>

namespace fs = std::filesystem;

// Helper process for scanning plugins in isolation
const wchar_t* SCANNER_PROCESS_NAME = L"VSTScanner.exe";

PluginScanner::PluginScanner() {
}

PluginScanner::~PluginScanner() {
    // Terminate any remaining scanner processes
    for (auto& job : activeJobs) {
        if (job.processHandle != INVALID_HANDLE_VALUE) {
            TerminateProcess(job.processHandle, 1);
            CloseHandle(job.processHandle);
        }
        if (job.pipeHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(job.pipeHandle);
        }
    }
}

void PluginScanner::scanDirectory(const std::wstring& path,
                                 std::function<void(const EVH::PluginInfo&)> onPluginFound,
                                 std::function<void(int, int, const std::wstring&)> onProgress) {
    
    std::vector<std::wstring> pluginFiles;
    
    // Find all DLL and VST3 files in the directory
    try {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                if (ext == L".dll" || ext == L".vst3") {
                    pluginFiles.push_back(entry.path().wstring());
                }
            }
        }
    } catch (const std::exception&) {
        // Directory access error
        return;
    }
    
    // Scan each plugin
    int current = 0;
    for (const auto& pluginPath : pluginFiles) {
        current++;
        
        if (onProgress) {
            onProgress(current, static_cast<int>(pluginFiles.size()), pluginPath);
        }
        
        EVH::PluginInfo info;
        if (scanPluginInProcess(pluginPath, info)) {
            if (onPluginFound) {
                onPluginFound(info);
            }
        }
        
        // Check for hung processes periodically
        if (current % 10 == 0) {
            terminateHungProcesses();
        }
    }
    
    // Final cleanup
    terminateHungProcesses();
}

bool PluginScanner::scanPluginInProcess(const std::wstring& path, EVH::PluginInfo& info) {
    // For now, do a simple in-process scan
    // In a production system, this should launch a separate process
    
    // Basic info
    info.path = path;
    info.validated = false;
    
    // Check file extension
    std::wstring ext = PathFindExtensionW(path.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == L".vst3") {
        info.type = EVH::PluginType::VST3;
    } else if (ext == L".dll") {
        // Could be VST2 or VST3
        info.type = EVH::PluginType::Unknown;
    } else {
        return false;
    }
    
    // Extract plugin name from filename
    wchar_t filename[MAX_PATH];
    wcscpy_s(filename, PathFindFileNameW(path.c_str()));
    PathRemoveExtensionW(filename);
    info.name = filename;
    
    // Set default values
    info.vendor = L"Unknown";
    info.is64Bit = (sizeof(void*) == 8);
    info.hasCustomEditor = true;  // Assume true
    info.numInputs = 2;
    info.numOutputs = 2;
    info.uniqueId = 0;
    info.isInstrument = false;
    
    // In a real implementation, we would launch a separate process here
    // For now, just do basic validation
    HMODULE hModule = LoadLibraryExW(path.c_str(), nullptr, 
                                     DONT_RESOLVE_DLL_REFERENCES | 
                                     LOAD_LIBRARY_AS_DATAFILE);
    if (hModule) {
        FreeLibrary(hModule);
        info.validated = true;
        return true;
    }
    
    info.errorMsg = L"Failed to load plugin module";
    return false;
}

bool PluginScanner::launchScannerProcess(const std::wstring& pluginPath, ScanJob& job) {
    // Create pipe for communication
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return false;
    }
    
    // Make read handle non-inheritable
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    
    // Prepare command line
    std::wstringstream cmdLine;
    cmdLine << L"\"" << SCANNER_PROCESS_NAME << L"\" \"" << pluginPath << L"\"";
    
    // Setup startup info
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;
    
    // Create process
    PROCESS_INFORMATION pi;
    BOOL success = CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmdLine.str().c_str()),
        nullptr,
        nullptr,
        TRUE,  // Inherit handles
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    
    // Close write end of pipe in parent process
    CloseHandle(hWritePipe);
    
    if (!success) {
        CloseHandle(hReadPipe);
        return false;
    }
    
    // Close thread handle (not needed)
    CloseHandle(pi.hThread);
    
    // Store handles
    job.processHandle = pi.hProcess;
    job.pipeHandle = hReadPipe;
    job.startTime = std::chrono::steady_clock::now();
    
    return true;
}

bool PluginScanner::readScanResult(HANDLE pipe, EVH::PluginInfo& info) {
    std::string buffer;
    char readBuffer[4096];
    DWORD bytesRead;
    
    // Read all data from pipe
    while (ReadFile(pipe, readBuffer, sizeof(readBuffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        readBuffer[bytesRead] = '\0';
        buffer += readBuffer;
    }
    
    // Parse the result (simple key-value format)
    std::istringstream stream(buffer);
    std::string line;
    
    while (std::getline(stream, line)) {
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            if (key == "path") {
                int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
                if (len > 0) {
                    info.path.resize(len - 1);
                    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &info.path[0], len);
                }
            } else if (key == "name") {
                int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
                if (len > 0) {
                    info.name.resize(len - 1);
                    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &info.name[0], len);
                }
            } else if (key == "vendor") {
                int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
                if (len > 0) {
                    info.vendor.resize(len - 1);
                    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &info.vendor[0], len);
                }
            } else if (key == "type") {
                if (value == "VST3") {
                    info.type = EVH::PluginType::VST3;
                } else if (value == "VST2") {
                    info.type = EVH::PluginType::VST2;
                } else {
                    info.type = EVH::PluginType::Unknown;
                }
            } else if (key == "is64Bit") {
                info.is64Bit = (value == "true");
            } else if (key == "hasEditor") {
                info.hasCustomEditor = (value == "true");
            } else if (key == "numInputs") {
                info.numInputs = std::stoi(value);
            } else if (key == "numOutputs") {
                info.numOutputs = std::stoi(value);
            } else if (key == "uniqueId") {
                info.uniqueId = static_cast<uint32_t>(std::stoul(value));
            } else if (key == "isInstrument") {
                info.isInstrument = (value == "true");
            } else if (key == "validated") {
                info.validated = (value == "true");
            } else if (key == "error") {
                int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
                if (len > 0) {
                    info.errorMsg.resize(len - 1);
                    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &info.errorMsg[0], len);
                }
                return false;
            }
        }
    }
    
    return info.validated;
}

void PluginScanner::terminateHungProcesses() {
    std::lock_guard<std::mutex> lock(jobMutex);
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = activeJobs.begin(); it != activeJobs.end(); ) {
        auto elapsed = now - it->startTime;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 
            EVH::MAX_PLUGIN_SCAN_TIME_MS) {
            
            // Terminate hung process
            TerminateProcess(it->processHandle, 1);
            CloseHandle(it->processHandle);
            CloseHandle(it->pipeHandle);
            
            it = activeJobs.erase(it);
        } else {
            ++it;
        }
    }
}

// Scanner process implementation (separate executable)
// This would be compiled as a separate VSTScanner.exe
#ifdef BUILD_SCANNER_PROCESS

#include <iostream>
#include <windows.h>
#include <string>

// Simple VST3 scanner entry point
int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        std::wcerr << L"Usage: VSTScanner.exe <plugin_path>" << std::endl;
        return 1;
    }
    
    const wchar_t* pluginPath = argv[1];
    
    // Set up structured exception handling
    __try {
        // Load the plugin
        HMODULE hModule = LoadLibraryW(pluginPath);
        if (!hModule) {
            std::cout << "error=Failed to load plugin DLL" << std::endl;
            return 1;
        }
        
        // Convert path to UTF-8 for output
        int pathLen = WideCharToMultiByte(CP_UTF8, 0, pluginPath, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8Path(pathLen - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, pluginPath, -1, &utf8Path[0], pathLen, nullptr, nullptr);
        
        std::cout << "path=" << utf8Path << std::endl;
        
        // Check for VST3 entry point
        typedef void* (*GetPluginFactory)();
        GetPluginFactory getFactory = (GetPluginFactory)GetProcAddress(hModule, "GetPluginFactory");
        
        if (getFactory) {
            // VST3 plugin
            std::cout << "type=VST3" << std::endl;
            std::cout << "is64Bit=" << (sizeof(void*) == 8 ? "true" : "false") << std::endl;
            
            // Try to get factory
            void* factory = getFactory();
            if (factory) {
                // In a real implementation, would query the factory for plugin info
                std::cout << "name=VST3 Plugin" << std::endl;
                std::cout << "vendor=Unknown" << std::endl;
                std::cout << "numInputs=2" << std::endl;
                std::cout << "numOutputs=2" << std::endl;
                std::cout << "hasEditor=true" << std::endl;
                std::cout << "isInstrument=false" << std::endl;
                std::cout << "uniqueId=0" << std::endl;
                std::cout << "validated=true" << std::endl;
            } else {
                std::cout << "error=Failed to get plugin factory" << std::endl;
                FreeLibrary(hModule);
                return 1;
            }
        } else {
            // Not a VST3 plugin
            std::cout << "error=Not a VST3 plugin (GetPluginFactory not found)" << std::endl;
            FreeLibrary(hModule);
            return 1;
        }
        
        FreeLibrary(hModule);
        return 0;
        
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        std::cout << "error=Plugin crashed during scanning" << std::endl;
        return 1;
    }
}

#endif // BUILD_SCANNER_PROCESS