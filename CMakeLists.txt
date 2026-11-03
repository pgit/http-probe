cmake_minimum_required(VERSION 3.30)
project(http_probe VERSION 0.0.1 DESCRIPTION "Requests hosts the way their HTTPS records say, on anyhttp and c-ares-async" LANGUAGES C CXX)

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
    # As in anyhttp: the shared ones share their names with the libstdc++ ones in /usr/local/lib.
    set(Boost_USE_STATIC_LIBS ON)
endif()

#
# I/O runtime: Capy and Corosio, and only those. The libraries take either, which is their business:
# this is a program, and it gets to decide. These shadow the cache entries of the libraries, which
# go on saying ASIO in CMakeCache.txt.
#
set(ANYHTTP_API COROSIO)
set(C_ARES_ASYNC_API ${ANYHTTP_API})

#
# TLS library: OpenSSL or AWS-LC. Their defaults differ, and COROSIO works on OpenSSL only. This
# variable shadows the cache entry anyhttp creates, which goes on saying AWS-LC in CMakeCache.txt.
#
set(TLS_LIBRARY OpenSSL)
set(C_ARES_ASYNC_TLS ${TLS_LIBRARY})

# --------------------------------------------------------------------------------------------------

#
# Pinned to a commit, and moving a pin is a commit of its own. SYSTEM, so that the warnings of this
# project do not apply to the headers of the libraries.
#
include(FetchContent)
FetchContent_Declare(anyhttp
    GIT_REPOSITORY https://github.com/pgit/anyhttp.git
    GIT_TAG cc3faee4fb351a56bbca48db7cfcac7585e59e8b
    SYSTEM)
FetchContent_Declare(c_ares_async
    GIT_REPOSITORY https://github.com/pgit/c-ares-async.git
    GIT_TAG 6d46c6c8e65231f23a250bcec5dd00b3a1d9ba34
    SYSTEM)

#
# Both libraries pin these as well, to the same commits for now. The first declaration wins, so these
# are the ones both of them get, and here they are SYSTEM, too.
#
FetchContent_Declare(capy
    GIT_REPOSITORY https://github.com/cppalliance/capy.git
    GIT_TAG 24c54af2cf967ccbf8d3809af4bcd9c8f61f553b
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_Declare(corosio
    GIT_REPOSITORY https://github.com/cppalliance/corosio.git
    GIT_TAG 41d0de18b6e4fa1b397c38a866f2daf52c6c36b5
    EXCLUDE_FROM_ALL
    SYSTEM)

FetchContent_MakeAvailable(anyhttp c_ares_async)

# Only for the command line. Neither library needs this component, so neither asks for it here.
find_package(Boost 1.88 REQUIRED COMPONENTS program_options)

# --------------------------------------------------------------------------------------------------

#
# The program in a library of its own, for the tests to link. The warnings are not for the sources
# from c-ares-async that the tests borrow, see test/CMakeLists.txt.
#
set(http_probe_warnings -Wall -Wextra -Werror -Wno-missing-designated-field-initializers)

add_library(http_probe_core STATIC src/output.cpp src/prober.cpp src/routes.cpp)
target_include_directories(http_probe_core PUBLIC src)
target_link_libraries(http_probe_core PUBLIC anyhttp::anyhttp c_ares_async::c_ares_async)
target_compile_options(http_probe_core PRIVATE ${http_probe_warnings})

add_executable(http-probe src/main.cpp)
target_link_libraries(http-probe PRIVATE http_probe_core Boost::program_options)
target_compile_options(http-probe PRIVATE ${http_probe_warnings})

enable_testing()
add_subdirectory(test)
