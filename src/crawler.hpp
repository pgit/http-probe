//
// The crawl: one URL at a time, breadth first, from the seeds and within the allowlist.
//
#pragma once

#include "robots.hpp"
#include "routes.hpp"

#include <anyhttp/client.hpp>

#include <c_ares_async/resolver.hpp>

#include <boost/url/url.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace crawler
{

// =================================================================================================

struct Config
{
   /// The hosts that may be connected to, exactly: no wildcards, and no subdomains implied.
   std::vector<std::string> allowed_hosts;

   /// Where to start. Each has to be on an allowed host.
   std::vector<std::string> seeds;

   /// How many pages to request at most, whatever comes of them. robots.txt does not count.
   std::size_t max_pages = 100;

   /// The least time from the end of one request to the start of the next one, per host. A longer
   /// "Crawl-delay" in the host's robots.txt takes precedence.
   std::chrono::milliseconds delay{1000};

   /// How long one request may take on one route, from connecting to the end of the response.
   std::chrono::milliseconds timeout{10000};

   /// The DNS servers to ask, as resolver::set_servers() takes them. Empty for those of the system.
   std::string dns_server;

   /// The CA certificates to verify servers against, as a PEM file. Empty for the system's.
   std::string tls_ca_file;
};

/// Something that happened in the course of a crawl, for whoever is watching.
struct Event
{
   enum class Kind
   {
      fetched, ///< a response arrived, whatever its status
      failed, ///< no response could be had
      disallowed, ///< robots.txt says no, or it could not be read
      outside_allowlist, ///< a link or a redirect to a host that is not allowed
      note, ///< something worth knowing about how a host is reached
   };

   Kind kind = Kind::note;
   std::string url;
   int status = 0; ///< with \c fetched
   std::string route; ///< with \c fetched, and \c note where about a route
   std::string detail;
};

/**
 * Crawls, using the resolver of c-ares-async to find out how to connect (see plan_routes()).
 *
 * The only thing that can make this reach a host is that host being in Config::allowed_hosts:
 * every seed, link and redirect is checked against it. Requests per host are spaced by
 * Config::delay and the host's robots.txt is honoured -- a robots.txt that can not be read
 * stops the crawl of its origin.
 *
 * There is one request at a time and a new connection for each of them.
 */
class Crawler
{
public:
   using Observer = std::function<void(const Event&)>;

   /// Throws \c std::invalid_argument for a configuration that is not good to go, starting with
   /// one that has no allowlist, and does nothing else until run().
   Crawler(anyhttp::Executor executor, Config config, Observer observer = {});

   /// Crawls until there is nothing left to do or Config::max_pages is reached.
   anyhttp::Task<void> run();

private:
   struct Response
   {
      anyhttp::error_code ec;
      anyhttp::client::Message message;
      std::string route;
      std::string error; // what went wrong, with ec or from an exception, on the last route
   };

   struct Pacing
   {
      std::chrono::steady_clock::time_point next_request{};
      std::chrono::milliseconds delay{};
   };

   bool is_allowed(std::string_view host) const;
   void emit(Event event) const;
   void enqueue(boost::urls::url url);

   anyhttp::Task<ServiceRecords> resolve_service(const Origin& origin);
   anyhttp::Task<Plan*> plan_for(const Origin& origin);
   anyhttp::Task<const Robots*> robots_for(Plan& plan);

   anyhttp::Task<Response> fetch(Plan& plan, boost::urls::url url);
   anyhttp::Task<Response> attempt(const Route& route, boost::urls::url url);
   anyhttp::Task<Response> exchange(anyhttp::client::Config client_config, boost::urls::url url);

   void visit(const boost::urls::url& url, const Response& response);

   anyhttp::Executor executor_;
   Config config_;
   Observer observer_;
   std::set<std::string> allowed_hosts_;
   c_ares_async::resolver resolver_;

   std::deque<boost::urls::url> frontier_;
   std::set<std::string> seen_;
   std::size_t requested_ = 0;

   std::map<std::string, Plan> plans_; // by Origin::key()
   std::map<std::string, Robots> robots_; // by Origin::key(), of the origin in the plan
   std::map<std::string, Pacing> pacing_; // by host
};

// =================================================================================================

} // namespace crawler
