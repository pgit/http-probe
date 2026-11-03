#include "output.hpp"

#include <format>
#include <string_view>

namespace http_probe
{

namespace
{

// =================================================================================================

namespace color_of
{
constexpr std::string_view name = "33";
constexpr std::string_view v4 = "35";
constexpr std::string_view v6 = "34";
constexpr std::string_view good = "32";
constexpr std::string_view moved = "36";
constexpr std::string_view bad = "31";
constexpr std::string_view field = "1;34"; // as anyhttp logs header fields
constexpr std::string_view note = "2";
} // namespace color_of

std::string colored(std::string_view text, std::string_view code, bool color)
{
   return color ? std::format("\x1b[{}m{}\x1b[0m", code, text) : std::string(text);
}

std::string status_text(int status, bool color)
{
   const auto code = status >= 200 && status < 300   ? color_of::good
                     : status >= 300 && status < 400 ? color_of::moved
                                                     : color_of::bad;
   return colored(std::to_string(status), code, color);
}

//
// to_text() with colours: the target, and the values of the address hints.
//
std::string record_text(const c_ares_async::https_record& record, bool color)
{
   auto text = std::format("HTTPS {} {}", record.priority,
                           record.target.empty() || record.target == "."
                              ? "."
                              : colored(record.target, color_of::name, color));

   for (std::string_view param : record.params)
   {
      const bool v4 = param.starts_with("ipv4hint=");
      if (v4 || param.starts_with("ipv6hint="))
      {
         const auto value = param.find('=') + 1;
         text += std::format(" {}{}", param.substr(0, value),
                             colored(param.substr(value), v4 ? color_of::v4 : color_of::v6, color));
      }
      else
         text += std::format(" {}", param);
   }
   return text;
}

// =================================================================================================

} // namespace

std::string format_result(const Result& result, bool color)
{
   using Outcome = Result::Outcome;

   const auto host = colored(result.host, color_of::name, color);
   std::string text;
   switch (result.outcome)
   {
   case Outcome::answered:
      text = std::format("{}: {} [{}] {} bytes in {}\n", host, status_text(result.status, color),
                         result.route, result.body_size, result.duration);
      break;
   case Outcome::failed:
      text =
         std::format("{}: {}: {}\n", host, colored("failed", color_of::bad, color), result.error);
      break;
   case Outcome::out_of_reach:
      text = std::format("{}: {}\n", host, colored("out of reach", color_of::moved, color));
      break;
   case Outcome::not_found:
      text = std::format("{}: not found\n", host);
      break;
   case Outcome::lookup_failed:
      text = std::format("{}: {}\n", host, colored("lookup failed", color_of::bad, color));
      break;
   }

   for (const auto& record : result.records)
      text += std::format("  record: {}\n", record_text(record, color));
   for (const auto& note : result.notes)
      text += colored(std::format("  note: {}", note), color_of::note, color) + "\n";
   for (const auto& [name, value] : result.fields)
      text += std::format("  {}: {}\n", colored(name, color_of::field, color), value);
   return text;
}

std::string format_summary(Counts counts, bool color)
{
   using Outcome = Result::Outcome;

   const auto count = [&](Outcome outcome, std::string_view text, std::string_view code) {
      const auto line = std::format("{} {}", counts[outcome], text);
      return counts[outcome] ? colored(line, code, color) : line;
   };
   return std::format(
      "{}, {}, {}, {} not found, {}", count(Outcome::answered, "answered", color_of::good),
      count(Outcome::failed, "failed", color_of::bad),
      count(Outcome::out_of_reach, "out of reach", color_of::moved), counts[Outcome::not_found],
      count(Outcome::lookup_failed, "lookups failed", color_of::bad));
}

// =================================================================================================

} // namespace http_probe
