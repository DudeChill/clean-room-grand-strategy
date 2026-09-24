cmake_minimum_required(VERSION 3.20)
project(hoi4clone CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wshadow)
add_compile_options($<$<CONFIG:Debug>:-DHOI_DEBUG_BUILD=1>)

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

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE hoicore)

add_executable(genmap tools/genmap.cpp)
target_link_libraries(genmap PRIVATE hoicore)

file(GLOB HOI_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
add_executable(hoi_tests ${HOI_TEST_SOURCES})
target_link_libraries(hoi_tests PRIVATE hoicore)

enable_testing()
add_test(NAME hoi_tests COMMAND hoi_tests)
