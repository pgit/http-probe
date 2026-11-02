//
// A small site on loopback, with an HTTPS record that sends the crawler to it over HTTP/3, and the
// command to crawl it with. Runs for as long as it takes to stop it.
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

   site.page("/robots.txt",
             {.body = "User-agent: *\nDisallow: /private/\n", .content_type = "text/plain"});
   site.page("/", {.body = R"(<a href="/about">about</a> <a href="/private/x">private</a>
                              <a href="https://www.example.test/">elsewhere</a>)"});
   site.page("/about", {.body = R"(<a href="/">home</a> <a href="/gone">gone</a>)"});

   std::println("crawler --allow localhost --dns-server {} --ca-file {} --delay-ms 100 "
                "https://localhost/",
                site.dns_address(), site.ca_file().string());
   std::fflush(stdout);

   boost::asio::signal_set signals(context, SIGINT, SIGTERM);
   signals.async_wait([&](auto, auto) { site.stop(); });
   context.run();
}
