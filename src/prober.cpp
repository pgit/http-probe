#include "prober.hpp"

#include <anyhttp/session.hpp>

#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <set>
#include <stdexcept>
#include <variant>

namespace http = boost::beast::http;

namespace http_probe
{

namespace
{

// =================================================================================================

constexpr std::string_view user_agent = "http-probe/0.0.1 (+https://github.com/pgit/http-probe)";

constexpr int max_alias_chain = 8;

// =================================================================================================

} // namespace

bool is_host_name(std::string_view host)
{
   const auto is_name_char = [](unsigned char c) {
      return std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_';
   };
   boost::system::error_code not_an_address;
   boost::asio::ip::make_address(host, not_an_address);
   return !host.empty() && host.size() <= 253 && std::ranges::all_of(host, is_name_char) &&
          not_an_address;
}

std::vector<std::string> read_hosts(std::istream& list, std::vector<std::string>& skipped)
{
   std::vector<std::string> hosts;
   for (std::string line; std::getline(list, line);)
   {
      std::string_view text = line;
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
         text.remove_prefix(1);
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
         text.remove_suffix(1);
      if (text.empty() || text.starts_with('#'))
         continue;

      std::string host(text);
      if (text.find("://") != text.npos)
         if (const auto url = boost::urls::parse_uri(text))
            host = url->host_address();

      if (is_host_name(host))
         hosts.push_back(std::move(host));
      else
         skipped.emplace_back(text);
   }
   return hosts;
}

Prober::Prober(anyhttp::Executor executor, Config config, Observer observer)
   : executor_(executor), config_(std::move(config)), observer_(std::move(observer)),
     resolver_(std::move(executor))
{
   if (config_.hosts.empty())
      throw std::invalid_argument("nothing to do: no host names given");

   std::set<std::string> seen;
   for (const auto& host : config_.hosts)
   {
      if (!is_host_name(host))
         throw std::invalid_argument(std::format("'{}' is not a host name", host));
      if (auto name = normalize_host(host); seen.insert(name).second)
         hosts_.push_back(std::move(name));
   }

   if (!config_.dns_server.empty())
      if (const auto ec = resolver_.set_servers(config_.dns_server))
         throw std::invalid_argument(
            std::format("invalid DNS server '{}': {}", config_.dns_server, ec.message()));
}

anyhttp::Task<void> Prober::run()
{
   for (const auto& host : hosts_)
      observer_(co_await probe(host));
}

// -------------------------------------------------------------------------------------------------

//
// SVCB resolution, section 3 of RFC 9460, up to the point where it would take addresses. A lookup
// that fails is not fatal: DNS is not protected here, and this is the choice section 3.1 leaves
// for that. A name without a record is the common case and not even worth a note.
//
anyhttp::Task<ServiceRecords> Prober::resolve_service(const std::string& host)
{
   ServiceRecords found{.owner = host};
   std::set<std::string> visited{found.owner};

   for (int hop = 0; hop < max_alias_chain; ++hop)
   {
      //
      // Outside of any timeout: the image has c-ares 1.34.5, and cancelling a lookup is what
      // GHSA-6wfj-rwm7-3542 is about. c-ares has timeouts of its own.
      //
      auto [ec, records] = co_await resolver_.resolve_https(found.owner);
      if (ec)
      {
         if (ec != c_ares_async::make_ares_error(ARES_ENODATA) &&
             ec != c_ares_async::make_ares_error(ARES_ENOTFOUND))
            found.notes.push_back(
               std::format("HTTPS lookup of '{}': {}", found.owner, ec.message()));
         co_return found;
      }

      //
      // A record in AliasMode makes the rest of the set irrelevant (section 2.4.1).
      //
      const auto alias = std::ranges::find(records, 0, &c_ares_async::https_record::priority);
      if (alias == records.end())
      {
         found.records = std::move(records);
         co_return found;
      }

      if (alias->target.empty() || alias->target == ".")
      {
         //
         // "The service is not available" (section 2.5.1), which is advisory. Treated like no
         // record at all, what comes of that is the connection attempt's to find out.
         //
         found.notes.push_back(std::format("'{}' says the service is not available", found.owner));
         found.aliased = false;
         co_return found;
      }

      found.aliased = true;
      found.owner = normalize_host(alias->target);
      if (!visited.insert(found.owner).second)
         break;
   }

   //
   // Section 3.1: as if the name had no record at all.
   //
   co_return ServiceRecords{
      .owner = host, .notes = {std::format("alias chain of '{}' is too long or has a loop", host)}};
}

anyhttp::Task<Result> Prober::probe(const std::string& host)
{
   Result result{.host = host};

   const auto found = co_await resolve_service(host);
   result.records = found.records;

   const auto plan = plan_routes(host, found);
   result.notes = plan.rejected;
   result.error = "no way to connect";

   for (const auto& route : plan.routes)
   {
      const auto start = std::chrono::steady_clock::now();
      auto response = co_await attempt(route, host);
      if (response.ec)
      {
         result.error = std::format("[{}] {}", route.to_string(), response.error);
         result.notes.push_back(result.error);
         continue;
      }

      result.error.clear();
      result.status = static_cast<int>(response.message.result_int());
      result.route = route.to_string();
      for (const auto& field : response.message)
         result.fields.emplace_back(field.name_string(), field.value());
      result.body_size = response.message.body().size();
      result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
         std::chrono::steady_clock::now() - start);
      break;
   }
   co_return result;
}

//
// Connects as the route says and asks for the host's root. Everything is reported rather than
// thrown: when_either() would otherwise sit out the timeout, as what throws does not count as
// having finished first.
//
anyhttp::Task<Prober::Response> Prober::attempt(const Route& route, const std::string& host)
{
   anyhttp::client::Config client_config;
   client_config.protocol = anyhttp::Protocol::h3;
   client_config.tls_ca_file = config_.tls_ca_file;

   //
   // The name of the route, as that is also what anyhttp verifies the certificate against, which
   // leaves it to its own resolver to look up the address.
   //
   client_config.url.set_scheme("https");
   client_config.url.set_host_address(route.host);
   client_config.url.set_port_number(route.port);

   const auto run = [](anyhttp::Executor executor, anyhttp::client::Config client_config,
                       boost::urls::url url) -> anyhttp::Task<Response> {
      try
      {
         anyhttp::client::Client client(executor, std::move(client_config));
         auto [connect_ec, session] = co_await client.connect();
         if (connect_ec)
            co_return Response{.ec = connect_ec,
                               .error = std::format("connect: {}", connect_ec.message())};

         anyhttp::Fields headers;
         headers.set(http::field::user_agent, user_agent);
         auto [get_ec, message] = co_await session.get(std::move(url), std::move(headers));
         co_return Response{.ec = get_ec,
                            .message = std::move(message),
                            .error = get_ec ? std::format("request: {}", get_ec.message()) : ""};
      }
      catch (const std::exception& ex)
      {
         co_return Response{.ec = make_error_code(boost::system::errc::connection_aborted),
                            .error = ex.what()};
      }
   };

   auto result = co_await anyhttp::when_either(
      run(executor_, std::move(client_config), boost::urls::url(std::format("https://{}/", host))),
      anyhttp::delay(config_.timeout));

   if (auto* response = std::get_if<0>(&result))
      co_return std::move(*response);

   co_return Response{.ec = make_error_code(boost::system::errc::timed_out),
                      .error = std::format("timed out after {}", config_.timeout)};
}

// =================================================================================================

} // namespace http_probe
