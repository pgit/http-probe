//
// One request to https://<host>/ for each host, in the way that the HTTPS records of the host say.
//
#pragma once

#include "routes.hpp"

#include <anyhttp/client.hpp>

#include <c_ares_async/resolver.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <istream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http_probe
{

// =================================================================================================

struct Config
{
   /// The host names to request, exactly as given: these are the only names that are connected to.
   std::vector<std::string> hosts;

   /// How many hosts to work on at the same time.
   std::size_t concurrency = 16;

   /// How long the request may take on one route, from connecting to the end of the response.
   std::chrono::milliseconds timeout{10000};

   /// The DNS servers to ask, as resolver::set_servers() takes them. Empty for those of the system.
   std::string dns_server;

   /// The CA certificates to verify servers against, as a PEM file. Empty for the system's.
   std::string tls_ca_file;
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

/// What came of one host.
struct Result
{
   enum class Outcome
   {
      answered, ///< it has HTTP/3, and a response arrived, whatever its status
      failed, ///< it has HTTP/3, and no response could be had
      out_of_reach, ///< it has HTTP/3, but on a target that can't be used, see plan_routes()
      no_http3, ///< its records offer no HTTP/3, or it has none, or the name does not exist
      lookup_failed, ///< DNS gave no answer, so there is no telling
   };

   std::string host;
   Outcome outcome = Outcome::no_http3;
   bool aliased = false; ///< its records are those of another name
   std::vector<c_ares_async::https_record> records; ///< the records that were considered

   int status = 0; ///< with \c answered
   std::string route; ///< with \c answered: what the response came over
   std::vector<std::pair<std::string, std::string>> fields; ///< with \c answered
   std::size_t body_size = 0; ///< with \c answered
   std::chrono::milliseconds duration{}; ///< with \c answered: of the attempt that got through

   std::string error; ///< with \c failed: what went wrong on the last route
   std::vector<std::string> notes; ///< records that could not be used, routes that did not work
};

/**
 * Looks up the HTTPS records of each of Config::hosts with the resolver of c-ares-async, and for
 * those that offer HTTP/3, sends one request (see plan_routes()). A response is what comes back,
 * whatever its status: a redirect is reported and not followed, nothing in the body is looked at,
 * and nothing else on the host is requested.
 *
 * Hosts are worked on Config::concurrency at a time, so results arrive as they are ready. All of
 * this is on the one executor, which is also what calls the observer.
 */
class Prober
{
public:
   using Observer = std::function<void(const Result&)>;

   /// Throws \c std::invalid_argument for a configuration that is not good to go, and does nothing
   /// else until run().
   Prober(anyhttp::Executor executor, Config config, Observer observer);

   anyhttp::Task<void> run();

private:
   struct Response
   {
      anyhttp::error_code ec;
      anyhttp::client::Message message;
      std::string error;
   };

   anyhttp::Task<void> work();
   anyhttp::Task<ServiceRecords> resolve_service(const std::string& host);
   anyhttp::Task<Result> probe(const std::string& host);
   anyhttp::Task<Response> attempt(const Route& route, const std::string& host);

   anyhttp::Executor executor_;
   Config config_;
   Observer observer_;
   std::vector<std::string> hosts_;
   std::size_t next_host_ = 0;
   c_ares_async::resolver resolver_;
};

// =================================================================================================

} // namespace http_probe
