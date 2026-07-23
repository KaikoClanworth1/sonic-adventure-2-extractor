cmake_minimum_required(VERSION 3.15)

# CMP0091 must be NEW *before* project() so that the MSVC runtime library is
# controlled by the MSVC_RUNTIME_LIBRARY property instead of being baked into
# CMAKE_CXX_FLAGS_<CONFIG>. This is what makes the static-CRT setting below work.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(SA2Extractor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Static CRT -- produces self-contained .exe files (no vcredist required).
# ---------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Output layout: all executables land in ${CMAKE_BINARY_DIR}/bin.
# The per-config variables stop multi-config generators (Visual Studio) from
# appending bin/Release, bin/Debug, ... subdirectories.
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

set(EXTERN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern")
set(IMGUI_DIR  "${EXTERN_DIR}/imgui")
set(GLFW_DIR   "${EXTERN_DIR}/glfw")
set(STB_DIR    "${EXTERN_DIR}/stb")

# ---------------------------------------------------------------------------
# GLFW (prebuilt, vendored).
#
# The prebuilt lib-vc2019 folder ships two static libs:
#   glfw3.lib    -- built against the DYNAMIC CRT (/MD)
#   glfw3_mt.lib -- built against the STATIC  CRT (/MT)
# Because this project links the static CRT, glfw3_mt.lib is the matching one;
# using glfw3.lib here yields a hard LNK2038 "RuntimeLibrary mismatch" error.
# Prefer the _mt variant, fall back to glfw3.lib.
# ---------------------------------------------------------------------------
if(EXISTS "${GLFW_DIR}/lib/glfw3_mt.lib")
    set(GLFW_LIBRARY "${GLFW_DIR}/lib/glfw3_mt.lib")
else()
    set(GLFW_LIBRARY "${GLFW_DIR}/lib/glfw3.lib")
endif()
message(STATUS "GLFW static library: ${GLFW_LIBRARY}")

# ===========================================================================
# sa2core -- static parsing library
# ===========================================================================
file(GLOB SA2CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")

add_library(sa2core STATIC ${SA2CORE_SOURCES})
target_include_directories(sa2core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${STB_DIR}"
)
set_target_properties(sa2core PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ===========================================================================
# sa2cli -- console harness
# ===========================================================================
file(GLOB SA2CLI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/*.cpp")

add_executable(sa2cli ${SA2CLI_SOURCES})
target_link_libraries(sa2cli PRIVATE sa2core)
set_target_properties(sa2cli PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ===========================================================================
# sa2viewer -- WIN32 GUI app (Dear ImGui + GLFW + OpenGL3)
# ===========================================================================
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
)

file(GLOB SA2APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp")

add_executable(sa2viewer WIN32 ${SA2APP_SOURCES} ${IMGUI_SOURCES})
target_include_directories(sa2viewer PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${GLFW_DIR}/include"
    "${STB_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
)
target_link_libraries(sa2viewer PRIVATE
    sa2core
    "${GLFW_LIBRARY}"
    opengl32
    winmm            # waveOut audio playback
)
set_target_properties(sa2viewer PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
    target_compile_definitions(sa2viewer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(sa2cli   PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(sa2core  PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
