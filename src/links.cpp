#include "links.hpp"

#include <boost/url/parse.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace crawler
{

namespace
{

// =================================================================================================

bool equals_ignoring_case(std::string_view a, std::string_view b)
{
   return std::ranges::equal(
      a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

//
// Where \p needle starts in \p text, from \p from on and in any case.
//
std::size_t find_ignoring_case(std::string_view text, std::string_view needle, std::size_t from)
{
   for (std::size_t i = from; i + needle.size() <= text.size(); ++i)
      if (equals_ignoring_case(text.substr(i, needle.size()), needle))
         return i;
   return std::string_view::npos;
}

std::string decode_ampersands(std::string_view text)
{
   std::string result;
   for (std::size_t i = 0; i < text.size();)
      if (text.substr(i, 5) == "&amp;")
      {
         result += '&';
         i += 5;
      }
      else
         result += text[i++];
   return result;
}

std::string_view trim(std::string_view text)
{
   const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
   while (!text.empty() && is_space(text.front()))
      text.remove_prefix(1);
   while (!text.empty() && is_space(text.back()))
      text.remove_suffix(1);
   return text;
}

// =================================================================================================

} // namespace

std::vector<std::string> extract_links(std::string_view html)
{
   std::vector<std::string> links;
   const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };

   std::size_t pos = 0;
   while ((pos = html.find('<', pos)) != html.npos)
   {
      if (html.substr(pos, 4) == "<!--")
      {
         const auto end = html.find("-->", pos + 4);
         if (end == html.npos)
            break;
         pos = end + 3;
         continue;
      }

      auto cursor = pos + 1;
      const auto name_start = cursor;
      while (cursor < html.size() && std::isalnum(static_cast<unsigned char>(html[cursor])))
         ++cursor;
      const auto name = html.substr(name_start, cursor - name_start);
      if (name.empty())
      {
         ++pos; // a closing tag, a declaration, or just a '<' in the text
         continue;
      }

      //
      // The attributes, up to the end of the tag.
      //
      std::string_view href, rel;
      bool has_href = false;
      while (cursor < html.size() && html[cursor] != '>')
      {
         if (is_space(html[cursor]) || html[cursor] == '/')
         {
            ++cursor;
            continue;
         }

         const auto attribute_start = cursor;
         while (cursor < html.size() && !is_space(html[cursor]) && html[cursor] != '=' &&
                html[cursor] != '>')
            ++cursor;
         const auto attribute = html.substr(attribute_start, cursor - attribute_start);

         while (cursor < html.size() && is_space(html[cursor]))
            ++cursor;
         if (cursor >= html.size() || html[cursor] != '=')
            continue; // an attribute without a value

         ++cursor;
         while (cursor < html.size() && is_space(html[cursor]))
            ++cursor;

         std::string_view value;
         if (cursor < html.size() && (html[cursor] == '"' || html[cursor] == '\''))
         {
            const auto end = html.find(html[cursor], cursor + 1);
            if (end == html.npos)
               return links; // the quote never ends, and neither does the tag
            value = html.substr(cursor + 1, end - cursor - 1);
            cursor = end + 1;
         }
         else
         {
            const auto value_start = cursor;
            while (cursor < html.size() && !is_space(html[cursor]) && html[cursor] != '>')
               ++cursor;
            value = html.substr(value_start, cursor - value_start);
         }

         if (equals_ignoring_case(attribute, "href"))
         {
            href = value;
            has_href = true;
         }
         else if (equals_ignoring_case(attribute, "rel"))
            rel = value;
      }

      if (equals_ignoring_case(name, "a") && has_href &&
          find_ignoring_case(rel, "nofollow", 0) == rel.npos)
         links.push_back(decode_ampersands(trim(href)));

      pos = cursor;

      //
      // What is inside of these is not markup, whatever it looks like.
      //
      if (equals_ignoring_case(name, "script") || equals_ignoring_case(name, "style"))
      {
         const auto end = find_ignoring_case(html, std::string("</") + std::string(name), pos);
         if (end == html.npos)
            break;
         pos = end;
      }
   }
   return links;
}

std::optional<boost::urls::url> resolve_link(boost::urls::url_view base, std::string_view reference)
{
   const auto parsed = boost::urls::parse_uri_reference(reference);
   if (!parsed)
      return std::nullopt;

   boost::urls::url url;
   if (!boost::urls::resolve(base, *parsed, url))
      return std::nullopt;

   const auto scheme = url.scheme_id();
   if ((scheme != boost::urls::scheme::http && scheme != boost::urls::scheme::https) ||
       url.encoded_host().empty() || url.has_userinfo())
      return std::nullopt;

   url.remove_fragment();
   url.normalize();
   if (url.encoded_path().empty())
      url.set_encoded_path("/");
   return url;
}

// =================================================================================================

} // namespace crawler
