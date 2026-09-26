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

# ---------------------------------------------------------------- SDL2_ttf
# 界面层用它渲染矢量字体（系统微软雅黑），Windows 版的 dll 已经把
# freetype/harfbuzz 静态链进去了，所以运行时只需要 SDL2_ttf.dll 这一个额外文件。
set(FC_SDL2_TTF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2_ttf-2.24.0/x86_64-w64-mingw32"
    CACHE PATH "SDL2_ttf 开发包根目录（包含 include/SDL2、lib、bin）")

find_path(FC_SDL2_TTF_INCLUDE SDL_ttf.h
    HINTS "${FC_SDL2_TTF_ROOT}/include/SDL2" "${FC_SDL2_TTF_ROOT}/include"
    NO_DEFAULT_PATH)
find_library(FC_SDL2_TTF_LIB SDL2_ttf
    HINTS "${FC_SDL2_TTF_ROOT}/lib"
    NO_DEFAULT_PATH)

# ---------------------------------------------------------------- zlib
# 只用来解 zip 里的 .nes（src/frontend/zipfile.cpp）。
# MinGW-w64 自带 zlib，Linux/macOS 上一般也预装。
find_package(ZLIB QUIET)

if(NOT ZLIB_FOUND)
    # CMake 自带的 FindZLIB 只搜标准安装前缀，不会去翻编译器自带的 sysroot，
    # 而 MinGW-w64 恰恰把 zlib 放在那里（<mingw>/x86_64-w64-mingw32/{include,lib}）。
    # 所以再按编译器自己的搜索路径兜一次 —— 找不到也不报错，只是少了 zip 支持。
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libz.a
                    OUTPUT_VARIABLE _fc_libz
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_fc_libz AND EXISTS "${_fc_libz}")
        get_filename_component(_fc_libz_dir "${_fc_libz}" DIRECTORY)
        find_path(_fc_zlib_include zlib.h
            HINTS "${_fc_libz_dir}/../include" "${_fc_libz_dir}/../../include")
        if(_fc_zlib_include)
            set(ZLIB_FOUND      TRUE)
            set(ZLIB_LIBRARIES  "${_fc_libz}")
            set(ZLIB_INCLUDE_DIRS "${_fc_zlib_include}")
            message(STATUS "使用编译器自带的 zlib: ${_fc_libz}")
        endif()
    endif()
endif()

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
if(FC_SDL2_INCLUDE AND FC_SDL2_LIB AND FC_SDL2_TTF_INCLUDE AND FC_SDL2_TTF_LIB)
    add_executable(fc-emulator
        src/frontend/main.cpp
        src/frontend/ui.cpp
        src/frontend/launcher.cpp
        src/frontend/pausemenu.cpp
        src/frontend/keycfg.cpp
        src/frontend/keyscreen.cpp
        src/frontend/zipfile.cpp)   # 总是编译：没有 zlib 时仍能读「不压缩」的 zip 条目
    target_include_directories(fc-emulator PRIVATE ${FC_SDL2_INCLUDE} ${FC_SDL2_TTF_INCLUDE})
    # 顺序要紧：SDL2_ttf 依赖 SDL2，必须排在前面
    target_link_libraries(fc-emulator PRIVATE fc-core ${FC_SDL2_TTF_LIB} ${FC_SDL2_LIB})
    if(ZLIB_FOUND)
        # ZLIB_LIBRARIES 是 find_package(ZLIB) 给出来的实际库路径（MinGW 下就是 libz.a）
        target_link_libraries(fc-emulator PRIVATE ${ZLIB_LIBRARIES})
        target_compile_definitions(fc-emulator PRIVATE FC_HAVE_ZLIB=1)
        target_include_directories(fc-emulator PRIVATE ${ZLIB_INCLUDE_DIRS})
    else()
        message(WARNING "未找到 zlib，将不支持直接加载 .zip（裸 .nes 不受影响）。")
    endif()

    if(WIN32)
        add_custom_command(TARGET fc-emulator POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FC_SDL2_ROOT}/bin/SDL2.dll" "$<TARGET_FILE_DIR:fc-emulator>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FC_SDL2_TTF_ROOT}/bin/SDL2_ttf.dll" "$<TARGET_FILE_DIR:fc-emulator>"
            COMMENT "复制 SDL2.dll / SDL2_ttf.dll 到输出目录")
    endif()
else()
    message(WARNING
        "未找到 SDL2 或 SDL2_ttf（SDL2: ${FC_SDL2_ROOT} / SDL2_ttf: ${FC_SDL2_TTF_ROOT}）。"
        "将只构建 fc-headless。"
        "请把两个开发包解压到 third_party/，或用 -DFC_SDL2_ROOT= / -DFC_SDL2_TTF_ROOT= 指定。")
endif()

# ---------------------------------------------------------------- 编译选项
if(MSVC)
    target_compile_options(fc-core PRIVATE /W3)
else()
    target_compile_options(fc-core PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    # 前端也要开：不然「构建零告警」只是一句没被验证过的声明
    target_compile_options(fc-emulator PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(fc-headless PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(fc-relay    PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(MINGW)
        target_link_options(fc-core PUBLIC -static-libgcc -static-libstdc++)
    endif()
endif()
