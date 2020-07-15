cmake_minimum_required(VERSION 2.8)
file(GLOB_RECURSE SOURCES RELATIVE ${CMAKE_SOURCE_DIR} "src/*.c")
project (pam-pgsql)
add_library(pam_pgsql SHARED ${SOURCES})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(PostgreSQL REQUIRED)
# Add include path because cmake doesn't include Debian's path
include_directories(/usr/include/postgresql)
find_package(PAM REQUIRED)

file(STRINGS /etc/os-release distro REGEX "^NAME=")
string(REGEX REPLACE "NAME=\"(.*)\"" "\\1" distro "${distro}")
message("found ${distro}")

if(distro EQUAL Debian)
include_directories(/usr/include/b64)
find_library(B64_LIBRARY b64)
target_link_libraries(pam_pgsql pq gcrypt b64 pam crypt)
add_compile_options(-std=c99 -Wall)
else()
target_include_directories(pam_pgsql PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(pam_pgsql ${CMAKE_SOURCE_DIR}/b64/libb64.a)
target_link_libraries(pam_pgsql pq gcrypt pam crypt)
set ( CMAKE_C_FLAGS "-std=c99 -Wall")
endif()

include_directories(src/)

add_definitions(-DSYSCONFDIR="/etc")
add_definitions(-D_DEFAULT_SOURCE=1)


