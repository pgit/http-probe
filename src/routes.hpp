//
// From the HTTPS records of a name to the ways of connecting to it (RFC 9460). No I/O in here:
// prober.cpp asks the questions, this says what the answers mean.
//
#pragma once

#include <anyhttp/common.hpp>

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
   bool not_found = false; ///< the host itself does not exist
   bool unavailable = false; ///< an alias to "." says that the service is not available
};

/// One way of connecting, always over TLS. The TLS name and the authority stay the host's.
struct Route
{
   anyhttp::Protocol protocol = anyhttp::Protocol::h3;
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
   bool out_of_reach = false; ///< the records say not to connect, or only to a target we can't use
};

/**
 * Says how to connect to https://\p host/, given what was found for it.
 *
 * Records go by priority, and within a record, HTTP/3 comes ahead of TCP. A name without a usable
 * record, the most common thing there is, gets \p default_port over TCP, and so does a record that
 * does not say which port (section 9.1 on both of them).
 *
 * anyhttp verifies the certificate against the host it connects to, while RFC 9460 (section 9.1)
 * wants that to be the origin, so a record is usable only if it stays on the host's own name. If
 * none of them does, the host is out of reach: the records do say where to go, and it is not here.
 * A name that does not exist and a lookup that failed have no routes either.
 */
Plan plan_routes(std::string_view host, const ServiceRecords& found,
                 std::uint16_t default_port = 443);

/// Whether a record that has to be taken seriously (section 8) offers HTTP/3, wherever it points.
bool has_h3_record(const ServiceRecords& found);

// =================================================================================================

} // namespace http_probe
