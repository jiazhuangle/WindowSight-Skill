cmake_minimum_required(VERSION 3.16)

# ============================================================
# WindowSight - Windows window structured info extractor
# Build configuration
# ============================================================

project(WindowSight
    VERSION 1.0.0
    LANGUAGES CXX
)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Output directory ----
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- Windows SDK path (can be set manually) ----
# If the system does not find the Windows SDK automatically, uncomment and adjust:
# set(CMAKE_SYSTEM_VERSION 10.0.22621.0)

# ---- Find jsoncpp ----
# Option 1: use vcpkg (recommended)
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#
# Option 2: use a system-installed jsoncpp
#   - Windows: vcpkg install jsoncpp:x64-windows
#   - or install manually and set jsoncpp_DIR
#
# Option 3: use FetchContent to download automatically
option(USE_FETCHCONTENT "Use FetchContent to download jsoncpp automatically" OFF)

if(USE_FETCHCONTENT)
    include(FetchContent)
    FetchContent_Declare(
        jsoncpp
        GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
        GIT_TAG 1.9.5
    )
    # Disable jsoncpp tests and examples to speed up the build
    set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_PKGCONFIG_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jsoncpp)
else()
    find_package(jsoncpp CONFIG REQUIRED)
endif()

# ---- Sources ----
set(SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/main.cpp
)

# ---- Executable ----
add_executable(WindowSight ${SOURCES})

# ---- Link libraries ----
# jsoncpp
if(USE_FETCHCONTENT)
    target_link_libraries(WindowSight PRIVATE jsoncpp_static)
else()
    target_link_libraries(WindowSight PRIVATE jsoncpp_lib)
endif()

# Windows system libraries
target_link_libraries(WindowSight PRIVATE
    uiautomationcore    # UI Automation core
    ole32               # COM
    oleaut32            # BSTR / SysAllocString
    user32              # window API
    gdi32               # GDI bitmaps (screenshots)
    windowscodecs       # WIC PNG encoding (screenshots)
)

# ---- Compile definitions ----
# Ensure the Unicode character set
target_compile_definitions(WindowSight PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# ---- MSVC-specific options ----
if(MSVC)
    target_compile_options(WindowSight PRIVATE
        /EHsc      # C++ exception handling
        /W4        # high warning level
        /utf-8     # UTF-8 source encoding
    )
endif()

# ---- Install rules (optional) ----
install(TARGETS WindowSight
    RUNTIME DESTINATION bin
)
