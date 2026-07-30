# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)
project(ihs_carla_bridge LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IHS_CARLA_BRIDGE_TESTS "Build the transport tests" ON)

# The ihs-vk-export wire transport: framed messages + SCM_RIGHTS fd passing over
# a Unix SEQPACKET socket. Header-only contracts (wire_protocol.h / vk_export.h)
# live in include/; the transport mechanics are compiled here. The socket
# session, IO thread, and the dart:ffi module build on top of this in later
# steps.
add_library(ihs_vke_transport STATIC
        src/scm_rights.cc
        src/session.cc
        src/server.cc)
target_include_directories(ihs_vke_transport
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ihs_vke_transport PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(ihs_vke_transport PUBLIC Threads::Threads)

if (IHS_CARLA_BRIDGE_TESTS)
    enable_testing()
    add_executable(scm_rights_test test/scm_rights_test.cc)
    target_link_libraries(scm_rights_test PRIVATE ihs_vke_transport)
    target_include_directories(scm_rights_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME scm_rights COMMAND scm_rights_test)

    add_executable(session_test test/session_test.cc)
    target_link_libraries(session_test PRIVATE ihs_vke_transport)
    target_include_directories(session_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME session COMMAND session_test)

    add_executable(server_test test/server_test.cc)
    target_link_libraries(server_test PRIVATE ihs_vke_transport)
    target_include_directories(server_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME server COMMAND server_test)
endif ()
