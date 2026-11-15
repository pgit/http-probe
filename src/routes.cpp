#include "routes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>

namespace http_probe
{

namespace
{

// =================================================================================================

//
// What a ServiceMode record says, once its parameters have been read.
//
struct Service
{
   std::vector<std::string> alpn;
   bool no_default_alpn = false;
   std::optional<std::uint16_t> port;
   std::optional<std::string> incompatible; // why this record has to be ignored, see section 8

   bool offers(std::string_view protocol) const { return std::ranges::contains(alpn, protocol); }

   //
   // What to connect with, of those to speak, in the order to try them, which is at most one
   // connection per transport. anyhttp's h2 offers HTTP/1.1 along with it, so that a server still
   // has the choice over TCP. Where h1 is not to be spoken, the prober does not take that choice.
   //
   std::vector<anyhttp::Protocol> protocols(const Protocols& to_speak = {}) const
   {
      using enum anyhttp::Protocol;
      std::vector<anyhttp::Protocol> result;
      if (offers("h3") && to_speak.speaks(h3))
         result.push_back(h3);
      if (offers("h2") && to_speak.speaks(h2))
         result.push_back(h2);
      else if (offers("http/1.1") && to_speak.speaks(h1))
         result.push_back(h1);
      return result;
   }
};

std::vector<std::string> split_list(std::string_view list)
{
   return list | std::views::split(',') |
          std::views::transform([](auto part) { return std::string(std::string_view(part)); }) |
          std::ranges::to<std::vector>();
}

//
// A key that can be listed as mandatory for a client like this one: everything it either acts on
// or may leave alone, like the address hints. Not "ech", which it can not do, and not a key it has
// no name for.
//
bool can_be_mandatory(std::string_view key)
{
   static constexpr std::array<std::string_view, 6> keys{"mandatory", "alpn",     "no-default-alpn",
                                                         "port",      "ipv4hint", "ipv6hint"};
   return std::ranges::contains(keys, key);
}

Service parse_service(const c_ares_async::https_record& record)
{
   Service service;
   for (std::string_view param : record.params)
   {
      const auto equals = param.find('=');
      const auto key = param.substr(0, equals);
      const auto value = equals == param.npos ? std::string_view{} : param.substr(equals + 1);

      if (key == "alpn")
         service.alpn = split_list(value);
      else if (key == "no-default-alpn")
         service.no_default_alpn = true;
      else if (key == "port")
      {
         std::uint16_t port = 0;
         const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
         if (ec != std::errc{} || end != value.data() + value.size())
            service.incompatible = std::format("invalid port '{}'", value);
         else
            service.port = port;
      }
      else if (key == "mandatory")
         for (const auto& mandatory : split_list(value))
            if (!can_be_mandatory(mandatory))
               service.incompatible = std::format("mandatory key '{}' is not supported", mandatory);
   }

   if (!service.no_default_alpn)
      service.alpn.emplace_back("http/1.1"); // the default set of the HTTPS record, section 9.1
   return service;
}

bool is_own_name(std::string_view target) { return target.empty() || target == "."; }

// -------------------------------------------------------------------------------------------------

constexpr std::string_view whitespace = " \t";

std::string_view trim(std::string_view text)
{
   const auto first = text.find_first_not_of(whitespace);
   if (first == text.npos)
      return {};
   return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

//
// The elements of a list in a field (RFC 9110, section 5.6.1), split at the commas that are not
// in a quoted string.
//
std::vector<std::string_view> split_field_list(std::string_view list)
{
   std::vector<std::string_view> elements;
   bool quoted = false;
   std::size_t start = 0;
   for (std::size_t i = 0; i < list.size(); ++i)
   {
      if (quoted && list[i] == '\\')
         ++i;
      else if (list[i] == '"')
         quoted = !quoted;
      else if (!quoted && list[i] == ',')
      {
         elements.push_back(list.substr(start, i - start));
         start = i + 1;
      }
   }
   elements.push_back(list.substr(start));
   return elements;
}

bool is_tchar(unsigned char c)
{
   return std::isalnum(c) != 0 ||
          std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
}

//
// A protocol-id is a token with anything else in it percent-encoded (RFC 7838, section 3).
//
std::optional<std::string> percent_decode(std::string_view text)
{
   std::string decoded;
   for (std::size_t i = 0; i < text.size(); ++i)
   {
      if (text[i] != '%')
      {
         decoded += text[i];
         continue;
      }
      unsigned value = 0;
      if (i + 2 >= text.size())
         return std::nullopt;
      const auto [end, ec] = std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
      if (ec != std::errc{} || end != text.data() + i + 3)
         return std::nullopt;
      decoded += static_cast<char>(value);
      i += 2;
   }
   return decoded;
}

//
// One alternative: protocol-id "=" alt-authority, with any parameters after it left alone.
//
std::optional<AltService> parse_alternative(std::string_view element)
{
   const auto equals = element.find('=');
   if (equals == 0 || equals == element.npos ||
       !std::ranges::all_of(element.substr(0, equals), is_tchar))
      return std::nullopt;
   auto protocol_id = percent_decode(element.substr(0, equals));
   if (!protocol_id)
      return std::nullopt;

   //
   // The authority is a quoted-string, which may have quoted pairs in it.
   //
   auto rest = element.substr(equals + 1);
   if (!rest.starts_with('"'))
      return std::nullopt;
   std::string authority;
   std::size_t i = 1;
   for (; i < rest.size() && rest[i] != '"'; ++i)
   {
      if (rest[i] == '\\' && ++i == rest.size())
         return std::nullopt;
      authority += rest[i];
   }
   if (i == rest.size())
      return std::nullopt;

   if (const auto parameters = trim(rest.substr(i + 1));
       !parameters.empty() && !parameters.starts_with(';'))
      return std::nullopt;

   //
   // [uri-host] ":" port, where the host may be an IPv6 literal with colons of its own.
   //
   const auto colon = authority.rfind(':');
   if (colon == authority.npos)
      return std::nullopt;
   const std::string_view port_text = std::string_view(authority).substr(colon + 1);
   std::uint16_t port = 0;
   const auto [end, ec] =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
   if (ec != std::errc{} || end != port_text.data() + port_text.size() || port == 0)
      return std::nullopt;

   return AltService{
      .protocol_id = std::move(*protocol_id), .host = authority.substr(0, colon), .port = port};
}

std::optional<anyhttp::Protocol> alt_svc_protocol(std::string_view protocol_id)
{
   using enum anyhttp::Protocol;
   if (protocol_id == "h3")
      return h3;
   if (protocol_id == "h2")
      return h2;
   if (protocol_id == "http/1.1")
      return h1;
   return std::nullopt;
}

//
// Where a connection goes, as far as the host stays the same: QUIC or TCP, and the port. Over TCP,
// it is up to the server which of HTTP/2 and HTTP/1.1 it speaks.
//
bool same_endpoint(const Route& a, const Route& b)
{
   using enum anyhttp::Protocol;
   return (a.protocol == h3) == (b.protocol == h3) && a.port == b.port;
}

// =================================================================================================

} // namespace

std::string normalize_host(std::string_view host)
{
   if (host.ends_with('.'))
      host.remove_suffix(1);

   std::string result(host);
   std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
   return result;
}

std::string to_text(const c_ares_async::https_record& record)
{
   auto text =
      std::format("HTTPS {} {}", record.priority, record.target.empty() ? "." : record.target);
   for (const auto& param : record.params)
      text += " " + param;
   return text;
}

std::string_view protocol_name(anyhttp::Protocol protocol)
{
   using enum anyhttp::Protocol;
   return protocol == h3 ? "h3" : protocol == h2 ? "h2" : "h1";
}

//
// No 'using enum' in the members, which would hide the fields of the same names.
//
bool Protocols::speaks(anyhttp::Protocol protocol) const
{
   if (!h3 && !h2 && !h1)
      return true;
   switch (protocol)
   {
   case anyhttp::Protocol::h3:
      return h3;
   case anyhttp::Protocol::h2:
      return h2;
   case anyhttp::Protocol::h1:
      return h1;
   }
   return false;
}

bool Protocols::speaks_tcp() const
{
   return speaks(anyhttp::Protocol::h2) || speaks(anyhttp::Protocol::h1);
}

std::string Protocols::to_string() const
{
   std::string text;
   for (const auto protocol : {anyhttp::Protocol::h3, anyhttp::Protocol::h2, anyhttp::Protocol::h1})
      if (speaks(protocol))
         text += std::format("{}{}", text.empty() ? "" : " or ", protocol_name(protocol));
   return text;
}

std::string Route::to_string() const
{
   return std::format("{} {}:{}", protocol_name(protocol), host, port);
}

Plan plan_routes(std::string_view host, const ServiceRecords& found, const Protocols& protocols,
                 std::uint16_t default_port)
{
   Plan plan{.rejected = found.notes};

   if (found.lookup_failed || found.not_found)
      return plan;

   auto records = found.records;
   std::ranges::stable_sort(records, {}, &c_ares_async::https_record::priority);

   bool any_compatible = false; // a record to take seriously, with a protocol that anyhttp speaks
   bool any_to_speak = false; // one of those that offers a protocol to speak, wherever it points
   for (const auto& record : records)
   {
      const auto text = to_text(record);
      const auto service = parse_service(record);
      if (service.incompatible)
      {
         plan.rejected.push_back(std::format("{}: {}", text, *service.incompatible));
         continue;
      }

      if (service.protocols().empty())
      {
         plan.rejected.push_back(std::format("{}: no protocol that anyhttp speaks", text));
         continue;
      }
      any_compatible = true;

      const auto to_speak = service.protocols(protocols);
      if (to_speak.empty())
      {
         plan.rejected.push_back(std::format("{}: offers no {}", text, protocols.to_string()));
         continue;
      }
      any_to_speak = true;

      const auto target = normalize_host(is_own_name(record.target) ? found.owner : record.target);
      if (target != host)
      {
         plan.rejected.push_back(std::format(
            "{}: target '{}' is not the host, and anyhttp can only verify the name it connects to",
            text, target));
         continue;
      }

      for (const auto protocol : to_speak)
      {
         Route route{.protocol = protocol,
                     .host = target,
                     .port = service.port.value_or(default_port),
                     .source = text};
         const auto same = [&](const Route& other) {
            return other.protocol == route.protocol && other.port == route.port;
         };
         if (std::ranges::none_of(plan.routes, same)) // a second try would only fail the same way
            plan.routes.push_back(std::move(route));
      }
   }

   if (!plan.routes.empty())
      return plan;

   //
   // Not one of the protocols to speak, wherever it would have been. Without a usable record, what
   // there is is TCP, and that goes for one who says that the service is not available, too.
   //
   if (any_compatible ? !any_to_speak : !protocols.speaks_tcp())
   {
      if (!any_compatible)
         plan.rejected.push_back(
            std::format("no usable HTTPS record offers {}", protocols.to_string()));
      plan.left_out = true;
      return plan;
   }

   if (found.unavailable)
   {
      plan.out_of_reach = true;
      return plan;
   }

   //
   // After an alias, the name it led to is an endpoint even without any parameters (section 3).
   //
   if (found.aliased && !any_compatible)
      plan.rejected.push_back(std::format(
         "alias target '{}' is not the host, and anyhttp can only verify the name it connects to",
         found.owner));

   if (found.aliased || any_compatible)
   {
      plan.out_of_reach = true;
      return plan;
   }

   using enum anyhttp::Protocol;
   plan.routes.push_back(Route{.protocol = protocols.speaks(h2) ? h2 : h1,
                               .host = std::string(host),
                               .port = default_port,
                               .source = "no usable HTTPS record"});
   return plan;
}

// -------------------------------------------------------------------------------------------------

std::string AltService::to_string() const
{
   return std::format("{}=\"{}:{}\"", protocol_id, host, port);
}

std::vector<AltService> parse_alt_svc(std::string_view value, std::vector<std::string>& skipped)
{
   std::vector<AltService> alternatives;
   for (const auto element : split_field_list(value) | std::views::transform(trim))
   {
      if (element.empty())
         continue;
      if (element == "clear")
         return {}; // whatever else there is, the server wants none of it
      if (auto alternative = parse_alternative(element))
         alternatives.push_back(std::move(*alternative));
      else
         skipped.emplace_back(element);
   }
   return alternatives;
}

AltSvcPlan plan_alt_svc(std::string_view host, const std::vector<AltService>& alternatives,
                        const std::vector<Route>& tried, const Protocols& protocols)
{
   AltSvcPlan plan;
   for (const auto& alternative : alternatives)
   {
      const auto text = std::format("Alt-Svc {}", alternative.to_string());
      const auto protocol = alt_svc_protocol(alternative.protocol_id);
      if (!protocol)
      {
         plan.rejected.push_back(std::format("{}: no protocol that anyhttp speaks", text));
         continue;
      }
      if (!protocols.speaks(*protocol))
      {
         plan.rejected.push_back(std::format("{}: not {}", text, protocols.to_string()));
         continue;
      }
      if (!alternative.host.empty() && normalize_host(alternative.host) != host)
      {
         plan.rejected.push_back(std::format("{}: '{}' is not the host, and no other host is asked",
                                             text, alternative.host));
         continue;
      }

      Route route{.protocol = *protocol,
                  .host = std::string(host),
                  .port = alternative.port,
                  .source = text};
      const auto same = [&](const Route& other) { return same_endpoint(route, other); };
      if (const auto it = std::ranges::find_if(tried, same); it != tried.end())
      {
         plan.rejected.push_back(std::format("{}: tried already, as {}", text, it->to_string()));
         continue;
      }

      plan.route = std::move(route);
      return plan;
   }
   return plan;
}

// =================================================================================================

} // namespace http_probe
