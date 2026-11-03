#include "prober.hpp"

#include <anyhttp/session.hpp>

#include <boost/beast/http/field.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <set>
#include <stdexcept>
#include <tuple>
#include <variant>

namespace http = boost::beast::http;

namespace http_probe
{

namespace
{

// =================================================================================================

constexpr std::string_view user_agent = "http-probe/0.0.1 (+https://github.com/pgit/http-probe)";

constexpr int max_alias_chain = 8;

//
// The resolver does not write anywhere by itself. Its messages go where those of anyhttp go, which
// is the default logger of spdlog.
//
void log_to_spdlog(c_ares_async::log_level level, std::string_view message)
{
   using enum c_ares_async::log_level;
   switch (level)
   {
   case debug:
      return spdlog::debug("{}", message);
   case info:
      return spdlog::info("{}", message);
   case warning:
      return spdlog::warn("{}", message);
   case error:
      return spdlog::error("{}", message);
   }
}

// =================================================================================================

} // namespace

bool is_host_name(std::string_view host)
{
   const auto is_name_char = [](unsigned char c) {
      return std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_';
   };
   const auto [not_an_address, address] = corosio::make_ip_address(host);
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

void prefer_ipv6(std::vector<corosio::endpoint>& addresses)
{
   std::ranges::stable_partition(addresses, &corosio::endpoint::is_v6);
}

Prober::Prober(capy::any_executor executor, Config config, Observer observer)
   : executor_(executor), config_(std::move(config)), observer_(std::move(observer)),
     resolver_(std::move(executor))
{
   if (config_.hosts.empty())
      throw std::invalid_argument("nothing to do: no host names given");
   if (config_.concurrency == 0)
      throw std::invalid_argument("a concurrency of 0 would never get anywhere");

   std::set<std::string> seen;
   for (const auto& host : config_.hosts)
   {
      if (!is_host_name(host))
         throw std::invalid_argument(std::format("'{}' is not a host name", host));
      if (auto name = normalize_host(host); seen.insert(name).second)
         hosts_.push_back(std::move(name));
   }

   resolver_.set_logger(&log_to_spdlog);

   if (!config_.dns_server.empty())
      if (const auto ec = resolver_.set_servers(config_.dns_server))
         throw std::invalid_argument(
            std::format("invalid DNS server '{}': {}", config_.dns_server, ec.message()));
}

capy::task<> Prober::run()
{
   std::vector<capy::task<capy::results<>>> workers;
   for (std::size_t i = 0; i < std::min(config_.concurrency, hosts_.size()); ++i)
      workers.push_back(work());

   std::ignore = co_await capy::when_all(std::move(workers));
}

capy::task<capy::results<>> Prober::work()
{
   while (next_host_ < hosts_.size())
   {
      const auto& host = hosts_[next_host_++];
      observer_(co_await probe(host));
   }
   co_return capy::results<>{};
}

// -------------------------------------------------------------------------------------------------

//
// SVCB resolution, section 3 of RFC 9460, up to the point where it would take addresses. A lookup
// that fails is not fatal: DNS is not protected here, and this is the choice section 3.1 leaves
// for that. A name without a record is the common case and not even worth a note.
//
capy::task<ServiceRecords> Prober::resolve_service(const std::string& host)
{
   ServiceRecords found{.owner = host};
   std::set<std::string> visited{found.owner};

   for (int hop = 0; hop < max_alias_chain; ++hop)
   {
      //
      // Outside of any timeout: the image has c-ares 1.34.5, and cancelling a lookup is what
      // GHSA-6wfj-rwm7-3542 is about. c-ares has timeouts of its own.
      //
      spdlog::debug("{}: asking for the HTTPS records of '{}'", host, found.owner);
      auto [ec, records] = co_await resolver_.resolve_https(found.owner);
      if (ec)
      {
         if (ec == c_ares_async::make_ares_error(ARES_ENOTFOUND))
            found.not_found = !found.aliased; // the target of an alias is another matter
         else if (ec != c_ares_async::make_ares_error(ARES_ENODATA))
         {
            found.lookup_failed = true;
            found.notes.push_back(
               std::format("HTTPS lookup of '{}': {}", found.owner, ec.message()));
         }
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
         // "The service is not available" (section 2.5.1). This is advisory, but one who does not
         // have to request anything can as well take the host's word for it.
         //
         found.notes.push_back(std::format("'{}' says the service is not available", found.owner));
         found.unavailable = true;
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

capy::task<Result> Prober::probe(const std::string& host)
{
   Result result{.host = host};

   const auto found = co_await resolve_service(host);
   result.aliased = found.aliased;
   result.records = found.records;

   const auto plan = plan_routes(host, found, config_.default_port);
   result.notes = plan.rejected;
   for (const auto& rejected : plan.rejected)
      spdlog::debug("{}: rejected: {}", host, rejected);

   if (plan.routes.empty())
   {
      result.outcome = found.lookup_failed ? Result::Outcome::lookup_failed
                       : found.not_found   ? Result::Outcome::not_found
                                           : Result::Outcome::out_of_reach;
      co_return result;
   }

   //
   // Routes stay on the host's own name, see plan_routes(), so that this is good for all of them.
   // Outside of any timeout, as with the records. Not having an address says nothing about the
   // host, and no record says that it has to have one.
   //
   spdlog::debug("{}: asking for the addresses of '{}'", host, host);
   auto [lookup_ec, addresses] = co_await resolver_.resolve(host, "");
   if (lookup_ec || addresses.empty())
   {
      result.outcome = Result::Outcome::lookup_failed;
      result.notes.push_back(std::format("address lookup of '{}': {}", host,
                                         lookup_ec ? lookup_ec.message() : "no addresses"));
      co_return result;
   }

   //
   // One after the other, and not at the same time as with Happy Eyeballs. c-ares goes by RFC 6724,
   // which puts IPv4 first wherever the source address is not a global one.
   //
   prefer_ipv6(addresses);

   result.outcome = Result::Outcome::failed;
   for (const auto& route : plan.routes)
   {
      const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
      for (const auto& address : addresses)
      {
         const corosio::endpoint endpoint(address, route.port);
         result.address = endpoint.address().to_string();
         spdlog::debug("{}: connecting to {} by {}, for {}", host, result.address,
                       route.to_string(), route.source);
         auto [ec, response] = co_await attempt(route, host, endpoint, deadline);
         if (ec)
         {
            result.error = std::format("[{}] {}", route.to_string(), response.error);
            result.notes.push_back(std::format("{} at {}", result.error, result.address));
            spdlog::debug("{}: {} at {}", host, result.error, result.address);
            if (response.request_sent)
               co_return result; // the host may have seen it, and anything else would make it two
            if (std::chrono::steady_clock::now() >= deadline)
               break; // the next address gets what is left of the timeout, which is nothing
            continue;
         }

         result.outcome = Result::Outcome::answered;
         result.error.clear();
         result.status = static_cast<int>(response.message.result_int());
         auto spoken = route;
         spoken.protocol = response.protocol;
         result.route = spoken.to_string();
         for (const auto& field : response.message)
            result.fields.emplace_back(field.name_string(), field.value());
         result.body_size = response.message.body().size();
         result.duration = response.duration;
         co_return result;
      }
   }
   co_return result;
}

//
// Connects as the route says, to this one address so that there is no doubt about which it was, and
// asks for the host's root, by the deadline. After that it ends the connection in an orderly way,
// which has limits of its own and is none of the timeout's business: the response is there by then.
//
capy::task<capy::results<Prober::Response>>
Prober::attempt(const Route& route, const std::string& host, const corosio::endpoint& endpoint,
                std::chrono::steady_clock::time_point deadline)
{
   anyhttp::client::Config client_config;
   client_config.protocol = route.protocol;
   client_config.tls_ca_file = config_.tls_ca_file;

   //
   // The name of the route is what anyhttp verifies the certificate against, wherever the
   // connection goes. The scheme is what makes it TLS over TCP, too.
   //
   client_config.endpoints = {endpoint};
   client_config.url.set_scheme("https");
   client_config.url.set_host_address(route.host);
   client_config.url.set_port_number(route.port);

   const auto start = std::chrono::steady_clock::now();
   anyhttp::client::Client client(executor_, std::move(client_config));
   anyhttp::Session session;
   bool request_sent = false;

   //
   // These stay around for as long as exchange() needs them: timeout() waits for what it has
   // cancelled to finish.
   //
   auto [ec, response] = co_await corosio::timeout(
      exchange(client, session, request_sent, boost::urls::url(std::format("https://{}/", host))),
      deadline);

   response.request_sent = request_sent;
   if (ec == capy::cond::timeout)
   {
      response.error = std::format("timed out after {}", config_.timeout);
      co_return {ec, std::move(response)};
   }

   response.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

   if (!ec)
      if (auto [shutdown_ec] = co_await session.shutdown(); shutdown_ec)
         spdlog::debug("{}: shutdown: {}", host, shutdown_ec.message());

   co_return {ec, std::move(response)};
}

//
// Everything is reported rather than thrown: one host that makes anyhttp throw does not get to end
// the run for the others.
//
capy::task<capy::results<Prober::Response>> Prober::exchange(anyhttp::client::Client& client,
                                                             anyhttp::Session& session,
                                                             bool& request_sent,
                                                             boost::urls::url url)
{
   try
   {
      auto [connect_ec, connected] = co_await client.connect();
      if (connect_ec)
         co_return {connect_ec,
                    Response{.error = std::format("connect: {}", connect_ec.message())}};

      session = std::move(connected);

      anyhttp::Fields headers;
      headers.set(http::field::user_agent, user_agent);
      request_sent = true;
      auto [get_ec, message] = co_await session.get(std::move(url), std::move(headers));
      co_return {get_ec,
                 Response{.message = std::move(message),
                          .error = get_ec ? std::format("request: {}", get_ec.message()) : "",
                          .protocol = session.protocol()}};
   }
   catch (const std::exception& ex)
   {
      co_return {std::make_error_code(std::errc::connection_aborted), Response{.error = ex.what()}};
   }
}

// =================================================================================================

} // namespace http_probe
