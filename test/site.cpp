#include "site.hpp"

#include <boost/beast/http/field.hpp>

#include "routes.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace http = boost::beast::http;

namespace crawler::test
{

// =================================================================================================

Site::Site(const anyhttp::Executor& executor, const std::string& listen_address)
   : requests_(std::make_shared<std::vector<Request>>())
{
   auto directory = (std::filesystem::temp_directory_path() / "crawler-test-XXXXXX").string();
   if (!mkdtemp(directory.data()))
      throw std::runtime_error("mkdtemp");
   directory_ = directory;
   certificate_file_ = directory_ / "certificate.pem";
   key_file_ = directory_ / "key.pem";

   const auto certificate = make_test_certificate("localhost");
   std::ofstream(certificate_file_) << certificate.certificate_pem;
   std::ofstream(key_file_) << certificate.key_pem;

   server_.emplace(executor, anyhttp::server::Config{.listen_address = listen_address,
                                                     .port = 0,
                                                     .tls_certificate_chain = certificate_file_,
                                                     .tls_private_key = key_file_});
   port_ = server_->local_endpoint().port();

   server_->on_request([this, requests = requests_](
                          anyhttp::server::Request request,
                          anyhttp::server::Response response) -> anyhttp::Task<void> {
      requests->push_back(Request{
         .target = std::string(request.url().encoded_target()),
         .authority = std::string(request.url().encoded_host_and_port()),
         .user_agent = std::string(request.fields()[http::field::user_agent]),
         .time = std::chrono::steady_clock::now(),
      });

      std::array<std::uint8_t, 1024> buffer;
      for (;;)
         if (auto [ec, n] = co_await request.read_some(boost::asio::buffer(buffer)); ec || n == 0)
            break;

      const auto it = pages_.find(std::string(request.url().encoded_path()));
      const auto page = it != pages_.end() ? it->second : Page{.status = 404};

      anyhttp::Fields fields;
      fields.set(http::field::content_type, page.content_type);
      if (!page.location.empty())
         fields.set(http::field::location, page.location);

      if (auto [ec] = co_await response.submit(page.status, std::move(fields)); ec)
         co_return;
      co_await response.write_eof(boost::asio::buffer(page.body));
   });

   dns_.emplace(executor);
}

Site::~Site()
{
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
}

// =================================================================================================

} // namespace crawler::test
