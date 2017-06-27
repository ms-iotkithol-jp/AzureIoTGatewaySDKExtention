#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.12)

set(ticc2541_sources
    ./src/ticc2541.c
)

set(ticc2541_headers
    ./inc/ticc2541.h
)

include_directories(
    ../../../../modules/filter/inc
    ./inc
    ${GW_INC}
)

#this builds the ticc2541 dynamic library
add_library(ticc2541 MODULE ${ticc2541_sources} ${ticc2541_headers})
#target_link_libraries(ticc2541 gateway m)

#target_link_libraries(ticc2541 ${LIBS} m)
add_dependencies(ticc2541 filter)

target_link_libraries(ticc2541 ${LIBS} parson)
linkSharedUtil(ticc2541)
