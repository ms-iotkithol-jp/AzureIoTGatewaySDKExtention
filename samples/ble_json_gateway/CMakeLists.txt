#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.12)

################################################
# Include GIO headers/libs
################################################
find_package(PkgConfig REQUIRED)
pkg_search_module(GWGIOUNIX REQUIRED gio-unix-2.0)

include_directories(${GWGIOUNIX_INCLUDE_DIRS})
set(LIBS ${GWGIOUNIX_LIBRARIES})

set(ble_json_gateway_sources
  ./src/main.c
)
set(ble_json_gateway_headers
)

include_directories(
  ../../inc
  ${GW_INC}
)
set(LIBS ${LIBS} gateway)

# Raspberry Pi 3 doesn't see 'signal' types unless this symbol is defined
add_definitions(-D_POSIX_C_SOURCE=1)

add_executable(ble_json_gateway ${ble_json_gateway_headers} ${ble_json_gateway_sources})
target_link_libraries(ble_json_gateway ${LIBS})

add_dependencies(ble_json_gateway ble identity_map logger iothub ticc2650)

linkSharedUtil(ble_json_gateway)
install_broker(ble_json_gateway ${CMAKE_CURRENT_BINARY_DIR}/$(Configuration) )
copy_gateway_dll(ble_json_gateway ${CMAKE_CURRENT_BINARY_DIR}/$(Configuration) )


add_sample_to_solution(ble_json_gateway)
