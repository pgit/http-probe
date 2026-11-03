//
// One request to https://<host>/ for each host, in the way that the HTTPS records of the host say.
//
#pragma once

#include "routes.hpp"

#include <anyhttp/client.hpp>

#include <c_ares_async/resolver.hpp>

#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/results.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/endpoint.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http_probe
{

namespace capy = boost::capy;
namespace corosio = boost::corosio;

// =================================================================================================

struct Config
{
   /// The host names to request, exactly as given: these are the only names that are connected to.
   std::vector<std::string> hosts;

   /// How many hosts to work on at the same time.
   std::size_t concurrency = 16;

   /// How long the request may take on one route, from connecting to the end of the response,
   /// including any addresses of the host that could not be connected to.
   std::chrono::milliseconds timeout{10000};

   /// The DNS servers to ask, comma separated: what resolver::set_servers() takes, or a host name
   /// with or without a port. Empty for those of the system.
   std::string dns_server;

   /// Whether to ask them over TLS (RFC 7858), and what to check their certificates against, in the
   /// trust store of the TLS library, which tls_ca_file has no say in. A single host name in
   /// dns_server goes without saying, as far as the name in here goes.
   c_ares_async::tls_options dns_tls;

   /// Who to look up the host names in dns_server with, always in the clear. Empty for the servers
   /// of the system, and only tests have a use for anything else.
   std::string dns_server_lookup;

   /// The CA certificates to verify servers against, as a PEM file. Empty for the system's.
   std::string tls_ca_file;

   /// Where to go for HTTPS when the records don't say. Only tests have a use for anything else.
   std::uint16_t default_port = 443;

   /// Connect only to the hosts with a record that offers HTTP/3, see has_h3_record(). Those are
   /// treated as usual, which includes going over TCP if HTTP/3 can not be connected to.
   bool h3_only = false;
};

/// Whether \p host is a name and nothing but a name: no URL, port, path or wildcard, and not an
/// address either, which has no records to look up.
bool is_host_name(std::string_view host);

/**
 * The hosts in a list, one per line, each as a name or as a URL to take the host from. Empty lines
 * and those starting with '#' are left out, and what is neither of the two goes to \p skipped, so
 * that one odd line does not get in the way of the others.
 */
std::vector<std::string> read_hosts(std::istream& list, std::vector<std::string>& skipped);

/// Moves the IPv6 addresses to the front, each family keeping the order it was in.
void prefer_ipv6(std::vector<corosio::endpoint>& addresses);

/// What came of one host.
struct Result
{
   enum class Outcome
   {
      answered, ///< a response arrived, whatever its status
      failed, ///< it was connected to, and no response could be had
      out_of_reach, ///< not connected to, its records say so or lead elsewhere, see plan_routes()
      not_found, ///< not connected to, the name does not exist
      lookup_failed, ///< not connected to, DNS gave no answer, so there is no telling
      no_h3, ///< not connected to, it has no record that offers HTTP/3, see Config::h3_only
   };

   std::string host;
   Outcome outcome = Outcome::failed;
   bool aliased = false; ///< its records are those of another name
   std::vector<c_ares_async::https_record> records; ///< the records that were considered

   int status = 0; ///< with \c answered
   std::string route; ///< with \c answered: what the response came over, as the server chose it
   std::vector<std::pair<std::string, std::string>> fields; ///< with \c answered
   std::size_t body_size = 0; ///< with \c answered
   std::chrono::milliseconds duration{}; ///< with \c answered: of the attempt that got through

   /// The address that answered or, with \c failed, the last one contacted. Empty without one.
   std::string address;
   std::string error; ///< with \c failed: what went wrong on the last route
   std::vector<std::string> notes; ///< records that could not be used, routes that did not work
};

/**
 * Looks up the HTTPS records and the addresses of each of Config::hosts with the resolver of
 * c-ares-async, and sends one request to each host that can be connected to (see plan_routes()),
 * trying its addresses one at a time on each route, IPv6 first. A response is what comes back,
 * whatever its status: a redirect is reported and not followed, nothing in the body is looked at,
 * and nothing else on the host is requested. The next route is for a connection that did not come
 * about: once a request has gone out, that was it for this host, however it ended.
 *
 * Hosts are worked on Config::concurrency at a time, so results arrive as they are ready. All of
 * this is on the one executor, which is also what calls the observer.
 */
class Prober
{
public:
   using Observer = std::function<void(const Result&)>;

   /// Throws \c std::invalid_argument for a configuration that is not good to go, and does nothing
   /// else until run(), which has more to say about the DNS servers: the same for a list that can't
   /// be used, and \c std::runtime_error for a host name in it that can't be looked up.
   Prober(capy::any_executor executor, Config config, Observer observer);

   capy::task<> run();

private:
   struct Response
   {
      anyhttp::client::Message message;
      std::string error;
      anyhttp::Protocol protocol = anyhttp::Protocol::h3; ///< what the server chose to speak
      bool request_sent = false;
      std::chrono::milliseconds duration{};
   };

   capy::task<std::string> dns_server_addresses();
   capy::task<capy::results<>> work();
   capy::task<ServiceRecords> resolve_service(const std::string& host);
   capy::task<Result> probe(const std::string& host);
   capy::task<capy::results<Response>> attempt(const Route& route, const std::string& host,
                                               const corosio::endpoint& endpoint,
                                               std::chrono::steady_clock::time_point deadline);
   capy::task<capy::results<Response>> exchange(anyhttp::client::Client& client,
                                                anyhttp::Session& session, bool& request_sent,
                                                boost::urls::url url);

   capy::any_executor executor_;
   Config config_;
   Observer observer_;
   std::vector<std::string> hosts_;
   std::size_t next_host_ = 0;

   /// For the duration of run(), as a connection for DNS over TLS stays open for as long as this is
   /// around, and with that the event loop busy. By pointer, as there is no std::optional of it.
   std::unique_ptr<c_ares_async::resolver> resolver_;
};

// =================================================================================================

} // namespace http_probe
