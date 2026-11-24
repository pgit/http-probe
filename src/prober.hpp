//
// One request to https://<host>/ for each host, or to another path on it, in the way that the HTTPS
// records of the host say.
//
#pragma once

#include "routes.hpp"

#include <anyhttp/client.hpp>

#include <c_ares_async/resolver.hpp>

#include <boost/capy/ex/any_executor.hpp>
#include <boost/capy/results.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/endpoint.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <optional>
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

   /// What to ask each host for, in origin form (RFC 9112, section 3.2.1): a path, which starts
   /// with '/', and maybe a query, as in "/robots.txt" or "/search?q=1", percent-encoded where it
   /// has to be. The request to an alternative service asks for the same.
   std::string path = "/";

   /// How many hosts to work on at the same time.
   std::size_t concurrency = 16;

   /// How long the request may take on one route, from connecting to the end of the response,
   /// including any addresses of the host that could not be connected to.
   std::chrono::milliseconds timeout{10000};

   /// The DNS servers to ask, comma separated: what resolver::set_servers() takes, or a host name
   /// with or without a port. Empty for those of the system.
   std::string dns_server;

   /// Whether to ask them over TLS (RFC 7858) or, with quic as well, over QUIC (RFC 9250), and what
   /// to check their certificates against, in the trust store of the TLS library, which tls_ca_file
   /// has no say in. A single host name in dns_server goes without saying, as far as the name in
   /// here goes.
   c_ares_async::tls_options dns_tls;

   /// Who to look up the host names in dns_server with, always in the clear. Empty for the servers
   /// of the system, and only tests have a use for anything else.
   std::string dns_server_lookup;

   /// The CA certificates to verify servers against, as a PEM file. Empty for the system's.
   std::string tls_ca_file;

   /// Where to append the secrets of every TLS handshake, in the NSS key log format that Wireshark
   /// reads (what SSLKEYLOGFILE names for curl and browsers), so that a capture can be decrypted.
   /// Empty for none. anyhttp writes it, and on the Corosio runtime for HTTP/3 only: its TLS over
   /// TCP has no key log. Whoever can read the file can read the traffic, so it is created 0600.
   std::string tls_keylog_file;

   /// Where to go for HTTPS when the records don't say. Only tests have a use for anything else.
   std::uint16_t default_port = 443;

   /// The protocols to speak, and no others, see plan_routes(): a host that offers none of them is
   /// left out. A server that takes HTTP/1.1 where h2 was offered is not sent the request without
   /// h1 among them.
   Protocols protocols;

   /// Stop after the lookups, with the addresses that it would have gone on to connect to, and
   /// connect to nothing. See Result::Outcome::resolved.
   bool dns_only = false;

   /// Send a request to each address of a host, all of them at the same time, rather than to the
   /// first that can be connected to. Config::timeout then is per address on each route. See
   /// Result::replies.
   bool each_address = false;

   /// After a response, send one more request to the alternative service that its Alt-Svc field
   /// names first, of those that plan_alt_svc() takes, at the address that answered. That is one
   /// request more at the most, per address with Config::each_address, and never to another host.
   /// See Result::alt_svc and AddressReply::alt_svc.
   bool alt_svc = false;

   /// Where the messages of this program and of c-ares go. anyhttp can't be told: it always uses
   /// the default logger of spdlog, so a logger other than that one can have a level of its own.
   std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();
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

/// The header fields of a response, in the order they came.
using HeaderFields = std::vector<std::pair<std::string, std::string>>;

/// What came of the request to an alternative service, see Config::alt_svc.
struct AltSvcReply
{
   std::string route; ///< what the response came over, as the server chose it, or what was tried
   std::string address;
   int status = 0; ///< without \c error
   std::string reason; ///< without \c error, see AddressReply::reason
   std::string location; ///< with a 3xx status, see Result::location
   HeaderFields fields; ///< without \c error
   std::string error; ///< what went wrong, if no response could be had
};

/// What came of the request to one address of a host, see Config::each_address.
struct AddressReply
{
   std::string address;
   int status = 0; ///< without \c error
   std::string reason; ///< without \c error: "OK", which HTTP/2 and HTTP/3 only know by the status
   std::string location; ///< with a 3xx status: the Location field as it came, see Result::location
   HeaderFields fields; ///< without \c error
   std::string error; ///< what went wrong on the last route, if no response could be had
   std::optional<AltSvcReply> alt_svc; ///< with Config::alt_svc, if the response named one to ask
};

/// What came of one host.
struct Result
{
   enum class Outcome
   {
      answered, ///< a response arrived, whatever its status, with Config::each_address from all
      failed, ///< it was connected to, and no response could be had, or not from all addresses
      out_of_reach, ///< not connected to, its records say so or lead elsewhere, see plan_routes()
      not_found, ///< not connected to, the name does not exist
      lookup_failed, ///< not connected to, DNS gave no answer, so there is no telling
      left_out, ///< not connected to, it offers none of Config::protocols
      resolved, ///< not connected to, but it could have been tried, see Config::dns_only
   };

   std::string host;
   Outcome outcome = Outcome::failed;
   bool aliased = false; ///< its records are those of another name
   std::vector<c_ares_async::https_record> records; ///< the records that were considered

   int status = 0; ///< with \c answered
   std::string route; ///< with \c answered: what the response came over, as the server chose it
   HeaderFields fields; ///< with \c answered
   /// With \c answered and a 3xx status: the Location field as it came, neither resolved nor
   /// followed, and empty without one.
   std::string location;
   std::size_t body_size = 0; ///< with \c answered
   std::chrono::milliseconds duration{}; ///< with \c answered: of the attempt that got through

   /// The address that answered or, with \c failed, the last one contacted. Empty without one.
   std::string address;
   std::vector<std::string> addresses; ///< with \c resolved: all of them, in the order of trying
   std::string error; ///< with \c failed: what went wrong on the last route
   std::vector<AddressReply> replies; ///< with Config::each_address, instead of the above
   /// With Config::alt_svc and \c answered, but not Config::each_address, if the response named an
   /// alternative service to ask. Whatever came of it, the outcome is that of the first request.
   std::optional<AltSvcReply> alt_svc;
   std::vector<std::string> notes; ///< records that could not be used, routes that did not work
};

/// What a run came to as a whole, counted as it goes. Everything happens on one executor, so these
/// are plain numbers.
struct Stats
{
   std::size_t https_lookups = 0; ///< queries for HTTPS records, those for alias targets included
   std::size_t address_lookups = 0; ///< queries for the addresses of a host

   /// Connections tried, one per address on each route that got a turn, whether a request went out
   /// on it or not.
   std::size_t attempts = 0;
   std::size_t requests = 0; ///< requests that went out, however they ended
   std::size_t responses = 0; ///< responses that came back whole
   std::size_t timeouts = 0; ///< attempts that ran out of time, before or after the request

   /// The responses by the first digit of their status, 2 for 2xx, and by the protocol the server
   /// chose to speak.
   std::map<int, std::size_t> status_classes;
   std::map<anyhttp::Protocol, std::size_t> protocols;

   /// What the responses came to, after decoding: the header as Boost.HTTP puts it into text, which
   /// is HTTP/1.1 whatever the protocol, and the body. TLS, QUIC and the framing of HTTP/2 and
   /// HTTP/3 are not in this, and neither is what came of a request that did not end in a response.
   std::size_t header_bytes = 0;
   std::size_t body_bytes = 0;

   /// How long each response took, from connecting to its end, as in Result::duration.
   std::vector<std::chrono::milliseconds> durations;

   /// From the start of run() to its end, including any lookups of the DNS servers.
   std::chrono::milliseconds elapsed{};
};

/**
 * Looks up the HTTPS records and the addresses of each of Config::hosts with the resolver of
 * c-ares-async, and sends one request to each host that can be connected to (see plan_routes()),
 * trying its addresses one at a time on each route, IPv6 first. A response is what comes back,
 * whatever its status: a redirect is reported and not followed, nothing in the body is looked at,
 * and nothing else on the host is requested. The next route is for a connection that did not come
 * about: once a request has gone out, that was it for this host, however it ended. With
 * Config::dns_only, it is the lookups and nothing else. With Config::each_address, all of this goes
 * for each address of the host instead of for the host: one request per address, to all of them at
 * the same time, with the replies in the order of the addresses. With Config::alt_svc, a response
 * may lead to one request more, to an alternative service at the same address, see
 * plan_alt_svc().
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
   Prober(const capy::any_executor& executor, Config config, Observer observer);

   capy::task<> run();

   /// What has been counted so far, all of the run once run() is done.
   const Stats& stats() const { return stats_; }

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
   capy::task<capy::results<AddressReply>> ask_address(const std::vector<Route>& routes,
                                                       const std::string& host,
                                                       const corosio::endpoint& address,
                                                       std::vector<std::string>& notes);
   capy::task<std::optional<AltSvcReply>> follow_alt_svc(const std::string& host,
                                                         const HeaderFields& fields,
                                                         const std::vector<Route>& tried,
                                                         const corosio::endpoint& address,
                                                         std::vector<std::string>& notes);
   capy::task<capy::results<Response>> attempt(const Route& route, const std::string& host,
                                               const corosio::endpoint& endpoint,
                                               std::chrono::steady_clock::time_point deadline);
   capy::task<capy::results<Response>> exchange(anyhttp::client::Client& client,
                                                anyhttp::Session& session, bool& request_sent,
                                                boost::urls::url url);
   void count_response(const Response& response);

   capy::any_executor executor_;
   Config config_;
   Observer observer_;
   std::vector<std::string> hosts_;
   std::size_t next_host_ = 0;
   Stats stats_;

   /// For the duration of run(), as a connection for DNS over TLS stays open for as long as this is
   /// around, and with that the event loop busy. By pointer, as there is no std::optional of it.
   std::unique_ptr<c_ares_async::resolver> resolver_;
};

// =================================================================================================

} // namespace http_probe
