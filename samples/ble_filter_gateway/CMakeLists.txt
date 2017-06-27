#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.12)

if (LINUX)
    pkg_search_module(GWGIOUNIX REQUIRED gio-unix-2.0)

    include_directories(${GWGIOUNIX_INCLUDE_DIRS})
    set(LIBS ${GWGIOUNIX_LIBRARIES})
endif()

set(ble_filter_sample_sources
    ./src/main.c
)

include_directories(./inc ${IOTHUB_CLIENT_INC_FOLDER})
include_directories(${GW_INC})
include_directories(../../modules/common)
set(LIBS ${LIBS} gateway)

if(LINUX)
    # Raspberry Pi 3 doesn't see 'signal' types unless this symbol is defined
    add_definitions(-D_POSIX_C_SOURCE=1)

    add_executable(ble_filter_sample ${ble_filter_sample_sources})

    add_dependencies(ble_filter_sample ble identity_map iothub filter logger ticc2541 ticc2650)

    target_link_libraries(ble_filter_sample nanomsg ${LIBS})
    linkSharedUtil(ble_filter_sample)
    install_broker(ble_filter_sample ${CMAKE_CURRENT_BINARY_DIR}/$(Configuration) )
    copy_gateway_dll(ble_filter_sample ${CMAKE_CURRENT_BINARY_DIR}/$(Configuration) )

    add_sample_to_solution(ble_filter_sample)

endif()

add_subdirectory(filters)
