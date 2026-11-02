#include "crawler.hpp"

#include "links.hpp"

#include <anyhttp/session.hpp>

#include <boost/beast/http/field.hpp>
#include <boost/url/parse.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

namespace http = boost::beast::http;

namespace crawler
{

namespace
{

// =================================================================================================

constexpr std::string_view agent_token = "crawler"; // what a robots.txt addresses us by
constexpr std::string_view user_agent = "crawler/0.0.1 (+https://github.com/pgit/crawler)";

constexpr int max_alias_chain = 8;
constexpr int max_robots_redirects = 5; // RFC 9309, section 2.3.1.2
constexpr std::size_t max_frontier = 100'000;

bool is_html(std::string_view content_type)
{
   std::string type(content_type.substr(0, content_type.find(';')));
   std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
   return type.empty() || type.starts_with("text/html") ||
          type.starts_with("application/xhtml+xml");
}

bool is_redirect(unsigned status)
{
   return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// =================================================================================================

} // namespace

Crawler::Crawler(anyhttp::Executor executor, Config config, Observer observer)
   : executor_(executor), config_(std::move(config)), observer_(std::move(observer)),
     resolver_(std::move(executor))
{
   if (config_.allowed_hosts.empty())
      throw std::invalid_argument("refusing to crawl without an allowlist of hosts");

   for (const auto& host : config_.allowed_hosts)
   {
      if (host.empty() || host.find('*') != host.npos)
         throw std::invalid_argument(
            std::format("'{}' is not a host: the allowlist takes exact names", host));
      allowed_hosts_.insert(normalize_host(host));
   }

   if (config_.seeds.empty())
      throw std::invalid_argument("nothing to crawl: no URLs given");

   if (!config_.dns_server.empty())
      if (const auto ec = resolver_.set_servers(config_.dns_server))
         throw std::invalid_argument(
            std::format("invalid DNS server '{}': {}", config_.dns_server, ec.message()));

   for (const auto& seed : config_.seeds)
   {
      const auto parsed = boost::urls::parse_uri(seed);
      const auto url = parsed ? resolve_link(*parsed, seed) : std::nullopt;
      if (!url)
         throw std::invalid_argument(std::format("'{}' is not an http or https URL", seed));
      if (!is_allowed(url->host_address()))
         throw std::invalid_argument(
            std::format("'{}' is not on an allowed host: '{}'", seed, url->host_address()));
      if (seen_.insert(std::string(url->buffer())).second)
         frontier_.push_back(*url);
   }
}

// -------------------------------------------------------------------------------------------------

bool Crawler::is_allowed(std::string_view host) const
{
   return allowed_hosts_.contains(normalize_host(host));
}

void Crawler::emit(Event event) const
{
   if (observer_)
      observer_(event);
}

void Crawler::enqueue(boost::urls::url url)
{
   if (!seen_.insert(std::string(url.buffer())).second)
      return;

   if (!is_allowed(url.host_address()))
      emit({.kind = Event::Kind::outside_allowlist, .url = std::string(url.buffer())});
   else if (frontier_.size() < max_frontier)
      frontier_.push_back(std::move(url));
}

// -------------------------------------------------------------------------------------------------

//
// SVCB resolution, section 3 of RFC 9460, up to the point where it would take addresses. A lookup
// that fails is not fatal: DNS is not protected here, and this is the choice section 3.1 leaves
// for that. A name without a record is the common case and not even worth a note.
//
anyhttp::Task<ServiceRecords> Crawler::resolve_service(const Origin& origin)
{
   ServiceRecords found{.owner = https_query_name(origin)};
   std::set<std::string> visited{found.owner};

   for (int hop = 0; hop < max_alias_chain; ++hop)
   {
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
   co_return ServiceRecords{.owner = https_query_name(origin),
                            .notes = {std::format("alias chain of '{}' is too long or has a loop",
                                                  https_query_name(origin))}};
}

anyhttp::Task<Plan*> Crawler::plan_for(const Origin& origin)
{
   const auto key = origin.key();
   if (auto it = plans_.find(key); it != plans_.end())
      co_return &it->second;

   auto found =
      origin.is_address ? ServiceRecords{.owner = origin.host} : co_await resolve_service(origin);

   auto plan = plan_routes(origin, found);
   for (const auto& route : plan.routes)
      emit({.kind = Event::Kind::note,
            .url = key,
            .route = route.to_string(),
            .detail = route.source});
   for (const auto& rejected : plan.rejected)
      emit({.kind = Event::Kind::note, .url = key, .detail = rejected});

   co_return &plans_.emplace(key, std::move(plan)).first->second;
}

//
// RFC 9309, section 2.3.1: anything in the 400s means there are no rules, anything else that is not
// a robots.txt means that we can not know them, and then nothing is allowed. That goes for a
// redirect to another origin as well, which the RFC would have us follow. It is not worth the
// complication to do that while we can not connect to most of them.
//
anyhttp::Task<const Robots*> Crawler::robots_for(Plan& plan)
{
   const auto key = plan.origin.key();
   if (auto it = robots_.find(key); it != robots_.end())
      co_return &it->second;

   auto url = boost::urls::url(std::format("{}/robots.txt", key));
   Robots robots = Robots::disallow_all();
   std::string problem;

   for (int redirects = 0;; ++redirects)
   {
      const auto response = co_await fetch(plan, url);
      if (response.ec)
      {
         problem = response.error;
         break;
      }

      const auto status = response.message.result_int();
      if (status >= 200 && status < 300)
         robots = Robots::parse(response.message.body(), agent_token);
      else if (status >= 400 && status < 500)
         robots = Robots{};
      else if (is_redirect(status) && redirects < max_robots_redirects)
      {
         const auto target = resolve_link(url, response.message[http::field::location]);
         if (target && origin_of(*target) && origin_of(*target)->key() == key)
         {
            url = *target;
            continue;
         }
         problem = "redirected to another origin";
      }
      else
         problem = std::format("status {}", status);
      break;
   }

   if (!problem.empty())
      emit({.kind = Event::Kind::note,
            .url = key,
            .detail =
               std::format("robots.txt could not be read, so nothing is allowed: {}", problem)});
   else if (const auto crawl_delay = robots.crawl_delay())
   {
      auto& pacing = pacing_[plan.origin.host];
      pacing.delay = std::max(pacing.delay, *crawl_delay);
   }

   co_return &robots_.emplace(key, std::move(robots)).first->second;
}

// -------------------------------------------------------------------------------------------------

//
// One request, whichever route gets it through. This is the only place that sends requests, so it
// is the one to see to the spacing, and to the allowlist one last time.
//
anyhttp::Task<Crawler::Response> Crawler::fetch(Plan& plan, boost::urls::url url)
{
   if (!is_allowed(plan.origin.host))
      throw std::logic_error(std::format("about to contact '{}'", plan.origin.host));

   Response response;
   response.ec = make_error_code(boost::system::errc::host_unreachable);
   response.error = "no way to connect";

   auto& pacing = pacing_[plan.origin.host];
   const auto wait_for = [&] { return pacing.next_request - std::chrono::steady_clock::now(); };
   if (wait_for().count() > 0)
      co_await anyhttp::delay(wait_for());

   for (auto route = plan.routes.begin(); route != plan.routes.end(); ++route)
   {
      response = co_await attempt(*route, url);
      if (response.ec)
      {
         emit({.kind = Event::Kind::note,
               .url = std::string(url.buffer()),
               .route = route->to_string(),
               .detail = response.error});
         continue;
      }

      std::rotate(plan.routes.begin(), route, std::next(route)); // go straight there next time
      break;
   }

   pacing.next_request = std::chrono::steady_clock::now() + std::max(config_.delay, pacing.delay);
   co_return response;
}

anyhttp::Task<Crawler::Response> Crawler::attempt(const Route& route, boost::urls::url url)
{
   anyhttp::client::Config client_config;
   client_config.protocol = route.protocol;
   client_config.tls_ca_file = config_.tls_ca_file;

   const auto with_route = [&](Response response) {
      response.route = route.to_string();
      return response;
   };

   //
   // HTTP/3 goes to the name of the route, as that is also what anyhttp verifies the certificate
   // against, which leaves it to its own resolver to look up the address.
   //
   if (route.protocol == anyhttp::Protocol::h3)
   {
      client_config.url.set_scheme("https");
      client_config.url.set_host_address(route.host);
      client_config.url.set_port_number(route.port);
      co_return with_route(co_await exchange(std::move(client_config), std::move(url)));
   }

   //
   // In the clear, nothing has to be verified, so the addresses can be ours. The lookup stays
   // outside of the timeout: the image has c-ares 1.34.5, and cancelling a lookup is what
   // GHSA-6wfj-rwm7-3542 is about. c-ares has timeouts of its own.
   //
   auto [ec, endpoints] = co_await resolver_.resolve(route.host, std::to_string(route.port));
   if (ec)
      co_return with_route(
         Response{.ec = ec, .error = std::format("{}: {}", route.host, ec.message())});

   Response response{.ec = make_error_code(boost::system::errc::host_unreachable),
                     .error = std::format("{}: no addresses", route.host)};
   for (const auto& endpoint : endpoints)
   {
      client_config.url.set_scheme("http");
      client_config.url.set_host_address(endpoint.address().to_string());
      client_config.url.set_port_number(endpoint.port());
      response = co_await exchange(client_config, url);
      if (!response.ec)
         break;
   }
   co_return with_route(std::move(response));
}

//
// Connects as told and asks for \p url, whose host is what goes into the "Host" field, whatever
// \p client_config connects to. Everything is reported rather than thrown: when_either() would
// otherwise sit out the timeout, as what throws does not count as having finished first.
//
anyhttp::Task<Crawler::Response> Crawler::exchange(anyhttp::client::Config client_config,
                                                   boost::urls::url url)
{
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
      run(executor_, std::move(client_config), std::move(url)), anyhttp::delay(config_.timeout));

   if (auto* response = std::get_if<0>(&result))
      co_return std::move(*response);

   co_return Response{.ec = make_error_code(boost::system::errc::timed_out),
                      .error = std::format("timed out after {}", config_.timeout)};
}

// -------------------------------------------------------------------------------------------------

//
// What a response means for the rest of the crawl: more URLs, if it has any.
//
void Crawler::visit(const boost::urls::url& url, const Response& response)
{
   const auto status = response.message.result_int();
   std::size_t links = 0;

   if (is_redirect(status))
   {
      if (const auto target = resolve_link(url, response.message[http::field::location]))
         enqueue(*target);
   }
   else if (status >= 200 && status < 300 && is_html(response.message[http::field::content_type]))
   {
      for (const auto& href : extract_links(response.message.body()))
         if (auto target = resolve_link(url, href))
         {
            enqueue(std::move(*target));
            ++links;
         }
   }

   emit({.kind = Event::Kind::fetched,
         .url = std::string(url.buffer()),
         .status = static_cast<int>(status),
         .route = response.route,
         .detail = std::format("{} bytes, {} links", response.message.body().size(), links)});
}

anyhttp::Task<void> Crawler::run()
{
   while (!frontier_.empty() && requested_ < config_.max_pages)
   {
      auto url = std::move(frontier_.front());
      frontier_.pop_front();

      auto* plan = co_await plan_for(*origin_of(url));

      //
      // The records can turn "http" into "https" (RFC 9460, section 9.5), and that may well be a
      // page we have already been to, or are going to.
      //
      if (url.scheme() != plan->origin.scheme)
      {
         url.set_scheme(plan->origin.scheme);
         if (url.has_port())
            url.set_port_number(plan->origin.port);
         if (!seen_.insert(std::string(url.buffer())).second)
            continue;
      }

      const auto text = std::string(url.buffer());
      if (plan->routes.empty())
      {
         emit({.kind = Event::Kind::failed, .url = text, .detail = "no way to connect"});
         continue;
      }

      const auto* robots = co_await robots_for(*plan);
      if (!robots->allowed(url.encoded_target()))
      {
         emit({.kind = Event::Kind::disallowed, .url = text});
         continue;
      }

      ++requested_;
      const auto response = co_await fetch(*plan, url);
      if (response.ec)
         emit({.kind = Event::Kind::failed, .url = text, .detail = response.error});
      else
         visit(url, response);
   }
}

// =================================================================================================

} // namespace crawler
