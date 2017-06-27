#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.12)

set(filter_sample_sources
    ./src/main.c
)
if(WIN32)
    set(filter_sample_sources 
        ${filter_sample_sources}
        ./src/filtersample_win.json
    )
    set_source_files_properties(./src/filtersample_win.json PROPERTIES HEADER_FILE_ONLY ON)
else()
    set(filter_sample_sources 
        ${filter_sample_sources}
#        ./src/filtersample_lin.json
    )
    set_source_files_properties(./src/filtersample_lin.json PROPERTIES HEADER_FILE_ONLY ON)
endif()

include_directories(./inc ${IOTHUB_CLIENT_INC_FOLDER})
include_directories(${GW_INC})
include_directories(../../modules/common)

add_executable(filter_sample ${filter_sample_sources})

add_dependencies(filter_sample filter logger ticc2541 ticc2650)

target_link_libraries(filter_sample gateway nanomsg ${CPPREST_LIBRARIES})
linkSharedUtil(filter_sample)
install_broker(filter_sample ${CMAKE_CURRENT_BINARY_DIR}/$(Configuration) )
copy_gateway_dll(filter_sample ${CMAKE_CURRENT_BINARY_DIR}/$(Configuration) )

add_sample_to_solution(filter_sample)

#add_subdirectory(filters)
