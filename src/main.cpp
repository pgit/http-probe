//
// crawler [options] HOST...
//
#include "prober.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view usage = R"(usage: crawler [options] HOST...
       crawler [options] --file LIST

Requests https://HOST/ once for each host name and prints the status and header fields of the
response. How to connect is up to the host's HTTPS records in DNS (RFC 9460). Redirects are reported
and not followed, and nothing else on the host is requested.

  --file LIST         read the hosts from a file, one per line, as names or as URLs
  --dns-server ADDR   the DNS server to ask, like 192.0.2.1 or 192.0.2.1:5353; default: the system's
  --ca-file PEM       the CA certificates to verify servers with; default: the system's
  --timeout-ms N      how long one request may take; default: 10000
)";

crawler::Config parse_arguments(int argc, char* argv[])
{
   crawler::Config config;
   for (int i = 1; i < argc; ++i)
   {
      const std::string_view argument = argv[i];
      if (!argument.starts_with("-"))
      {
         config.hosts.emplace_back(argument);
         continue;
      }

      if (i + 1 == argc)
         throw std::invalid_argument(std::format("{} needs a value, or is unknown", argument));
      const std::string_view value = argv[++i];

      if (argument == "--file")
      {
         std::ifstream list{std::string(value)};
         if (!list)
            throw std::invalid_argument(std::format("can't read '{}'", value));
         std::vector<std::string> skipped;
         config.hosts.append_range(crawler::read_hosts(list, skipped));
         for (const auto& line : skipped)
            std::println(stderr, "crawler: skipping '{}': not a host name", line);
      }
      else if (argument == "--dns-server")
         config.dns_server = value;
      else if (argument == "--ca-file")
         config.tls_ca_file = value;
      else if (argument == "--timeout-ms")
      {
         unsigned milliseconds = 0;
         const auto [end, ec] =
            std::from_chars(value.data(), value.data() + value.size(), milliseconds);
         if (ec != std::errc{} || end != value.data() + value.size())
            throw std::invalid_argument(std::format("{}: '{}' is not a number", argument, value));
         config.timeout = std::chrono::milliseconds(milliseconds);
      }
      else
         throw std::invalid_argument(std::format("unknown option '{}'", argument));
   }
   return config;
}

void print(const crawler::Result& result)
{
   if (result.error.empty())
      std::println("{}: {} [{}] {} bytes in {}", result.host, result.status, result.route,
                   result.body_size, result.duration);
   else
      std::println("{}: failed: {}", result.host, result.error);

   for (const auto& record : result.records)
      std::println("  record: {}", crawler::to_text(record));
   for (const auto& note : result.notes)
      std::println("  note: {}", note);
   for (const auto& [name, value] : result.fields)
      std::println("  {}: {}", name, value);
}

} // namespace

int main(int argc, char* argv[])
{
   spdlog::set_level(spdlog::level::err); // anyhttp logs through the default logger

   bool all_answered = true;
   const auto report = [&](const crawler::Result& result) {
      all_answered = all_answered && result.error.empty();
      print(result);
   };

   try
   {
      boost::asio::io_context context;
      crawler::Prober prober(context.get_executor(), parse_arguments(argc, argv), report);
      boost::asio::co_spawn(context, prober.run(), [](const std::exception_ptr& ex) {
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

   return all_answered ? 0 : 1;
}
