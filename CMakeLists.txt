cmake_minimum_required(VERSION 3.15)
project(fc-emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- SDL2
set(FC_SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2-2.30.9/x86_64-w64-mingw32"
    CACHE PATH "SDL2 开发包根目录（包含 include/SDL2、lib、bin）")

find_path(FC_SDL2_INCLUDE SDL.h
    HINTS "${FC_SDL2_ROOT}/include/SDL2" "${FC_SDL2_ROOT}/include"
    NO_DEFAULT_PATH)
find_library(FC_SDL2_LIB SDL2
    HINTS "${FC_SDL2_ROOT}/lib"
    NO_DEFAULT_PATH)

# ---------------------------------------------------------------- 模拟器核心（不含前端）
set(FC_CORE_SOURCES
    src/core/cpu6502.cpp
    src/core/ppu.cpp
    src/core/apu.cpp
    src/core/cartridge.cpp
    src/core/bus.cpp
    src/core/emulator.cpp
    src/net/net.cpp
)

add_library(fc-core STATIC ${FC_CORE_SOURCES})
target_include_directories(fc-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
    target_link_libraries(fc-core PUBLIC ws2_32 winmm)
endif()

# ---------------------------------------------------------------- 无图形验证程序
add_executable(fc-headless src/frontend/headless.cpp)
target_link_libraries(fc-headless PRIVATE fc-core)

# ---------------------------------------------------------------- 中继服务器
# 独立小程序：不依赖模拟器核心，方便单独丢到公网服务器上编译运行。
#   g++ -O2 -std=c++17 -o fc-relay src/frontend/relay.cpp -Isrc
add_executable(fc-relay src/frontend/relay.cpp)
target_include_directories(fc-relay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(fc-relay PRIVATE ws2_32)
endif()
if(MINGW)
    target_link_options(fc-relay PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------- SDL2 图形前端
if(FC_SDL2_INCLUDE AND FC_SDL2_LIB)
    add_executable(fc-emulator
        src/frontend/main.cpp
        src/frontend/keycfg.cpp
        src/frontend/keyscreen.cpp)
    target_include_directories(fc-emulator PRIVATE ${FC_SDL2_INCLUDE})
    target_link_libraries(fc-emulator PRIVATE fc-core ${FC_SDL2_LIB})

    if(WIN32)
        add_custom_command(TARGET fc-emulator POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FC_SDL2_ROOT}/bin/SDL2.dll" "$<TARGET_FILE_DIR:fc-emulator>"
            COMMENT "复制 SDL2.dll 到输出目录")
    endif()
else()
    message(WARNING
        "未找到 SDL2（查找路径: ${FC_SDL2_ROOT}）。"
        "将只构建 fc-headless。"
        "请解压 SDL2 开发包到 third_party/ 或用 -DFC_SDL2_ROOT=<路径> 指定。")
endif()

# ---------------------------------------------------------------- 编译选项
if(MSVC)
    target_compile_options(fc-core PRIVATE /W3)
else()
    target_compile_options(fc-core PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    if(MINGW)
        target_link_options(fc-core PUBLIC -static-libgcc -static-libstdc++)
    endif()
endif()
