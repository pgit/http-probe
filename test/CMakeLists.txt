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
target_link_libraries(fake_dns PUBLIC c_ares_async::corosio anyhttp::anyhttp OpenSSL::SSL
                                      OpenSSL::Crypto)

add_library(test_site STATIC site.cpp)
target_include_directories(test_site PUBLIC .)
target_link_libraries(test_site PUBLIC http_probe_core fake_dns)
target_compile_options(test_site PRIVATE ${http_probe_warnings})

add_executable(http_probe_tests output_test.cpp prober_test.cpp routes_test.cpp)
target_link_libraries(http_probe_tests PRIVATE test_site GTest::gmock GTest::gtest_main)
target_compile_options(http_probe_tests PRIVATE ${http_probe_warnings})

include(GoogleTest)
gtest_discover_tests(http_probe_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 5)

#
# A site on loopback to point the program at by hand, as the README shows.
#
add_executable(demo_site demo_site.cpp)
target_link_libraries(demo_site PRIVATE test_site)
target_compile_options(demo_site PRIVATE ${http_probe_warnings})

#
# What the program itself does ahead of any connection. Everything else is in the tests above.
#
add_test(NAME http_probe_refuses_no_hosts COMMAND http-probe)
set_tests_properties(http_probe_refuses_no_hosts PROPERTIES
    PASS_REGULAR_EXPRESSION "nothing to do: no host names")

add_test(NAME http_probe_refuses_a_url COMMAND http-probe https://localhost/)
set_tests_properties(http_probe_refuses_a_url PROPERTIES
    PASS_REGULAR_EXPRESSION "is not a host name")

add_test(NAME http_probe_refuses_a_file_it_can_not_read COMMAND http-probe --file /nonexistent/list)
set_tests_properties(http_probe_refuses_a_file_it_can_not_read PROPERTIES
    PASS_REGULAR_EXPRESSION "can't read")

add_test(NAME http_probe_refuses_a_concurrency_of_zero
         COMMAND http-probe --concurrency 0 localhost)
set_tests_properties(http_probe_refuses_a_concurrency_of_zero PROPERTIES
    PASS_REGULAR_EXPRESSION "concurrency of 0")

add_test(NAME http_probe_refuses_a_negative_concurrency
         COMMAND http-probe --concurrency=-1 localhost)
set_tests_properties(http_probe_refuses_a_negative_concurrency PROPERTIES
    PASS_REGULAR_EXPRESSION "--concurrency can't be negative")

add_test(NAME http_probe_refuses_a_negative_timeout COMMAND http-probe --timeout-ms -1 localhost)
set_tests_properties(http_probe_refuses_a_negative_timeout PROPERTIES
    PASS_REGULAR_EXPRESSION "--timeout-ms can't be negative")

add_test(NAME http_probe_refuses_an_unknown_option COMMAND http-probe --no-such-option localhost)
set_tests_properties(http_probe_refuses_an_unknown_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unrecognised option '--no-such-option'")

add_test(NAME http_probe_describes_its_options COMMAND http-probe --help)
set_tests_properties(http_probe_describes_its_options PROPERTIES
    PASS_REGULAR_EXPRESSION "--dns-server SERVER")

add_test(NAME http_probe_refuses_dot_without_a_name COMMAND http-probe --dot localhost)
set_tests_properties(http_probe_refuses_dot_without_a_name PROPERTIES
    PASS_REGULAR_EXPRESSION "--dot needs a name")

add_test(NAME http_probe_refuses_tls_options_without_dot
         COMMAND http-probe --tls-no-verify localhost)
set_tests_properties(http_probe_refuses_tls_options_without_dot PROPERTIES
    PASS_REGULAR_EXPRESSION "only apply with --dot")

add_test(NAME http_probe_takes_dns_only
         COMMAND http-probe --dns-only --dns-server 127.0.0.1:1 localhost)
set_tests_properties(http_probe_takes_dns_only PROPERTIES
    PASS_REGULAR_EXPRESSION "1 lookups failed")

add_test(NAME http_probe_refuses_each_address_with_dns_only
         COMMAND http-probe --each-address --dns-only localhost)
set_tests_properties(http_probe_refuses_each_address_with_dns_only PROPERTIES
    PASS_REGULAR_EXPRESSION "rules out --each-address")

# An empty list gets as far as having no hosts, and goes nowhere.
add_test(NAME http_probe_takes_each_address_with_a_list
         COMMAND http-probe --each-address --file /dev/null)
set_tests_properties(http_probe_takes_each_address_with_a_list PROPERTIES
    PASS_REGULAR_EXPRESSION "no host names given")

#
# The levels of logging, as far as they show without a connection: the lookups fail on a port of
# loopback that nothing listens on, so this needs nothing but the program.
#
add_test(NAME http_probe_logs_its_steps_with_v
         COMMAND http-probe -v --dns-server 127.0.0.1:1 localhost)
set_tests_properties(http_probe_logs_its_steps_with_v PROPERTIES
    PASS_REGULAR_EXPRESSION "asking for the HTTPS records"
    FAIL_REGULAR_EXPRESSION "--- 0 ---")

add_test(NAME http_probe_shows_the_turns_of_the_event_loop_with_vv
         COMMAND http-probe -vv --dns-server 127.0.0.1:1 localhost)
set_tests_properties(http_probe_shows_the_turns_of_the_event_loop_with_vv PROPERTIES
    PASS_REGULAR_EXPRESSION "--- 0 ---")
