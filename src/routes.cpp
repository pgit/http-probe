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

constexpr std::string_view no_tls_over_tcp =
   "needs TLS over TCP, which anyhttp does not have yet (only HTTP/3 has TLS)";

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

std::string Route::to_string() const { return std::format("h3 {}:{}", host, port); }

Plan plan_routes(std::string_view host, const ServiceRecords& found)
{
   Plan plan{.rejected = found.notes};

   auto records = found.records;
   std::ranges::stable_sort(records, {}, &c_ares_async::https_record::priority);

   bool any_compatible = false;
   for (const auto& record : records)
   {
      const auto text = to_text(record);
      const auto service = parse_service(record);
      if (service.incompatible)
      {
         plan.rejected.push_back(std::format("{}: {}", text, *service.incompatible));
         continue;
      }
      any_compatible = true;

      const auto target = normalize_host(is_own_name(record.target) ? found.owner : record.target);
      if (!service.offers("h3"))
         plan.rejected.push_back(std::format("{}: {}", text, no_tls_over_tcp));
      else if (target != host)
      {
         plan.h3_out_of_reach = true;
         plan.rejected.push_back(std::format(
            "{}: target '{}' is not the host, and anyhttp can only verify the name it connects to",
            text, target));
      }
      else
         plan.routes.push_back(
            Route{.host = target, .port = service.port.value_or(443), .source = text});
   }

   //
   // After an alias, the name it led to is an endpoint even without any parameters (section 3),
   // and that is what is left for a name without any usable record, too. Either way it comes down
   // to the default ALPN set.
   //
   if (found.aliased)
      plan.rejected.push_back(
         std::format("alias target '{}' without parameters: {}", found.owner, no_tls_over_tcp));
   else if (!any_compatible)
      plan.rejected.push_back(std::format("no usable HTTPS record: {}", no_tls_over_tcp));

   return plan;
}

// =================================================================================================

} // namespace http_probe
