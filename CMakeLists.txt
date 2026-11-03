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
# Pinned to a commit, and moving a pin is a commit of its own. SYSTEM, so that the warnings of this
# project do not apply to the headers of the libraries.
#
include(FetchContent)
FetchContent_Declare(anyhttp
    GIT_REPOSITORY https://github.com/pgit/anyhttp.git
    GIT_TAG 9c7627d78cc350376e6f2e6872b251d854a565d8
    SYSTEM)
FetchContent_Declare(c_ares_async
    GIT_REPOSITORY https://github.com/pgit/c-ares-async.git
    GIT_TAG 2fb55f687f210d3da816e24aab9cefb61d6b4673
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
