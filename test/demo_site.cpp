//
// A site on loopback, with an HTTPS record that sends the program to it over HTTP/3, and the
// command to request it with. Runs for as long as it takes to stop it.
//
#include "site.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/signal_set.hpp>

#include <spdlog/spdlog.h>

#include <csignal>
#include <print>
#include <tuple>

namespace capy = boost::capy;
namespace corosio = boost::corosio;

static capy::task<> stop_on_signal(corosio::signal_set& signals, http_probe::test::Site& site)
{
   std::ignore = co_await signals.wait();
   site.stop();
}

int main()
{
   spdlog::set_level(spdlog::level::err);

   corosio::io_context context;
   http_probe::test::Site site(context.get_executor());

   site.dns().add_https("localhost", 1, ".",
                        {{.key = ARES_SVCB_PARAM_ALPN, .value = {2, 'h', '3'}},
                         {.key = ARES_SVCB_PARAM_PORT,
                          .value = {static_cast<unsigned char>(site.port() >> 8),
                                    static_cast<unsigned char>(site.port() & 0xff)}}});

   site.page("/", {.body = "<p>hello</p>"});

   std::println("http-probe --dns-server {} --ca-file {} localhost", site.dns_address(),
                site.ca_file().string());
   (void)std::fflush(stdout);

   corosio::signal_set signals(context, SIGINT, SIGTERM);
   capy::run_async(context.get_executor())(stop_on_signal(signals, site));
   context.run();
}
