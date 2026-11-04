cmake_minimum_required(VERSION 3.16)
project(Nintendall VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
if(MINGW)
  # Link zlib statically so there's no zlib1.dll to ship.
  find_library(ZLIB_LIBRARY NAMES libz.a REQUIRED)
endif()
find_package(ZLIB REQUIRED)

if(WIN32)
  set(NINTENDALL_WIN_RC assets/nintendall.rc)
endif()

add_executable(nintendall
  ${NINTENDALL_WIN_RC}
  src/main.cpp
  src/app.cpp
  src/audio.cpp
  src/config.cpp
  src/core.cpp
  src/detect.cpp
  src/font.cpp
  src/input.cpp
  src/paths.cpp
  src/png.cpp
  src/systems.cpp
  src/video.cpp
  src/zip.cpp
)

target_include_directories(nintendall PRIVATE src third_party/libretro)
target_compile_definitions(nintendall PRIVATE NINTENDALL_VERSION="${PROJECT_VERSION}")

if(TARGET SDL2::SDL2main)
  target_link_libraries(nintendall PRIVATE SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2)
  target_link_libraries(nintendall PRIVATE SDL2::SDL2)
else()
  target_include_directories(nintendall PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(nintendall PRIVATE ${SDL2_LIBRARIES})
endif()
target_link_libraries(nintendall PRIVATE ZLIB::ZLIB ${CMAKE_DL_LIBS})

if(WIN32)
  # A normal windowed app (no console), with the compiler runtime built in so
  # the only DLL that ships next to it is SDL2.dll.
  set_target_properties(nintendall PROPERTIES WIN32_EXECUTABLE ON)
  if(MINGW)
    target_link_options(nintendall PRIVATE -static-libgcc -static-libstdc++ -static)
  endif()
  add_custom_command(TARGET nintendall POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:nintendall> $<TARGET_FILE_DIR:nintendall>
    COMMAND_EXPAND_LISTS)
  install(FILES $<TARGET_RUNTIME_DLLS:nintendall> DESTINATION .)
  set(NINTENDALL_CORE_PLATFORM windows)
else()
  set(NINTENDALL_CORE_PLATFORM auto)
endif()

# All-in-one package: the emulator cores are bundled in cores/ next to the program.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NINTENDALL_CORES_DIR ${CMAKE_BINARY_DIR}/cores)
add_custom_target(bundle_cores ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bundle_cores.py
          --dest ${NINTENDALL_CORES_DIR} --cache ${CMAKE_SOURCE_DIR}/cores/${NINTENDALL_CORE_PLATFORM}
          --platform ${NINTENDALL_CORE_PLATFORM}
  COMMENT "Bundling emulator cores"
  VERBATIM)
add_dependencies(nintendall bundle_cores)

# "cmake --install build --prefix dist/Nintendall" makes the shippable folder.
install(TARGETS nintendall RUNTIME DESTINATION .)
install(DIRECTORY ${NINTENDALL_CORES_DIR}/ DESTINATION cores)
install(FILES README.md THIRD_PARTY_LICENSES.md DESTINATION .)

if(MSVC)
  target_compile_options(nintendall PRIVATE /W3)
else()
  target_compile_options(nintendall PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
