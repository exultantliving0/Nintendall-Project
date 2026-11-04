# Cross-compile Nintendall for 64-bit Windows from Linux with MinGW-w64.
#   cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
# Easiest: run tools/build_windows.sh, which also fetches SDL2 and makes the installer.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(NINTENDALL_SDL2_MINGW "${CMAKE_CURRENT_LIST_DIR}/../third_party/win/SDL2-2.32.10/x86_64-w64-mingw32")
set(NINTENDALL_SDL2_TTF_MINGW "${CMAKE_CURRENT_LIST_DIR}/../third_party/win/SDL2_ttf-2.24.0/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 "${NINTENDALL_SDL2_MINGW}" "${NINTENDALL_SDL2_TTF_MINGW}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
