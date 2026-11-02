//
// One request to https://<host>/ for each host, in the way that the HTTPS records of the host say.
//
#pragma once

#include "routes.hpp"

#include <anyhttp/client.hpp>

#include <c_ares_async/resolver.hpp>

#include <chrono>
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
   std::string host;
   std::vector<c_ares_async::https_record> records; ///< the records that were considered

   int status = 0; ///< the status of the response, 0 if there was none
   std::string route; ///< what the response came over
   std::vector<std::pair<std::string, std::string>> fields; ///< the header fields of the response
   std::size_t body_size = 0;
   std::chrono::milliseconds duration{}; ///< of the attempt that gave the response

   std::string error; ///< why there is no response
   std::vector<std::string> notes; ///< records that could not be used, routes that did not work
};

/**
 * Requests each of Config::hosts once and in turn, using the resolver of c-ares-async to find out
 * how to connect to it (see plan_routes()). A response is what comes back, whatever its status: a
 * redirect is reported and not followed, nothing in the body is looked at, and nothing else on the
 * host is requested.
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

   anyhttp::Task<ServiceRecords> resolve_service(const std::string& host);
   anyhttp::Task<Result> probe(const std::string& host);
   anyhttp::Task<Response> attempt(const Route& route, const std::string& host);

   anyhttp::Executor executor_;
   Config config_;
   Observer observer_;
   std::vector<std::string> hosts_;
   c_ares_async::resolver resolver_;
};

// =================================================================================================

} // namespace http_probe
