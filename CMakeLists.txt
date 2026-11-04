cmake_minimum_required(VERSION 3.16)
project(Nintendall VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(nintendall
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

# All-in-one package: the emulator cores are bundled in cores/ next to the program.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NINTENDALL_CORES_DIR ${CMAKE_BINARY_DIR}/cores)
add_custom_target(bundle_cores ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/bundle_cores.py
          --dest ${NINTENDALL_CORES_DIR} --cache ${CMAKE_SOURCE_DIR}/cores
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
