//
// What the program prints, in the colours of c-ares-async's example where it can: host names are
// yellow, and addresses come the way "ip -color address" gives them, IPv4 in magenta and IPv6 in
// blue. Without colour, the text is the same but for the escape sequences.
//
#pragma once

#include "prober.hpp"

#include <cstddef>
#include <map>
#include <string>

namespace http_probe
{

// =================================================================================================

using Counts = std::map<Result::Outcome, std::size_t>;

/// What to print of a result, and how.
struct Style
{
   bool color = false;
   bool all_headers = false; ///< the header fields of each response, not only its status line
};

/// All there is to say about one host, a line each and with a line break at the end. A response
/// has a line of its own, and with Style::all_headers its header fields after that.
std::string format_result(const Result& result, const Style& style);

/// How it went overall, without a line break.
std::string format_summary(Counts counts, bool color);

/// The numbers of a run, a line each and with a line break at the end. What is about responses is
/// left out where there were none.
std::string format_stats(const Stats& stats);

// =================================================================================================

} // namespace http_probe
