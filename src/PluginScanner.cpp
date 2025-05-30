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
    
    // Find all DLL files in the directory
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
            onPluginFound(info);
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
    ScanJob job;
    job.path = path;
    
    // Launch scanner process
    if (!launchScannerProcess(path, job)) {
        return false;
    }
    
    // Add to active jobs
    {
        std::lock_guard<std::mutex> lock(jobMutex);
        activeJobs.push_back(job);
    }
    
    // Wait for result with timeout
    bool success = false;
    auto startTime = std::chrono::steady_clock::now();
    
    while (true) {
        // Check if process is still running
        DWORD exitCode;
        if (GetExitCodeProcess(job.processHandle, &exitCode)) {
            if (exitCode != STILL_ACTIVE) {
                // Process finished
                success = (exitCode == 0) && readScanResult(job.pipeHandle, info);
                break;
            }
        }
        
        // Check timeout
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 
            EVH::MAX_PLUGIN_SCAN_TIME_MS) {
            // Timeout - kill the process
            TerminateProcess(job.processHandle, 1);
            break;
        }
        
        // Small delay
        Sleep(50);
    }
    
    // Cleanup
    CloseHandle(job.processHandle);
    CloseHandle(job.pipeHandle);
    
    // Remove from active jobs
    {
        std::lock_guard<std::mutex> lock(jobMutex);
        activeJobs.erase(
            std::remove_if(activeJobs.begin(), activeJobs.end(),
                          [&](const ScanJob& j) { return j.processHandle == job.processHandle; }),
            activeJobs.end()
        );
    }
    
    return success;
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
                info.path = std::wstring(value.begin(), value.end());
            } else if (key == "name") {
                info.name = std::wstring(value.begin(), value.end());
            } else if (key == "vendor") {
                info.vendor = std::wstring(value.begin(), value.end());
            } else if (key == "type") {
                info.type = (value == "VST3") ? EVH::PluginType::VST3 : EVH::PluginType::VST2;
            } else if (key == "is64Bit") {
                info.is64Bit = (value == "true");
            } else if (key == "hasEditor") {
                info.hasCustomEditor = (value == "true");
            } else if (key == "numInputs") {
                info.numInputs = std::stoi(value);
            } else if (key == "numOutputs") {
                info.numOutputs = std::stoi(value);
            } else if (key == "uniqueId") {
                info.uniqueId = std::stoul(value);
            } else if (key == "isInstrument") {
                info.isInstrument = (value == "true");
            } else if (key == "validated") {
                info.validated = (value == "true");
            } else if (key == "error") {
                info.errorMsg = std::wstring(value.begin(), value.end());
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
        
        // Check for VST2 entry point
        typedef AEffect* (*VSTPluginMain)(audioMasterCallback);
        VSTPluginMain vstMain = nullptr;
        
        vstMain = (VSTPluginMain)GetProcAddress(hModule, "VSTPluginMain");
        if (!vstMain) {
            vstMain = (VSTPluginMain)GetProcAddress(hModule, "main");
        }
        
        if (vstMain) {
            // VST2 plugin
            AEffect* effect = vstMain([](AEffect*, int32_t, int32_t, intptr_t, void*, float) -> intptr_t {
                return 0;
            });
            
            if (effect && effect->magic == kEffectMagic) {
                // Extract plugin information
                std::cout << "path=" << std::string(pluginPath, pluginPath + wcslen(pluginPath)) << std::endl;
                std::cout << "type=VST2" << std::endl;
                std::cout << "is64Bit=" << (sizeof(void*) == 8 ? "true" : "false") << std::endl;
                std::cout << "numInputs=" << effect->numInputs << std::endl;
                std::cout << "numOutputs=" << effect->numOutputs << std::endl;
                std::cout << "uniqueId=" << effect->uniqueID << std::endl;
                std::cout << "hasEditor=" << (effect->flags & effFlagsHasEditor ? "true" : "false") << std::endl;
                std::cout << "isInstrument=" << (effect->flags & effFlagsIsSynth ? "true" : "false") << std::endl;
                
                // Get plugin name
                char name[256] = {0};
                effect->dispatcher(effect, effGetEffectName, 0, 0, name, 0);
                std::cout << "name=" << name << std::endl;
                
                // Get vendor
                char vendor[256] = {0};
                effect->dispatcher(effect, effGetVendorString, 0, 0, vendor, 0);
                std::cout << "vendor=" << vendor << std::endl;
                
                std::cout << "validated=true" << std::endl;
            } else {
                std::cout << "error=Invalid VST plugin" << std::endl;
                FreeLibrary(hModule);
                return 1;
            }
        } else {
            // Check for VST3
            // Note: VST3 scanning would require more complex COM initialization
            std::cout << "error=Not a VST2 plugin" << std::endl;
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