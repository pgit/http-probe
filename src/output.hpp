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

/// All there is to say about one host, a line each and with a line break at the end.
std::string format_result(const Result& result, bool color);

/// How it went overall, without a line break.
std::string format_summary(Counts counts, bool color);

// =================================================================================================

} // namespace http_probe
