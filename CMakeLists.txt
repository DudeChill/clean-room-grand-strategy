cmake_minimum_required(VERSION 3.20)
project(hoi4clone CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Warning flags are toolchain specific: MSVC's /W3 is the closest equivalent of
# -Wall -Wextra, and /utf-8 is required because the sources are UTF-8 (MSVC would
# otherwise read them in the machine's ANSI code page). /EHsc is the standard C++
# exception model, which the test runner's `throw` needs. /bigobj raises the object
# file section limit, which the largest translation units (tests/test_persistence.cpp)
# can exceed in Debug builds.
if(MSVC)
  add_compile_options(/W3 /utf-8 /EHsc /bigobj)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wshadow)
endif()

add_compile_definitions($<$<CONFIG:Debug>:HOI_DEBUG_BUILD=1>)

file(GLOB_RECURSE HOI_LIB_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/data/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/sim/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/save/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.cpp"
)

add_library(hoicore STATIC ${HOI_LIB_SOURCES})
target_include_directories(hoicore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The socket code lives in this library (src/net/transport.cpp, src/game/server.cpp) and
# uses Winsock through src/core/socket_compat.h. PUBLIC so every executable that links
# hoicore and opens a socket resolves WSAStartup/socket/send/recv.
if(WIN32)
  target_link_libraries(hoicore PUBLIC ws2_32)
endif()

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE hoicore)

add_executable(genmap tools/genmap.cpp)
target_link_libraries(genmap PRIVATE hoicore)

file(GLOB HOI_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
add_executable(hoi_tests ${HOI_TEST_SOURCES})
target_link_libraries(hoi_tests PRIVATE hoicore)

enable_testing()
add_test(NAME hoi_tests COMMAND hoi_tests)
