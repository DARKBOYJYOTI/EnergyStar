cmake_minimum_required(VERSION 3.24)
project(EnergyStar LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/MT /O1 /Os /Gy /Gw /W4 /EHsc /utf-8)
set(ICON_FILE "${CMAKE_SOURCE_DIR}/../Assets/EnergyStar.ico")
configure_file(Assets/EnergyStar.rc.in "${CMAKE_BINARY_DIR}/EnergyStar.rc" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/UI/UI.rc.in" "${CMAKE_BINARY_DIR}/UI.rc" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/Assets/UI.manifest" "${CMAKE_BINARY_DIR}/UI.manifest" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/UI/Resource.h" "${CMAKE_BINARY_DIR}/Resource.h" COPYONLY)

add_executable(EnergyStar.Service WIN32 Service/Service.cpp "${CMAKE_BINARY_DIR}/EnergyStar.rc")
target_link_libraries(EnergyStar.Service PRIVATE Advapi32 Wtsapi32 Userenv Shell32 Ole32)

add_executable(EnergyStar.Agent WIN32 Agent/Agent.cpp)
target_link_libraries(EnergyStar.Agent PRIVATE User32)

add_executable(EnergyStar WIN32 UI/UI.cpp "${CMAKE_BINARY_DIR}/UI.rc" "${CMAKE_BINARY_DIR}/EnergyStar.rc")
target_link_libraries(EnergyStar PRIVATE Comctl32 Shell32 Ole32)
target_include_directories(EnergyStar PRIVATE "${CMAKE_BINARY_DIR}")

foreach(target EnergyStar.Service EnergyStar.Agent EnergyStar)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  target_link_options(${target} PRIVATE /OPT:REF /OPT:ICF)
endforeach()
