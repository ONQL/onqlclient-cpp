cmake_minimum_required(VERSION 3.14)
project(onql_cpp_driver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(onql_cpp STATIC src/client.cpp)
target_include_directories(onql_cpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(WIN32)
    target_link_libraries(onql_cpp PRIVATE ws2_32)
endif()

find_package(Threads REQUIRED)
target_link_libraries(onql_cpp PRIVATE Threads::Threads)

# Install targets
install(TARGETS onql_cpp ARCHIVE DESTINATION lib)
install(DIRECTORY include/onql DESTINATION include)

# Optional: build example
option(ONQL_BUILD_EXAMPLE "Build example program" OFF)
if(ONQL_BUILD_EXAMPLE)
    add_executable(onql_cpp_example example/main.cpp)
    target_link_libraries(onql_cpp_example PRIVATE onql_cpp)
endif()
