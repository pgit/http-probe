//
// A web site with DNS of its own, all on loopback, for the program to be pointed at: an anyhttp
// server on one port, for every protocol, and a DNS server with whatever zone the test wants. The
// server has a certificate for "localhost". That name comes from /etc/hosts and not from the DNS
// server, which a test can only have the last word on with a name that the certificate is not for.
//
#pragma once

#include <anyhttp/server.hpp>

#include <fake_dns_server.hpp>
#include <test_certificate.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace http_probe::test
{

// =================================================================================================

class Site
{
public:
   struct Page
   {
      unsigned status = 200;
      std::string body;
      std::string content_type = "text/html";
      std::string location;
      std::chrono::milliseconds delay{}; ///< how long the server takes to get to it
   };

   /// One request that arrived.
   struct Request
   {
      std::string target;
      std::string authority;
      std::string user_agent;
      std::chrono::steady_clock::time_point time;
   };

   explicit Site(const anyhttp::Executor& executor);
   ~Site();

   /// A path that has no page gets a 404.
   void page(std::string path, Page page) { pages_[std::move(path)] = std::move(page); }

   std::uint16_t port() const { return port_; }
   const std::filesystem::path& ca_file() const { return certificate_file_; }

   /// The zone. Good until stop(), as opposed to what was asked of it, see dns_queries().
   fake_dns_server& dns() { return *dns_; }
   std::string dns_address() const { return dns_->address(); }

   /// How often the DNS server was asked about \p name, also after stop().
   std::size_t dns_queries(std::string_view name) const;

   /// Everything the server was asked so far, in order, and all of the targets alone.
   const std::vector<Request>& requests() const { return *requests_; }
   std::vector<std::string> targets() const;

   /// Ends the servers, which is what lets the context run dry.
   void stop();

private:
   void start(const anyhttp::Executor& executor);

   std::filesystem::path directory_;
   std::filesystem::path certificate_file_;
   std::filesystem::path key_file_;
   std::map<std::string, Page> pages_;
   std::shared_ptr<std::vector<Request>> requests_;
   std::optional<anyhttp::server::Server> server_;
   std::optional<fake_dns_server> dns_;
   std::vector<fake_dns_server::query> queries_; // what the DNS server knew when it was stopped
   std::uint16_t port_ = 0;
};

// =================================================================================================

} // namespace http_probe::test
