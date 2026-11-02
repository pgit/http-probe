#include "robots.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ranges>

namespace crawler
{

namespace
{

// =================================================================================================

constexpr std::size_t parse_limit = 500 * 1024; // RFC 9309, section 2.5: at least this much

std::string_view trim(std::string_view text)
{
   const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
   while (!text.empty() && is_space(text.front()))
      text.remove_prefix(1);
   while (!text.empty() && is_space(text.back()))
      text.remove_suffix(1);
   return text;
}

std::string lower(std::string_view text)
{
   std::string result(text);
   std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
   return result;
}

//
// "*" matches any number of characters, and "$" at the end of a pattern the end of the target. A
// pattern is a prefix otherwise.
//
bool matches(std::string_view pattern, std::string_view target)
{
   const bool anchored = pattern.ends_with('$');
   if (anchored)
      pattern.remove_suffix(1);

   std::size_t p = 0, t = 0;
   std::size_t star = std::string_view::npos, resume = 0;
   while (p < pattern.size())
   {
      if (pattern[p] == '*')
      {
         star = p++;
         resume = t;
      }
      else if (t < target.size() && pattern[p] == target[t])
      {
         ++p;
         ++t;
      }
      else if (star != std::string_view::npos && resume < target.size())
      {
         p = star + 1;
         t = ++resume;
      }
      else
         return false;
   }

   if (!anchored || t == target.size())
      return true;

   //
   // Anchored, with some of the target left: only an earlier "*" can use it up, by matching what
   // follows it against the end of the target.
   //
   if (star == std::string_view::npos)
      return false;
   const auto tail = pattern.substr(star + 1);
   return target.size() - resume >= tail.size() && target.ends_with(tail);
}

// =================================================================================================

} // namespace

Robots Robots::disallow_all()
{
   Robots robots;
   robots.rules_.push_back(Rule{.pattern = "/", .allow = false});
   return robots;
}

Robots Robots::parse(std::string_view text, std::string_view agent)
{
   struct Group
   {
      std::vector<std::string> agents;
      std::vector<Rule> rules;
      std::optional<std::chrono::milliseconds> crawl_delay;
   };
   std::vector<Group> groups;
   bool in_agent_lines = false;

   for (auto part : text.substr(0, parse_limit) | std::views::split('\n'))
   {
      auto line = std::string_view(part);
      line = trim(line.substr(0, line.find('#')));
      const auto colon = line.find(':');
      if (colon == line.npos)
         continue;

      const auto key = lower(trim(line.substr(0, colon)));
      const auto value = trim(line.substr(colon + 1));

      if (key == "user-agent")
      {
         if (!in_agent_lines)
            groups.emplace_back();
         groups.back().agents.push_back(lower(value));
         in_agent_lines = true;
         continue;
      }

      if (key != "allow" && key != "disallow" && key != "crawl-delay")
         continue;

      in_agent_lines = false;
      if (groups.empty())
         continue; // a rule ahead of any "User-agent" applies to nobody

      if (key == "crawl-delay")
      {
         //
         // Fractions are common. Up to a day, as there is no telling a site that means it from
         // one that does not, and to stay well within what the duration can hold.
         //
         const std::string number(value);
         char* end = nullptr;
         const double seconds = std::strtod(number.c_str(), &end);
         if (end != number.c_str() && *end == '\0' && std::isfinite(seconds) && seconds >= 0)
            groups.back().crawl_delay =
               std::chrono::milliseconds(static_cast<long long>(std::min(seconds, 86400.0) * 1000));
      }
      else if (!value.empty()) // an empty "Disallow" does not disallow anything
         groups.back().rules.push_back(
            Rule{.pattern = std::string(value), .allow = key == "allow"});
   }

   //
   // All groups naming the agent apply, merged, and those for "*" only if there is none of those.
   //
   const auto select = [&](std::string_view name) {
      Robots robots;
      bool any = false;
      for (const auto& group : groups)
         if (std::ranges::contains(group.agents, name))
         {
            any = true;
            robots.rules_.insert(robots.rules_.end(), group.rules.begin(), group.rules.end());
            if (group.crawl_delay)
               robots.crawl_delay_ = std::max(
                  robots.crawl_delay_.value_or(std::chrono::milliseconds{}), *group.crawl_delay);
         }
      return std::pair{any, std::move(robots)};
   };

   auto [named, robots] = select(lower(agent));
   return named ? std::move(robots) : select("*").second;
}

bool Robots::allowed(std::string_view target) const
{
   if (target == "/robots.txt")
      return true; // implicitly allowed, RFC 9309 section 2.2.2

   const Rule* best = nullptr;
   for (const auto& rule : rules_)
      if (matches(rule.pattern, target) &&
          (!best || rule.pattern.size() > best->pattern.size() ||
           (rule.pattern.size() == best->pattern.size() && rule.allow)))
         best = &rule;

   return !best || best->allow;
}

// =================================================================================================

} // namespace crawler
