//
// From the HTTPS records of a name to the ways of connecting to an origin (RFC 9460). No I/O in
// here: crawler.cpp asks the questions, this says what the answers mean.
//
#pragma once

#include <anyhttp/common.hpp>

#include <c_ares_async/resolver.hpp>

#include <boost/url/url_view.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crawler
{

// =================================================================================================

/// Lower case and without the dot at the end, which is how host names are compared.
std::string normalize_host(std::string_view host);

struct Origin
{
   std::string scheme; ///< "http" or "https"
   std::string host; ///< as normalize_host() makes it
   std::uint16_t port = 0; ///< never empty: the default of the scheme, if the URL has none
   bool is_address = false; ///< the host is an IP address, which has no DNS records to look up

   std::string key() const; ///< "scheme://host:port"
};

/// The origin of an "http" or "https" URL that has a host, and nothing for any other.
std::optional<Origin> origin_of(boost::urls::url_view url);

/// The name to ask for an origin's HTTPS records: of the origin in its "https" form (section 9.5),
/// with a port prefix unless that is on 443 (section 9.1), as in "_8443._https.example.com".
std::string https_query_name(const Origin& origin);

// -------------------------------------------------------------------------------------------------

/// What SVCB resolution (section 3) found for one origin.
struct ServiceRecords
{
   std::string owner; ///< the name the records are of, at the end of any alias chain
   bool aliased = false; ///< at least one AliasMode record was followed
   std::vector<c_ares_async::https_record> records; ///< the ServiceMode records of \c owner
   std::vector<std::string> notes; ///< what went wrong on the way, none of which is fatal
};

/// One way of connecting. The TLS name and the "Host" stay those of the origin, whatever this says.
struct Route
{
   anyhttp::Protocol protocol = anyhttp::Protocol::h1;
   std::string host; ///< the name to connect to
   std::uint16_t port = 0;
   std::string source; ///< what led to this route, for people to read

   std::string to_string() const; ///< "h3 example.com:443", or "h1 example.com:80"
};

/// How to reach an origin.
struct Plan
{
   Origin origin; ///< the one to send requests for: "https" where the records say so (section 9.5)
   std::vector<Route> routes; ///< in the order to try them; empty if there is no usable way
   std::vector<std::string> rejected; ///< what the records offered, or we knew of, and why not
};

/**
 * Says how to connect to \p origin, given what was found for it.
 *
 * What is usable is limited by anyhttp as much as by the records. TLS exists on HTTP/3 only, so an
 * "https" origin can be reached through a record that has "h3" in its ALPN set and in no other way.
 * And anyhttp verifies the certificate against the host it connects to, while RFC 9460 (section
 * 9.1) wants that to be the origin, so a record is usable only if it stays on the origin's name.
 *
 * An "http" origin is reached in the clear, over HTTP/1.1, unless it has an alias or a compatible
 * record. Those mean it is to be treated as "https" (section 9.5), with no way back to cleartext
 * if the result turns out not to be usable.
 */
Plan plan_routes(const Origin& origin, const ServiceRecords& found);

// =================================================================================================

} // namespace crawler
