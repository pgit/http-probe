//
// Finding the links in a page, and making absolute URLs out of them.
//
#pragma once

#include <boost/url/url.hpp>
#include <boost/url/url_view.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crawler
{

// =================================================================================================

/**
 * The targets of the \c <a> elements in \p html, in the order they appear and as they are written,
 * without those marked rel="nofollow".
 *
 * This is a scanner, not an HTML parser. It knows about quotes, comments, and the contents of
 * \c <script> and \c <style>, which is what it takes not to be fooled by what only looks like a
 * tag. There is no \c <base>, and the only character reference decoded is "&amp;", the one that is
 * common in a URL.
 */
std::vector<std::string> extract_links(std::string_view html);

/**
 * Makes a URL to crawl out of \p reference: resolved against \p base, normalized and without its
 * fragment. Nothing for anything other than "http" and "https", which includes every "mailto:" and
 * "javascript:", for a URL with user information in it, and for what does not parse.
 */
std::optional<boost::urls::url> resolve_link(boost::urls::url_view base,
                                             std::string_view reference);

// =================================================================================================

} // namespace crawler
