#
# GoogleTest as in c-ares-async: from wherever it is installed, from its release archive otherwise.
#
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    FIND_PACKAGE_ARGS NAMES GTest)
set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

find_package(OpenSSL REQUIRED)

#
# The DNS server of c-ares-async's own tests, at the commit of the pin: not an interface of that
# library, but all it takes to have a zone of our own on loopback. Moving the pin may mean adapting
# to it. Compiled apart, as the warnings of this project are not for those sources.
#
add_library(
    fake_dns STATIC ${c_ares_async_SOURCE_DIR}/test/fake_dns_server.cpp
                    ${c_ares_async_SOURCE_DIR}/test/test_certificate.cpp)
target_include_directories(fake_dns SYSTEM PUBLIC ${c_ares_async_SOURCE_DIR}/test)
target_link_libraries(fake_dns PUBLIC c_ares_async::c_ares_async anyhttp::anyhttp OpenSSL::SSL
                                      OpenSSL::Crypto)

add_library(test_site STATIC site.cpp)
target_include_directories(test_site PUBLIC .)
target_link_libraries(test_site PUBLIC crawler_core fake_dns)
target_compile_options(test_site PRIVATE ${crawler_warnings})

add_executable(crawler_tests prober_test.cpp routes_test.cpp)
target_link_libraries(crawler_tests PRIVATE test_site GTest::gmock GTest::gtest_main)
target_compile_options(crawler_tests PRIVATE ${crawler_warnings})

include(GoogleTest)
gtest_discover_tests(crawler_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 5)

#
# A site on loopback to point the program at by hand, as the README shows.
#
add_executable(demo_site demo_site.cpp)
target_link_libraries(demo_site PRIVATE test_site)
target_compile_options(demo_site PRIVATE ${crawler_warnings})

#
# What the program itself does ahead of any connection. Everything else is in the tests above.
#
add_test(NAME crawler_refuses_no_hosts COMMAND crawler)
set_tests_properties(crawler_refuses_no_hosts PROPERTIES
    PASS_REGULAR_EXPRESSION "nothing to do: no host names")

add_test(NAME crawler_refuses_a_url COMMAND crawler https://localhost/)
set_tests_properties(crawler_refuses_a_url PROPERTIES
    PASS_REGULAR_EXPRESSION "is not a host name")

add_test(NAME crawler_refuses_a_file_it_can_not_read COMMAND crawler --file /nonexistent/list)
set_tests_properties(crawler_refuses_a_file_it_can_not_read PROPERTIES
    PASS_REGULAR_EXPRESSION "can't read")
