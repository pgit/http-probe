#include "site.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/system/system_error.hpp>

#include "routes.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <system_error>
#include <tuple>

namespace capy = boost::capy;
namespace corosio = boost::corosio;
namespace http = boost::beast::http;

namespace http_probe::test
{

// =================================================================================================

Site::Site(const capy::any_executor& executor) : requests_(std::make_shared<std::vector<Request>>())
{
   auto directory = (std::filesystem::temp_directory_path() / "http-probe-test-XXXXXX").string();
   if (!mkdtemp(directory.data()))
      throw std::runtime_error("mkdtemp");
   directory_ = directory;
   certificate_file_ = directory_ / "certificate.pem";
   key_file_ = directory_ / "key.pem";

   const auto certificate = make_test_certificate("localhost");
   std::ofstream(certificate_file_) << certificate.certificate_pem;
   std::ofstream(key_file_) << certificate.key_pem;

   start(executor);
   port_ = server_->local_endpoint().port();
   capy::run_async(executor, polling_.get_token())(poll_dns());

   server_->on_request(
      [this, requests = requests_](anyhttp::server::Request request,
                                   anyhttp::server::Response response) -> capy::task<> {
         requests->push_back(Request{
            .target = std::string(request.url().encoded_target()),
            .authority = std::string(request.url().encoded_host_and_port()),
            .user_agent = std::string(request.fields()[http::field::user_agent]),
            .time = std::chrono::steady_clock::now(),
         });

         std::array<std::uint8_t, 1024> buffer;
         for (;;)
            if (auto [ec, n] = co_await request.read_some(capy::make_buffer(buffer)); ec || n == 0)
               break;

         const auto it = pages_.find(std::string(request.url().encoded_path()));
         const auto page = it != pages_.end() ? it->second : Page{.status = 404};

         if (page.delay.count() > 0)
            std::ignore = co_await corosio::delay(page.delay);

         anyhttp::Fields fields;
         fields.set(http::field::content_type, page.content_type);
         if (!page.location.empty())
            fields.set(http::field::location, page.location);

         if (auto [ec] = co_await response.submit(page.status, std::move(fields)); ec)
            co_return;
         co_await response.write_eof(boost::asio::buffer(page.body)); // has no overload for Capy's
      });
}

//
// Port 0 finds a free port for one protocol, and then the servers need the same number for the
// other: anyhttp's has TCP and UDP on one port, and so does the DNS server. With tests running at
// the same time, another one may have the other half by then, so this is tried a few times.
//
void Site::start(const capy::any_executor& executor)
{
   for (int attempt = 1;; ++attempt)
   {
      const auto try_again = [&](std::error_code ec) {
         return ec == std::errc::address_in_use && attempt < 20;
      };

      try
      {
         //
         // "::" takes IPv4 as well, and "localhost" may be either.
         //
         server_.emplace(executor,
                         anyhttp::server::Config{.listen_address = "::",
                                                 .port = 0,
                                                 .tls_certificate_chain = certificate_file_,
                                                 .tls_private_key = key_file_});
         dns_.emplace(dns_context_.get_executor());
         return;
      }
      catch (const std::system_error& ex) // from the server, on Corosio
      {
         if (!try_again(ex.code()))
            throw;
      }
      catch (const boost::system::system_error& ex) // from the DNS server, on Asio
      {
         if (!try_again(ex.code()))
            throw;
      }
      server_.reset();
      dns_.reset();
   }
}

//
// A millisecond at most for the DNS server to get to a question, which nothing here would notice.
// Once stopped, this does not look at the site anymore, which may be gone by the time it gets here.
//
capy::task<> Site::poll_dns()
{
   const std::stop_token stopped = co_await capy::this_coro::stop_token;
   while (!stopped.stop_requested())
   {
      dns_context_.restart();
      dns_context_.poll();
      std::ignore = co_await corosio::delay(std::chrono::milliseconds(1));
   }
}

Site::~Site()
{
   polling_.request_stop();
   std::error_code ignored;
   std::filesystem::remove_all(directory_, ignored);
}

std::vector<std::string> Site::targets() const
{
   std::vector<std::string> targets;
   for (const auto& request : *requests_)
      targets.push_back(request.target);
   return targets;
}

std::size_t Site::dns_queries(std::string_view name) const
{
   const auto& queries = dns_ ? dns_->queries() : queries_;
   return std::ranges::count_if(queries, [&](const auto& query) {
      return normalize_host(query.name) == normalize_host(name);
   });
}

void Site::stop()
{
   server_.reset();
   if (dns_)
      queries_ = dns_->queries();
   dns_.reset();
   polling_.request_stop();
}

// =================================================================================================

} // namespace http_probe::test
