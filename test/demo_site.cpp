//
// A site on loopback, with an HTTPS record that sends the program to it over HTTP/3, and the
// command to request it with. Runs for as long as it takes to stop it.
//
#include "site.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <spdlog/spdlog.h>

#include <print>

int main()
{
   spdlog::set_level(spdlog::level::err);

   boost::asio::io_context context;
   crawler::test::Site site(context.get_executor());

   site.dns().add_https("localhost", 1, ".",
                        {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}},
                         {ARES_SVCB_PARAM_PORT,
                          {static_cast<unsigned char>(site.port() >> 8),
                           static_cast<unsigned char>(site.port() & 0xff)}}});

   site.page("/", {.body = "<p>hello</p>"});

   std::println("crawler --dns-server {} --ca-file {} localhost", site.dns_address(),
                site.ca_file().string());
   std::fflush(stdout);

   boost::asio::signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait([&](auto, auto) { site.stop(); });
   context.run();
}
