//
// From the HTTPS records of a name to the ways of connecting to it (RFC 9460). No I/O in here:
// prober.cpp asks the questions, this says what the answers mean.
//
#pragma once

#include <c_ares_async/resolver.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace http_probe
{

// =================================================================================================

/// Lower case and without the dot at the end, which is how host names are compared.
std::string normalize_host(std::string_view host);

/// A record the way a zone file has it, without the name: "HTTPS 1 . alpn=h3 port=8443".
std::string to_text(const c_ares_async::https_record& record);

// -------------------------------------------------------------------------------------------------

/// What SVCB resolution (section 3) found for a host.
struct ServiceRecords
{
   std::string owner; ///< the name the records are of, at the end of any alias chain
   bool aliased = false; ///< at least one AliasMode record was followed
   std::vector<c_ares_async::https_record> records; ///< the ServiceMode records of \c owner
   std::vector<std::string> notes; ///< what went wrong on the way, none of which is fatal
   bool lookup_failed = false; ///< no answer, which is not to say that there are no records
};

/// One way of connecting, which is HTTP/3 so far. The TLS name and the authority stay the host's.
struct Route
{
   std::string host; ///< the name to connect to
   std::uint16_t port = 443;
   std::string source; ///< the record that led to this route

   std::string to_string() const; ///< "h3 example.com:443"
};

/// How to reach a host.
struct Plan
{
   std::vector<Route> routes; ///< in the order to try them; empty if there is no usable way
   std::vector<std::string> rejected; ///< what the records offered, or we knew of, and why not
   bool h3_out_of_reach = false; ///< a record offers HTTP/3, but on a target that can't be used
};

/**
 * Says how to connect to https://\p host/, given what was found for it.
 *
 * What is usable is limited by anyhttp as much as by the records. TLS exists on HTTP/3 only, so
 * a host can be reached through a record that has "h3" in its ALPN set and in no other way. And
 * anyhttp verifies the certificate against the host it connects to, while RFC 9460 (section 9.1)
 * wants that to be the origin, so a record is usable only if it stays on the host's own name.
 */
Plan plan_routes(std::string_view host, const ServiceRecords& found);

// =================================================================================================

} // namespace http_probe
