#Copyright (c) Microsoft. All rights reserved.
#Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 2.8.12)

set(ticc2650_sources
    ./src/ticc2650.c
)

set(ticc2650_headers
    ./inc/ticc2650.h
)

include_directories(
    ../../../../modules/filter/inc
    ./inc
    ${GW_INC}
)

#this builds the ticc2650 dynamic library
add_library(ticc2650 MODULE ${ticc2650_sources} ${ticc2650_headers})
#target_link_libraries(ticc2650 gateway m)

#target_link_libraries(ticc2650 ${LIBS} m)
add_dependencies(ticc2650 filter)
target_link_libraries(ticc2650 ${LIBS} parson)
linkSharedUtil(ticc2650)
