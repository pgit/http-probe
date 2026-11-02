//
// http-probe [options] HOST...
//
#include "prober.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view usage = R"(usage: http-probe [options] HOST...
       http-probe [options] --file LIST

Looks up the HTTPS records of each host (RFC 9460), and requests https://HOST/ once from those that
offer HTTP/3, printing the status and header fields of the response. Redirects are reported and not
followed, and nothing else on the host is requested.

  --file LIST         read the hosts from a file, one per line, as names or as URLs
  --dns-server ADDR   the DNS server to ask, like 192.0.2.1 or 192.0.2.1:5353; default: the system's,
                      which may well not answer for HTTPS records
  --ca-file PEM       the CA certificates to verify servers with; default: the system's
  --concurrency N     how many hosts to work on at the same time; default: 16
  --timeout-ms N      how long one request may take; default: 10000
  --verbose           also print the hosts without HTTP/3, and why

The exit code is 0 if every host with HTTP/3 answered, whatever the status, and every lookup got an
answer.
)";

struct Arguments
{
   http_probe::Config config;
   bool verbose = false;
};

template <typename T>
T number(std::string_view option, std::string_view text)
{
   T value{};
   const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
   if (ec != std::errc{} || end != text.data() + text.size())
      throw std::invalid_argument(std::format("{}: '{}' is not a number", option, text));
   return value;
}

Arguments parse_arguments(int argc, char* argv[])
{
   Arguments arguments;
   auto& config = arguments.config;
   for (int i = 1; i < argc; ++i)
   {
      const std::string_view argument = argv[i];
      if (!argument.starts_with("-"))
      {
         config.hosts.emplace_back(argument);
         continue;
      }

      if (argument == "--verbose")
      {
         arguments.verbose = true;
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
         config.hosts.append_range(http_probe::read_hosts(list, skipped));
         for (const auto& line : skipped)
            std::println(stderr, "http-probe: skipping '{}': not a host name", line);
      }
      else if (argument == "--dns-server")
         config.dns_server = value;
      else if (argument == "--ca-file")
         config.tls_ca_file = value;
      else if (argument == "--concurrency")
         config.concurrency = number<std::size_t>(argument, value);
      else if (argument == "--timeout-ms")
         config.timeout = std::chrono::milliseconds(number<unsigned>(argument, value));
      else
         throw std::invalid_argument(std::format("unknown option '{}'", argument));
   }
   return arguments;
}

using Outcome = http_probe::Result::Outcome;

void print(const http_probe::Result& result)
{
   switch (result.outcome)
   {
   case Outcome::answered:
      std::println("{}: {} [{}] {} bytes in {}", result.host, result.status, result.route,
                   result.body_size, result.duration);
      break;
   case Outcome::failed:
      std::println("{}: failed: {}", result.host, result.error);
      break;
   case Outcome::out_of_reach:
      std::println("{}: HTTP/3 out of reach", result.host);
      break;
   case Outcome::no_http3:
      std::println("{}: no HTTP/3", result.host);
      break;
   case Outcome::lookup_failed:
      std::println("{}: lookup failed", result.host);
      break;
   }

   for (const auto& record : result.records)
      std::println("  record: {}", http_probe::to_text(record));
   for (const auto& note : result.notes)
      std::println("  note: {}", note);
   for (const auto& [name, value] : result.fields)
      std::println("  {}: {}", name, value);

   std::fflush(stdout); // a run can take a while, and it is worth watching
}

} // namespace

int main(int argc, char* argv[])
{
   spdlog::set_level(spdlog::level::err); // anyhttp logs through the default logger

   std::map<Outcome, std::size_t> counts;
   std::size_t with_records = 0;

   try
   {
      auto [config, verbose] = parse_arguments(argc, argv);
      const bool own_dns_server = !config.dns_server.empty();

      const auto report = [&, verbose](const http_probe::Result& result) {
         ++counts[result.outcome];
         if (!result.records.empty() || result.aliased)
            ++with_records;
         if (verbose || result.outcome != Outcome::no_http3)
            print(result);
      };

      boost::asio::io_context context;
      http_probe::Prober prober(context.get_executor(), std::move(config), report);
      boost::asio::co_spawn(context, prober.run(), [&](const std::exception_ptr& ex) {
         if (ex)
            std::rethrow_exception(ex);

         //
         // Here and not after run(): a host that did not answer can keep the context from running
         // dry for a while, see the README, and this is what the wait would be for.
         //
         const auto count = [&](Outcome outcome) { return counts[outcome]; };
         std::println(stderr,
                      "{} answered, {} failed, {} with HTTP/3 out of reach, {} without HTTP/3, "
                      "{} lookups failed",
                      count(Outcome::answered), count(Outcome::failed),
                      count(Outcome::out_of_reach), count(Outcome::no_http3),
                      count(Outcome::lookup_failed));
         if (with_records == 0 && !own_dns_server)
            std::println(stderr, "http-probe: none of the hosts has an HTTPS record. Not every "
                                 "resolver answers for them, so try --dns-server.");
      });
      context.run();
   }
   catch (const std::invalid_argument& ex)
   {
      std::println(stderr, "http-probe: {}\n\n{}", ex.what(), usage);
      return 2;
   }
   catch (const std::exception& ex)
   {
      std::println(stderr, "http-probe: {}", ex.what());
      return 1;
   }

   return counts[Outcome::failed] == 0 && counts[Outcome::lookup_failed] == 0 ? 0 : 1;
}
