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
# C is needed for the vendored Dart DL API (dart_api_dl.c), used by the
# BridgeState event stream to post to a Dart native port.
project(ihs_carla_bridge LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IHS_CARLA_BRIDGE_TESTS "Build the transport tests" ON)

# Vendored Dart native (dynamic-linking) API. dart_api_dl.c is compiled into the
# module so it can Dart_PostCObject to a Dart native port; the headers are BSD
# (Dart authors). Only bridge.cc references them, but both library targets
# include the source so the static test lib links identically to the module.
set(IHS_CB_DART_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dart-sdk/include)
set(IHS_CB_DART_SRC ${IHS_CB_DART_DIR}/dart_api_dl.c)

# The ihs-vk-export wire transport: framed messages + SCM_RIGHTS fd passing over
# a Unix SEQPACKET socket. Header-only contracts (wire_protocol.h / vk_export.h)
# live in include/; the transport mechanics are compiled here. The socket
# session, IO thread, and the dart:ffi module build on top of this in later
# steps.
add_library(ihs_vke_transport STATIC
        src/scm_rights.cc
        src/session.cc
        src/server.cc
        src/shell_export_source.cc
        src/bridge.cc
        ${IHS_CB_DART_SRC})
target_include_directories(ihs_vke_transport
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${IHS_CB_DART_DIR})
# -Wpedantic only on our own C++; the vendored C file uses its own conventions.
target_compile_options(ihs_vke_transport PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
find_package(Threads REQUIRED)
# ${CMAKE_DL_LIBS} provides dlsym for ShellExportSource's symbol resolution.
target_link_libraries(ihs_vke_transport PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# The deployable dart:ffi module: one SHARED object built from all the transport
# sources, exporting ONLY the ihs_carla_bridge_* C ABI functions (each tagged
# IHS_CB_EXPORT / __attribute__((visibility("default")))); everything else --
# the ihs_vke::* internals and the vendored Dart DL API -- is hidden. The static
# lib above stays as the tests' link target; this is what ships.
add_library(ihs_carla_bridge SHARED
        src/scm_rights.cc
        src/session.cc
        src/server.cc
        src/shell_export_source.cc
        src/bridge.cc
        ${IHS_CB_DART_SRC})
target_include_directories(ihs_carla_bridge
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${IHS_CB_DART_DIR})
target_compile_options(ihs_carla_bridge PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
set_target_properties(ihs_carla_bridge PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(ihs_carla_bridge PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
# Pin the dynamic export table to exactly the four C ABI symbols; localize the
# rest (incl. std::thread's vague-linkage instantiations).
target_link_options(ihs_carla_bridge PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/module_exports.map")

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

    add_executable(bridge_test test/bridge_test.cc)
    target_link_libraries(bridge_test PRIVATE ihs_vke_transport)
    target_include_directories(bridge_test
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME bridge COMMAND bridge_test)
endif ()
