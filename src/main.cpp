//
// http-probe [options] HOST...
//
#include "output.hpp"
#include "prober.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
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
  --all               also print the hosts without HTTP/3, and why
  --color             colour the output even where it does not go to a terminal, e.g. into 'less -R'
  -v, --verbose       log to stderr what is going on, the requests and responses of anyhttp and the
                      queries of c-ares among it; -vv for all of QUIC as well, which is a lot. To
                      make sense of it, use --concurrency 1.

The exit code is 0 if every host with HTTP/3 answered, whatever the status, and every lookup got an
answer.
)";

struct Arguments
{
   http_probe::Config config;
   bool all = false;
   bool color = false;
   int verbosity = 0;
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

      if (argument == "--all")
      {
         arguments.all = true;
         continue;
      }
      if (argument == "--color")
      {
         arguments.color = true;
         continue;
      }
      if (argument == "-v" || argument == "--verbose" || argument == "-vv")
      {
         arguments.verbosity += argument == "-vv" ? 2 : 1;
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

//
// Everywhere that anyhttp and c-ares log to, as both use the default logger. On stderr, so that the
// results can go somewhere by themselves.
//
void set_up_logging(int verbosity, bool color)
{
   spdlog::set_default_logger(spdlog::stderr_color_mt(
      "http-probe", color ? spdlog::color_mode::always : spdlog::color_mode::automatic));
   spdlog::set_pattern("%H:%M:%S.%e %^%l%$ %v");
   spdlog::set_level(verbosity >= 2   ? spdlog::level::trace
                     : verbosity == 1 ? spdlog::level::debug
                                      : spdlog::level::err);
}

} // namespace

int main(int argc, char* argv[])
{
   using Outcome = http_probe::Result::Outcome;

   http_probe::Counts counts;
   std::size_t with_records = 0;

   try
   {
      auto [config, all, color, verbosity] = parse_arguments(argc, argv);
      set_up_logging(verbosity, color);

      const bool own_dns_server = !config.dns_server.empty();
      const bool color_results = color || isatty(STDOUT_FILENO);
      const bool color_summary = color || isatty(STDERR_FILENO);

      const auto report = [&](const http_probe::Result& result) {
         ++counts[result.outcome];
         if (!result.records.empty() || result.aliased)
            ++with_records;
         if (all || result.outcome != Outcome::no_http3)
         {
            std::print("{}", http_probe::format_result(result, color_results));
            std::fflush(stdout); // a run can take a while, and it is worth watching
         }
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
         std::println(stderr, "{}", http_probe::format_summary(counts, color_summary));
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
