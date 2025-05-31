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

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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
        comctl32
)

# Scanner process executable
add_executable(VSTScanner
    src/PluginScanner.cpp
)

target_compile_definitions(VSTScanner PRIVATE BUILD_SCANNER_PROCESS)
target_include_directories(VSTScanner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(VSTScanner
    PRIVATE
        shlwapi
        shell32
        ole32
)

# Example application
add_executable(VSTHostExample
    examples/main.cpp
)

target_link_libraries(VSTHostExample
    PRIVATE
        EnhancedVSTHostLib
        comctl32
        shell32
        shlwapi
)

# Set output directories
set_target_properties(VSTScanner VSTHostExample
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Install rules
install(TARGETS EnhancedVSTHostLib VSTScanner VSTHostExample
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${HEADERS} DESTINATION include)

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

# Generate pkg-config file if on Unix-like systems
if(UNIX)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/enhanced-vst-host.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
        @ONLY
    )

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/enhanced-vst-host.pc"
        DESTINATION lib/pkgconfig
    )
endif()