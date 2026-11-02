cmake_minimum_required(VERSION 3.30)
project(crawler VERSION 0.0.1 DESCRIPTION "Requests hosts the way their HTTPS records say, on anyhttp and c-ares-async" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED On)

# --------------------------------------------------------------------------------------------------

#
# What everything in one process has to agree on. Both libraries have a say in each of these and
# neither knows about the other, so it is decided here, ahead of them, and for both.
#

#
# Standard library: libc++ with clang, libstdc++ with GCC, as in anyhttp. anyhttp sets this up for
# its own directory only, so c-ares-async would be left with libstdc++ and the Boost in /usr/local
# if it was not done up here, where it reaches both.
#
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    add_link_options(-stdlib=libc++)
    list(PREPEND CMAKE_PREFIX_PATH /opt/libc++)
endif()

# I/O runtime: ASIO or COROSIO. COROSIO would have to declare the capy and corosio pins here, too.
set(ANYHTTP_API ASIO)
set(C_ARES_ASYNC_API ${ANYHTTP_API})

#
# TLS library: OpenSSL or AWS-LC. Their defaults differ, and COROSIO works on OpenSSL only. This
# variable shadows the cache entry anyhttp creates, which goes on saying AWS-LC in CMakeCache.txt.
#
set(TLS_LIBRARY OpenSSL)
set(C_ARES_ASYNC_TLS ${TLS_LIBRARY})

# --------------------------------------------------------------------------------------------------

#
# Pinned to a commit, and moving a pin is a commit of its own. To build against a checkout instead
# there is the 'dev' preset, which sets FETCHCONTENT_SOURCE_DIR_*. SYSTEM, so that the warnings of
# this project do not apply to the headers of the libraries.
#
# anyhttp: the head of branch 'packaging', move this to master once that is merged
#
include(FetchContent)
FetchContent_Declare(anyhttp
    GIT_REPOSITORY https://github.com/pgit/anyhttp.git
    GIT_TAG af39b5c71490978d545c1b47ee8e149f9a7082c9
    SYSTEM)
FetchContent_Declare(c_ares_async
    GIT_REPOSITORY https://github.com/pgit/c-ares-async.git
    GIT_TAG 2fb55f687f210d3da816e24aab9cefb61d6b4673
    SYSTEM)
FetchContent_MakeAvailable(anyhttp c_ares_async)

# --------------------------------------------------------------------------------------------------

#
# The program in a library of its own, for the tests to link. The warnings are not for the sources
# from c-ares-async that the tests borrow, see test/CMakeLists.txt.
#
set(crawler_warnings -Wall -Wextra -Werror -Wno-missing-designated-field-initializers)

add_library(crawler_core STATIC src/prober.cpp src/routes.cpp)
target_include_directories(crawler_core PUBLIC src)
target_link_libraries(crawler_core PUBLIC anyhttp::anyhttp c_ares_async::c_ares_async)
target_compile_options(crawler_core PRIVATE ${crawler_warnings})

add_executable(crawler src/main.cpp)
target_link_libraries(crawler PRIVATE crawler_core)
target_compile_options(crawler PRIVATE ${crawler_warnings})

enable_testing()
add_subdirectory(test)
