# hfsnet ホスト GUI 版 (プロトタイプ)
# ビルド:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# 依存ライブラリは third_party/ に同梱する (imgui / glfw / glew /
# nativefiledialog-extended)。エンジンだけを試す場合は
#   -DHFSNET_BUILD_GUI=OFF で GUI なし (ヘッドレス専用) になる。
cmake_minimum_required(VERSION 3.20)
project(hfsnet-host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HFSNET_BUILD_GUI "GUI をビルドする" ON)

# ---- エンジン (全プラットフォーム共通) ----
set(ENGINE_SOURCES
    src/engine.cpp
    src/ui_strings.cpp
)
if(WIN32)
    list(APPEND ENGINE_SOURCES src/platform_win.cpp)
elseif(APPLE)
    list(APPEND ENGINE_SOURCES src/platform_mac.cpp)
else()
    list(APPEND ENGINE_SOURCES src/platform_linux.cpp)
endif()

if(HFSNET_BUILD_GUI)
    # ---- Dear ImGui ----
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )

    # ---- GLFW ----
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glfw)

    # ---- GLEW (リリース同梱の glew.c を静的ビルド) ----
    add_library(glew STATIC third_party/glew/src/glew.c)
    target_include_directories(glew PUBLIC third_party/glew/include)
    target_compile_definitions(glew PUBLIC GLEW_STATIC)
    find_package(OpenGL REQUIRED)
    if(WIN32)
        target_link_libraries(glew PUBLIC OpenGL::GL)
    elseif(APPLE)
        target_link_libraries(glew PUBLIC "-framework OpenGL")
    else()
        target_link_libraries(glew PUBLIC OpenGL::GL)
    endif()

    # ---- nativefiledialog-extended ----
    if(UNIX AND NOT APPLE)
        # Linux では GTK3 の代わりに xdg-desktop-portal 後端を使う
        # (GTK3 開発パッケージが無い環境でもビルドできるように)
        set(NFD_PORTAL ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(third_party/nativefiledialog-extended)

    # WIN32: Windows では GUI サブシステムにし、起動時にコンソール
    # ウィンドウが開かないようにする (他 OS では無視される)
    add_executable(hfsnet-host WIN32 src/main.cpp ${ENGINE_SOURCES}
                               ${IMGUI_SOURCES})
    if(WIN32)
        # アプリケーションアイコンを exe のリソースに埋め込む
        target_sources(hfsnet-host PRIVATE hfsnet.rc)
    endif()
    target_include_directories(hfsnet-host PRIVATE
        src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(hfsnet-host PRIVATE glfw glew nfd)
else()
    add_executable(hfsnet-host src/main.cpp ${ENGINE_SOURCES})
    target_include_directories(hfsnet-host PRIVATE src)
    target_compile_definitions(hfsnet-host PRIVATE HFSNET_NO_GUI)
endif()

if(WIN32)
    target_link_libraries(hfsnet-host PRIVATE ws2_32 iphlpapi)
    if(MINGW)
        # GCC ランタイム DLL (libgcc_s_seh / libstdc++) を同梱せずに
        # 済むよう静的リンクする (配布先に MinGW 環境を要求しない)
        target_link_options(hfsnet-host PRIVATE
            -static-libgcc -static-libstdc++)
    endif()
    if(MSVC)
        if(HFSNET_BUILD_GUI)
            # GUI サブシステムでも通常の main() から入れるようにする
            target_link_options(hfsnet-host PRIVATE /ENTRY:mainCRTStartup)
        endif()
        # ソース内の日本語文字列・コメントを正しく読むため
        target_compile_options(hfsnet-host PRIVATE /utf-8)
        target_compile_definitions(hfsnet-host PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(hfsnet-host PRIVATE Threads::Threads)
endif()
