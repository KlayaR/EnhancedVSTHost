cmake_minimum_required(VERSION 3.20)
project(EnhancedVSTHost VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /MP")
    
    # Enable High DPI support
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml")
endif()

# Find packages
find_package(Threads REQUIRED)


# VST3 SDK path (you need to set this)
set(VST3_SDK_PATH "C:/SDKs/vst3sdk" CACHE PATH "Path to VST3 SDK")

# ASIO SDK path (optional)
set(ASIO_SDK_PATH "C:/SDKs/asiosdk" CACHE PATH "Path to ASIO SDK")

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VST2_SDK_PATH}
    ${VST3_SDK_PATH}
)

if(EXISTS ${ASIO_SDK_PATH})
    include_directories(${ASIO_SDK_PATH}/common)
    add_definitions(-DASIO_SUPPORT)
endif()

# Source files
set(SOURCES
    src/EnhancedVSTHost.cpp
    src/PluginScanner.cpp
    src/AudioEngines.cpp
    src/PluginInstance.cpp
    src/HelperComponents.cpp
)

set(HEADERS
    include/EnhancedVSTHost.h
)

# Main library
add_library(EnhancedVSTHostLib STATIC ${SOURCES} ${HEADERS})

target_link_libraries(EnhancedVSTHostLib
    PRIVATE
        Threads::Threads
        shlwapi
        shell32
        ole32
        uuid
        avrt
)

# Scanner process executable
add_executable(VSTScanner
    src/PluginScanner.cpp
)

target_compile_definitions(VSTScanner PRIVATE BUILD_SCANNER_PROCESS)

target_link_libraries(VSTScanner
    PRIVATE
        shlwapi
)

# 32-bit bridge executable (requires separate 32-bit build)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(VSTBridge32
        src/Bridge32.cpp
    )
    
    target_link_libraries(VSTBridge32
        PRIVATE
            shlwapi
    )
endif()

# Example application
add_executable(VSTHostExample
    examples/main.cpp
)

target_link_libraries(VSTHostExample
    PRIVATE
        EnhancedVSTHostLib
)

# Set output directories
set_target_properties(VSTScanner VSTHostExample
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(TARGET VSTBridge32)
    set_target_properties(VSTBridge32
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# Install rules
install(TARGETS EnhancedVSTHostLib VSTScanner VSTHostExample
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${HEADERS} DESTINATION include)

# Copy VST2 SDK files if needed
if(EXISTS ${VST2_SDK_PATH})
    install(DIRECTORY ${VST2_SDK_PATH}/pluginterfaces
        DESTINATION include/vst2sdk
    )
endif()

# Create manifest file for High DPI support
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/manifest.xml" 
"<?xml version='1.0' encoding='UTF-8' standalone='yes'?>
<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>
  <application xmlns='urn:schemas-microsoft-com:asm.v3'>
    <windowsSettings>
      <dpiAware xmlns='http://schemas.microsoft.com/SMI/2005/WindowsSettings'>True/PM</dpiAware>
      <dpiAwareness xmlns='http://schemas.microsoft.com/SMI/2016/WindowsSettings'>PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>")

# Generate pkg-config file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/enhanced-vst-host.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
    @ONLY
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
    DESTINATION lib/pkgconfig
)