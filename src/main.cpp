//
// http-probe [options] HOST...
//
#include "output.hpp"
#include "prober.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/program_options.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace po = boost::program_options;

namespace
{

constexpr std::string_view usage = R"(usage: http-probe [options] HOST...
       http-probe [options] --file LIST
)";

constexpr std::string_view about =
   "Looks up the HTTPS records of each host (RFC 9460), and requests https://HOST/ once, the way "
   "they say or over TCP on port 443 if they don't, printing the status and header fields of the "
   "response. Redirects are reported and not followed, and nothing else on the host is requested.";

constexpr std::string_view exit_code =
   "The exit code is 0 if every host that was connected to answered, whatever the status, and "
   "every lookup got an answer.";

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
// How wide to print the help. Either stream will do, as they are on the same terminal, and with
// 'http-probe --help | less' only stderr still is one.
//
unsigned help_columns()
{
   for (const int fd : {STDOUT_FILENO, STDERR_FILENO})
   {
      winsize ws{};
      if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40)
         return ws.ws_col;
   }
   return po::options_description::m_default_line_length;
}

//
// A paragraph broken at its spaces into lines that fit.
//
std::string wrap(std::string_view paragraph, unsigned columns)
{
   std::string wrapped;
   std::size_t line = 0;
   for (const auto range : paragraph | std::views::split(' '))
   {
      const std::string_view word{range};
      if (line > 0 && line + 1 + word.size() > columns)
      {
         wrapped += '\n';
         line = 0;
      }
      else if (line > 0)
      {
         wrapped += ' ';
         ++line;
      }
      wrapped += word;
      line += word.size();
   }
   return wrapped;
}

//
// The options, stored into 'arguments', which has to outlive parsing them.
//
po::options_description describe_options(Arguments& arguments)
{
   auto& config = arguments.config;

   const auto columns = help_columns();
   po::options_description desc("Options", columns, columns / 2);
   auto opts = desc.add_options();
   opts("help,h", po::bool_switch(&arguments.help), "produce this help message");
   opts("host", po::value(&config.hosts)->value_name("HOST"),
        "a host to request, the same as giving HOST by itself");
   opts("file", po::value(&arguments.files)->value_name("LIST"),
        "read the hosts from a file, one per line, as names or as URLs");
   opts("dns-server", po::value(&config.dns_server)->value_name("SERVER"),
        "the DNS servers to ask, comma separated, by address or by name, like 192.0.2.1, "
        "192.0.2.1:5353 or dns.example.com; default: the system's, which may well not answer for "
        "HTTPS records");
   opts("dot", po::bool_switch(&config.dns_tls.enabled),
        "ask the DNS servers over TLS (RFC 7858), which moves them to port 853 unless they say "
        "otherwise");
   opts("tls-hostname", po::value(&config.dns_tls.hostname)->value_name("NAME"),
        "with --dot, the name to use for SNI and to verify the certificate of the DNS server "
        "against, like one.one.one.one; default: the host name in --dns-server");
   opts("tls-no-verify", po::value<bool>()->zero_tokens()->notifier([&config](bool) {
      config.dns_tls.verify = false;
   }),
        "with --dot, accept any certificate from the DNS server (testing only); does not apply to "
        "the hosts");
   opts("ca-file", po::value(&config.tls_ca_file)->value_name("PEM"),
        "the CA certificates to verify the hosts with; default: the system's. Not for --dot, which "
        "goes by the trust store of OpenSSL, as SSL_CERT_FILE can say");
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
   opts("h3", po::bool_switch(&config.protocols.h3),
        "speak HTTP/3, where an HTTPS record offers it. Of --h3, --h2 and --h1, only those given "
        "are spoken, all of them without any, and a host that offers none of them is left out");
   opts("h2", po::bool_switch(&config.protocols.h2),
        "speak HTTP/2, over TCP where a record offers it or there is none. Without --h1, a server "
        "that chooses HTTP/1.1 is not sent the request");
   opts("h1", po::bool_switch(&config.protocols.h1),
        "speak HTTP/1.1, over TCP where a record offers it or there is none");
   opts("dns-only", po::bool_switch(&config.dns_only),
        "connect to nothing: stop after the lookups, and print the records and the addresses of "
        "each host");
   opts("each-address", po::bool_switch(&config.each_address),
        "send a request to each address of a host and print the status line of each, instead of "
        "one request per host");
   opts("all", po::bool_switch(&arguments.all),
        "also print the hosts that do not exist, and those that --h3, --h2 or --h1 leave out");
   opts("color", po::bool_switch(&arguments.color),
        "colour the output even where it does not go to a terminal, e.g. into 'less -R'");
   opts("verbose,v", po::value<std::vector<std::string>>()->zero_tokens()->composing(),
        "log to stderr what is going on: the steps of this program and the queries of c-ares. -vv "
        "adds all that anyhttp has to say, with a line after each turn of the event loop, which "
        "is a lot. To make sense of it, use --concurrency 1.");
   return desc;
}

void print_usage(std::ostream& out, const po::options_description& options)
{
   const auto columns = help_columns();
   out << usage << '\n'
       << wrap(about, columns) << "\n\n"
       << options << '\n'
       << wrap(exit_code, columns) << '\n';
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
// Sets up the two loggers, on stderr so that the results can go somewhere by themselves, and
// returns the one for this program and c-ares. The default logger is left to anyhttp, which uses
// nothing else, and that gets it a level of its own.
//
std::shared_ptr<spdlog::logger> set_up_logging(int verbosity, bool color)
{
   const auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(
      color ? spdlog::color_mode::always : spdlog::color_mode::automatic);
   sink->set_pattern("%H:%M:%S.%e %^%l%$ %v");

   const auto anyhttp = std::make_shared<spdlog::logger>("anyhttp", sink);
   anyhttp->set_level(verbosity >= 2 ? spdlog::level::trace : spdlog::level::err);
   spdlog::set_default_logger(anyhttp);

   auto own = std::make_shared<spdlog::logger>("http-probe", sink);
   own->set_level(verbosity >= 1 ? spdlog::level::debug : spdlog::level::err);
   return own;
}

//
// Runs the context until it is out of work, one handler at a time, with a line after each, in red
// when it took 10 ms or more. That shows which turn of the event loop a log line belongs to. The
// lines are those of run() in anyhttp/utils.hpp, which prints them to stdout: here, they go with
// the log and stay out of the results.
//
std::size_t run_with_separators(boost::corosio::io_context& context, bool color)
{
   using namespace std::chrono;

   std::size_t i = 0;
   auto t0 = steady_clock::now();
   for (; context.run_one(); ++i)
   {
      const auto t1 = steady_clock::now();
      const auto dt = duration_cast<milliseconds>(t1 - t0);
      t0 = t1;
      // clang-format off
      if (dt < 10ms)
         std::println(stderr, "--- {} ------------------------------------------------------------------------", i);
      else
         std::println(stderr, "{}--- {} ({}) ----------------------------------------------------------------{}",
                      color ? "\x1b[1;31m" : "", i, dt, color ? "\x1b[0m" : "");
      // clang-format on
   }
   return i;
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
      config.logger = set_up_logging(verbosity, color);

      const bool own_dns_server = !config.dns_server.empty();
      const bool color_results = color || isatty(STDOUT_FILENO);
      const bool color_log = color || isatty(STDERR_FILENO);

      const auto report = [&](const http_probe::Result& result) {
         ++counts[result.outcome];
         if (!result.records.empty() || result.aliased)
            ++with_records;
         if (all || (result.outcome != Outcome::not_found && result.outcome != Outcome::left_out))
         {
            std::print("{}", http_probe::format_result(result, color_results));
            std::fflush(stdout); // a run can take a while, and it is worth watching
         }
      };

      boost::corosio::io_context context;
      http_probe::Prober prober(context.get_executor(), std::move(config), report);
      std::exception_ptr failed;

      //
      // What a handler throws ends the process, so this one leaves it for after run().
      //
      boost::capy::run_async(
         context.get_executor(), []() noexcept {},
         [&](std::exception_ptr ex) noexcept {
            failed = ex;
            context.stop();
         })(prober.run());
      if (verbosity >= 2)
         run_with_separators(context, color_log);
      else
         context.run();
      if (failed)
         std::rethrow_exception(failed);

      std::println(stderr, "{}", http_probe::format_summary(counts, color_log));
      if (with_records == 0 && !own_dns_server)
         std::println(stderr, "http-probe: none of the hosts has an HTTPS record. Not every "
                              "resolver answers for them, so try --dns-server.");
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
