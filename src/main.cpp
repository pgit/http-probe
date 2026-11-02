//
// Does not crawl anything yet. It sets up both libraries on one event loop, without connecting to
// or looking up anything, to show that they build and link into one process.
//
#include <anyhttp/client.hpp>
#include <c_ares_async.hpp>

#include <boost/asio/io_context.hpp>

#include <spdlog/spdlog.h>

int main()
{
   spdlog::set_level(spdlog::level::warn); // anyhttp logs through the default logger, info is noisy

   boost::asio::io_context context;
   anyhttp::client::Client client(context.get_executor(), anyhttp::client::Config{});
   c_ares_async::resolver resolver(context.get_executor());
   context.run();
}
