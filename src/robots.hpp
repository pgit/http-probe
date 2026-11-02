//
// The rules of a robots.txt (RFC 9309), and "Crawl-delay", which is not in there but which many
// sites use.
//
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crawler
{

// =================================================================================================

class Robots
{
public:
   /// Allows everything, as a site without a robots.txt does.
   Robots() = default;

   /// Allows nothing, which is what to assume of a robots.txt that can not be read.
   static Robots disallow_all();

   /// Reads the rules that apply to \p agent, the product token of the crawler.
   static Robots parse(std::string_view text, std::string_view agent);

   /**
    * Whether \p target, the path and query of a URL, may be fetched: the longest matching rule
    * decides, an "Allow" where two are of the same length. Patterns and targets are compared as
    * they are, without undoing any percent encoding in either.
    */
   bool allowed(std::string_view target) const;

   /// How long the site wants to be left alone between two requests, if it says so.
   std::optional<std::chrono::milliseconds> crawl_delay() const { return crawl_delay_; }

private:
   struct Rule
   {
      std::string pattern;
      bool allow = false;
   };

   std::vector<Rule> rules_;
   std::optional<std::chrono::milliseconds> crawl_delay_;
};

// =================================================================================================

} // namespace crawler
