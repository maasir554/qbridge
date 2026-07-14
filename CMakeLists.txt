cmake_minimum_required(VERSION 3.20)
project(QBridge VERSION 0.1.0 LANGUAGES C CXX)

option(QBRIDGE_BUILD_CLI "Build the qbridge command line utility" ON)
option(QBRIDGE_BUILD_TESTS "Build QBridge tests" ON)
option(QBRIDGE_BUILD_SHARED "Build qbridge as a shared library" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(QBRIDGE_BUILD_SHARED)
  set(QBRIDGE_LIBRARY_TYPE SHARED)
else()
  set(QBRIDGE_LIBRARY_TYPE STATIC)
endif()

add_library(qbridge ${QBRIDGE_LIBRARY_TYPE}
  src/client.cpp
  src/protocol.cpp
  src/socket.cpp
)
add_library(QBridge::qbridge ALIAS qbridge)
target_include_directories(qbridge
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE src
)
target_compile_definitions(qbridge PRIVATE QBRIDGE_BUILDING_LIBRARY)
if(QBRIDGE_BUILD_SHARED)
  target_compile_definitions(qbridge PUBLIC QBRIDGE_SHARED)
endif()
if(MSVC)
  target_compile_options(qbridge PRIVATE /W4 /permissive- /EHsc)
  target_compile_definitions(qbridge PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  target_compile_options(qbridge PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
if(WIN32)
  target_link_libraries(qbridge PRIVATE ws2_32)
endif()

if(QBRIDGE_BUILD_CLI)
  add_executable(qbridge-cli cli/main.cpp)
  target_link_libraries(qbridge-cli PRIVATE QBridge::qbridge)
  set_target_properties(qbridge-cli PROPERTIES OUTPUT_NAME qbridge)
endif()

include(CTest)
if(BUILD_TESTING AND QBRIDGE_BUILD_TESTS)
  add_executable(qbridge-tests
    tests/test_main.cpp
    tests/test_protocol.cpp
  )
  target_include_directories(qbridge-tests PRIVATE src tests)
  target_link_libraries(qbridge-tests PRIVATE QBridge::qbridge)
  add_test(NAME qbridge-tests COMMAND qbridge-tests)
endif()

include(GNUInstallDirs)
configure_file(cmake/qbridge.pc.in qbridge.pc @ONLY)
install(TARGETS qbridge
  EXPORT QBridgeTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qbridge.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

