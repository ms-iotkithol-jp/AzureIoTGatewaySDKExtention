#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.12)

################################################
# Include GIO headers/libs
################################################

set(filter_sources
    ./src/filter.c
)
set(filter_headers
    ./inc/filter.h
    ./inc/filter_api.h
)

include_directories(./inc)
include_directories(${GW_INC})

set(LIBS ${LIBS} gateway)

add_library(filter MODULE ${filter_sources} ${filter_headers})
target_link_libraries(filter ${LIBS})

#this builds the filter static library
add_library(filter_static STATIC ${filter_sources} ${filter_headers})
target_compile_definitions(filter_static PRIVATE BUILD_MODULE_TYPE_STATIC)
target_link_libraries(filter_static gateway)

linkSharedUtil(filter)
linkSharedUtil(filter_static)

add_module_to_solution(filter)

if(install_modules)
    install(TARGETS filter LIBRARY DESTINATION "${LIB_INSTALL_DIR}/modules") 
endif()
