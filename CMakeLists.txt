cmake_minimum_required(VERSION 3.15)
project(PoPSnF_Reignited C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Build Mode Configuration: Production (default) vs Debug
option(ENABLE_DEBUG "Enable debug logging, telemetry, profiling, and diagnostics" OFF)

if(ENABLE_DEBUG OR DEBUG OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(SNF_DEBUG=1)
    message(STATUS "SnF Build Mode: DEBUG (SNF_DEBUG=1) - Logging, telemetry, and profiling enabled")
else()
    add_compile_definitions(SNF_DEBUG=0)
    message(STATUS "SnF Build Mode: PRODUCTION (SNF_DEBUG=0) - Lean, silent, high-performance")
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/elf32
    ${CMAKE_SOURCE_DIR}/src/runtime
    ${CMAKE_SOURCE_DIR}/src/bridge
    ${CMAKE_SOURCE_DIR}/src/mods
    ${CMAKE_SOURCE_DIR}/third_party/dynarmic/include
    ${CMAKE_SOURCE_DIR}/third_party/SDL2/x86_64-w64-mingw32/include
    ${CMAKE_SOURCE_DIR}/third_party/SDL2/x86_64-w64-mingw32/include/SDL2
    ${CMAKE_SOURCE_DIR}/third_party/openal/include
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/miniz
    ${CMAKE_SOURCE_DIR}/third_party/lzma
)

set(SOURCES
    src/main.cpp
    src/elf32/elf32_loader.c
    src/runtime/dynarmic_host.cpp
    src/bridge/bridge_libc.c
    src/bridge/bridge_gles.c
    src/bridge/bridge_openal.c
    src/bridge/bridge_jni.c
    src/config.cpp
    src/gpu_detect.cpp
    src/mods/soft_mods.cpp
)

if(WIN32)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app.res.obj"
        COMMAND "${CMAKE_RC_COMPILER}" -i "app.rc" -o "${CMAKE_CURRENT_BINARY_DIR}/app.res.obj"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/res/app.rc" "${CMAKE_SOURCE_DIR}/src/res/app_icon.ico"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/res"
    )
    list(APPEND SOURCES "${CMAKE_CURRENT_BINARY_DIR}/app.res.obj")
endif()

add_executable(PoPSnF_Reignited ${SOURCES})

# Platform-specific library configuration
if(WIN32)
    set(DYNARMIC_LIB "${CMAKE_SOURCE_DIR}/third_party/dynarmic/lib/windows-x64/libdynarmic.a")
    set(SDL2_LIB "${CMAKE_SOURCE_DIR}/third_party/SDL2/x86_64-w64-mingw32/lib/libSDL2.dll.a")
    set(OPENAL_LIB "${CMAKE_SOURCE_DIR}/third_party/openal/libs/Win64/libOpenAL32.dll.a")
    set(PLATFORM_LIBS opengl32 winmm)
elseif(UNIX AND NOT APPLE)
    set(DYNARMIC_LIB "${CMAKE_SOURCE_DIR}/third_party/dynarmic/lib/linux-x64/libdynarmic.a")
    # Placeholder for Linux when you compile it later
    find_package(SDL2 REQUIRED)
    find_package(OpenAL REQUIRED)
    set(SDL2_LIB SDL2::SDL2)
    set(OPENAL_LIB OpenAL::OpenAL)
    set(PLATFORM_LIBS GL)
endif()

target_link_libraries(PoPSnF_Reignited PRIVATE
    ${DYNARMIC_LIB}
    ${SDL2_LIB}
    ${OPENAL_LIB}
    ${PLATFORM_LIBS}
)

set(LAUNCHER_SOURCES
    src/launcher/launcher_main.cpp
    src/launcher/extractor.cpp
    src/launcher/embedded_assets.cpp
    src/config.cpp
    src/gpu_detect.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl2.cpp
    third_party/imgui/backends/imgui_impl_opengl2.cpp
    third_party/miniz/miniz.c
    third_party/miniz/miniz_tdef.c
    third_party/miniz/miniz_tinfl.c
    third_party/miniz/miniz_zip.c
    third_party/lzma/LzmaDec.c
)

if(WIN32)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/launcher.res.obj"
        COMMAND "${CMAKE_RC_COMPILER}" -i "launcher.rc" -o "${CMAKE_CURRENT_BINARY_DIR}/launcher.res.obj"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/res/launcher.rc" "${CMAKE_SOURCE_DIR}/src/res/app_icon.ico"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/res"
    )
    list(APPEND LAUNCHER_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/launcher.res.obj")
endif()

add_executable(PoPSnF_Launcher ${LAUNCHER_SOURCES})

target_link_libraries(PoPSnF_Launcher PRIVATE
    ${SDL2_LIB}
    ${PLATFORM_LIBS}
    comdlg32
)

# In production builds, suppress the extra console/terminal window by targeting the Windows GUI subsystem
# and strip all debug symbol tables for lean, high-performance release binaries
if(WIN32)
    if(NOT (ENABLE_DEBUG OR DEBUG OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
        set_target_properties(PoPSnF_Reignited PROPERTIES WIN32_EXECUTABLE TRUE LINK_FLAGS "-s")
        set_target_properties(PoPSnF_Launcher PROPERTIES WIN32_EXECUTABLE TRUE LINK_FLAGS "-s")
    endif()
endif()

if(WIN32)
    # Copy required DLLs and deploy executables to game/ directory
    add_custom_command(TARGET PoPSnF_Reignited POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/SDL2/x86_64-w64-mingw32/bin/SDL2.dll"
            "$<TARGET_FILE_DIR:PoPSnF_Reignited>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/openal/bin/Win64/soft_oal.dll"
            "$<TARGET_FILE_DIR:PoPSnF_Reignited>/OpenAL32.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_SOURCE_DIR}/game"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:PoPSnF_Reignited>"
            "${CMAKE_SOURCE_DIR}/game/PoPSnF_Reignited.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/SDL2/x86_64-w64-mingw32/bin/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/game/SDL2.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/openal/bin/Win64/soft_oal.dll"
            "${CMAKE_SOURCE_DIR}/game/OpenAL32.dll"
    )

    add_custom_command(TARGET PoPSnF_Launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:PoPSnF_Launcher>"
            "${CMAKE_SOURCE_DIR}/game/PoPSnF_Launcher.exe"
    )
endif()
