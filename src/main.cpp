//
// crawler --allow HOST [--allow HOST ...] [options] URL...
//
#include "crawler.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view usage =
   R"(usage: crawler --allow HOST [--allow HOST ...] [options] URL...

Crawls the URLs and what they link to, on the given hosts only. How to connect to a host is up to
its HTTPS records in DNS (RFC 9460).

  --allow HOST        a host that may be connected to, by its exact name; at least one is required
  --dns-server ADDR   the DNS server to ask, like 192.0.2.1 or 192.0.2.1:5353; default: the system's
  --ca-file PEM       the CA certificates to verify servers with; default: the system's
  --max-pages N       the most pages to request; default: 100
  --delay-ms N        the least time between two requests to a host; default: 1000
  --timeout-ms N      how long one request may take; default: 10000
)";

template <typename T>
T number(std::string_view option, std::string_view text)
{
   T value{};
   const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
   if (ec != std::errc{} || end != text.data() + text.size())
      throw std::invalid_argument(std::format("{}: '{}' is not a number", option, text));
   return value;
}

crawler::Config parse_arguments(int argc, char* argv[])
{
   crawler::Config config;
   for (int i = 1; i < argc; ++i)
   {
      const std::string_view argument = argv[i];
      if (!argument.starts_with("-"))
      {
         config.seeds.emplace_back(argument);
         continue;
      }

      if (i + 1 == argc)
         throw std::invalid_argument(std::format("{} needs a value, or is unknown", argument));
      const std::string_view value = argv[++i];

      if (argument == "--allow")
         config.allowed_hosts.emplace_back(value);
      else if (argument == "--dns-server")
         config.dns_server = value;
      else if (argument == "--ca-file")
         config.tls_ca_file = value;
      else if (argument == "--max-pages")
         config.max_pages = number<std::size_t>(argument, value);
      else if (argument == "--delay-ms")
         config.delay = std::chrono::milliseconds(number<unsigned>(argument, value));
      else if (argument == "--timeout-ms")
         config.timeout = std::chrono::milliseconds(number<unsigned>(argument, value));
      else
         throw std::invalid_argument(std::format("unknown option '{}'", argument));
   }
   return config;
}

} // namespace

int main(int argc, char* argv[])
{
   spdlog::set_level(spdlog::level::err); // anyhttp logs through the default logger

   std::size_t fetched = 0;
   const auto report = [&](const crawler::Event& event) {
      using Kind = crawler::Event::Kind;
      switch (event.kind)
      {
      case Kind::fetched:
         ++fetched;
         std::println("{} {} [{}] {}", event.status, event.url, event.route, event.detail);
         break;
      case Kind::failed:
         std::println("failed {}: {}", event.url, event.detail);
         break;
      case Kind::disallowed:
         std::println("disallowed by robots.txt: {}", event.url);
         break;
      case Kind::outside_allowlist:
         std::println("not allowed: {}", event.url);
         break;
      case Kind::note:
         std::println("# {}: {}{}", event.url,
                      event.route.empty() ? "" : std::format("[{}] ", event.route), event.detail);
         break;
      }
   };

   try
   {
      boost::asio::io_context context;
      crawler::Crawler crawl(context.get_executor(), parse_arguments(argc, argv), report);
      boost::asio::co_spawn(context, crawl.run(), [](const std::exception_ptr& ex) {
         if (ex)
            std::rethrow_exception(ex);
      });
      context.run();
   }
   catch (const std::invalid_argument& ex)
   {
      std::println(stderr, "crawler: {}\n\n{}", ex.what(), usage);
      return 2;
   }
   catch (const std::exception& ex)
   {
      std::println(stderr, "crawler: {}", ex.what());
      return 1;
   }

   return fetched > 0 ? 0 : 1;
}
