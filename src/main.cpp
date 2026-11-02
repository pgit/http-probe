//
// http-probe [options] HOST...
//
#include "output.hpp"
#include "prober.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/program_options.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace po = boost::program_options;

namespace
{

constexpr std::string_view about = R"(usage: http-probe [options] HOST...
       http-probe [options] --file LIST

Looks up the HTTPS records of each host (RFC 9460), and requests https://HOST/ once from those that
offer HTTP/3, printing the status and header fields of the response. Redirects are reported and not
followed, and nothing else on the host is requested.
)";

constexpr std::string_view exit_code = R"(
The exit code is 0 if every host with HTTP/3 answered, whatever the status, and every lookup got an
answer.
)";

struct Arguments
{
   http_probe::Config config;
   std::vector<std::string> files;
   bool help = false;
   bool all = false;
   bool color = false;
   int verbosity = 0;
};

//
// The numbers are read as signed ones, because lexical_cast takes -1 for the largest value of an
// unsigned type, which would pass as a choice.
//
int not_negative(std::string_view option, int value)
{
   if (value < 0)
      throw std::invalid_argument(std::format("{} can't be negative", option));
   return value;
}

//
// The options, stored into 'arguments', which has to outlive parsing them.
//
po::options_description describe_options(Arguments& arguments)
{
   auto& config = arguments.config;

   po::options_description desc("Options", 100, 50);
   auto opts = desc.add_options();
   opts("help,h", po::bool_switch(&arguments.help), "produce this help message");
   opts("host", po::value(&config.hosts)->value_name("HOST"),
        "a host to request, the same as giving HOST by itself");
   opts("file", po::value(&arguments.files)->value_name("LIST"),
        "read the hosts from a file, one per line, as names or as URLs");
   opts("dns-server", po::value(&config.dns_server)->value_name("ADDR"),
        "the DNS server to ask, like 192.0.2.1 or 192.0.2.1:5353; default: the system's, which may "
        "well not answer for HTTPS records");
   opts("ca-file", po::value(&config.tls_ca_file)->value_name("PEM"),
        "the CA certificates to verify servers with; default: the system's");
   opts("concurrency",
        po::value<int>()
           ->value_name("N")
           ->default_value(static_cast<int>(config.concurrency))
           ->notifier(
              [&config](int value) { config.concurrency = not_negative("--concurrency", value); }),
        "how many hosts to work on at the same time");
   opts("timeout-ms",
        po::value<int>()
           ->value_name("N")
           ->default_value(static_cast<int>(config.timeout.count()))
           ->notifier([&config](int value) {
              config.timeout = std::chrono::milliseconds(not_negative("--timeout-ms", value));
           }),
        "how long one request may take");
   opts("all", po::bool_switch(&arguments.all), "also print the hosts without HTTP/3, and why");
   opts("color", po::bool_switch(&arguments.color),
        "colour the output even where it does not go to a terminal, e.g. into 'less -R'");
   opts("verbose,v", po::value<std::vector<std::string>>()->zero_tokens()->composing(),
        "log to stderr what is going on, the requests and responses of anyhttp and the queries of "
        "c-ares among it; -vv for all of QUIC as well, which is a lot. To make sense of it, use "
        "--concurrency 1.");
   return desc;
}

void print_usage(std::ostream& out, const po::options_description& options)
{
   out << about << '\n' << options << exit_code;
}

void parse_arguments(int argc, char* argv[], const po::options_description& options,
                     Arguments& arguments)
{
   try
   {
      po::positional_options_description positional;
      positional.add("host", -1);

      const auto parsed =
         po::command_line_parser(argc, argv).options(options).positional(positional).run();

      po::variables_map vm;
      po::store(parsed, vm);
      po::notify(vm);

      // 'verbose' takes no argument, so its parsed value is always empty: count occurrences
      arguments.verbosity = static_cast<int>(std::ranges::count_if(
         parsed.options, [](const po::option& option) { return option.string_key == "verbose"; }));
   }
   catch (const po::error& ex)
   {
      throw std::invalid_argument(ex.what());
   }

   if (arguments.help)
      return;

   for (const auto& file : arguments.files)
   {
      std::ifstream list{file};
      if (!list)
         throw std::invalid_argument(std::format("can't read '{}'", file));
      std::vector<std::string> skipped;
      arguments.config.hosts.append_range(http_probe::read_hosts(list, skipped));
      for (const auto& line : skipped)
         std::println(stderr, "http-probe: skipping '{}': not a host name", line);
   }
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

   Arguments arguments;
   const auto options = describe_options(arguments);

   try
   {
      parse_arguments(argc, argv, options, arguments);
      if (arguments.help)
      {
         print_usage(std::cout, options);
         return 0;
      }

      auto& [config, files, help, all, color, verbosity] = arguments;
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
      std::println(stderr, "http-probe: {}\n", ex.what());
      print_usage(std::cerr, options);
      return 2;
   }
   catch (const std::exception& ex)
   {
      std::println(stderr, "http-probe: {}", ex.what());
      return 1;
   }

   return counts[Outcome::failed] == 0 && counts[Outcome::lookup_failed] == 0 ? 0 : 1;
}
