//
// What the program prints: with colour, and with nothing of it left without.
//
#include "output.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace http_probe;
using namespace std::chrono_literals;
using Outcome = Result::Outcome;
using testing::HasSubstr;
using testing::Not;

namespace
{

Result answered(int status = 200)
{
   return {.host = "example.com",
           .outcome = Outcome::answered,
           .records = {{.priority = 1,
                        .target = ".",
                        .params = {"alpn=h3", "ipv4hint=192.0.2.1", "ipv6hint=2001:db8::1"}}},
           .status = status,
           .route = "h3 example.com:443",
           .fields = {{"server", "test"}},
           .body_size = 5,
           .duration = 12ms,
           .address = "192.0.2.1"};
}

} // namespace

TEST(Output, SaysWhatCameBack)
{
   EXPECT_EQ(format_result(answered(), false),
             "example.com: 200 [h3 example.com:443] at 192.0.2.1, 5 bytes in 12ms\n"
             "  record: HTTPS 1 . alpn=h3 ipv4hint=192.0.2.1 ipv6hint=2001:db8::1\n"
             "  server: test\n");
}

TEST(Output, SaysWhichAddressFailed)
{
   EXPECT_EQ(format_result({.host = "example.com",
                            .outcome = Outcome::failed,
                            .address = "2001:db8::1",
                            .error = "[h3 example.com:443] timed out"},
                           false),
             "example.com: failed at 2001:db8::1: [h3 example.com:443] timed out\n");
}

TEST(Output, ColoursAnAddressByItsFamily)
{
   auto result = answered();
   EXPECT_THAT(format_result(result, true), HasSubstr("at \x1b[35m192.0.2.1\x1b[0m,"));

   result.address = "2001:db8::1";
   EXPECT_THAT(format_result(result, true), HasSubstr("at \x1b[34m2001:db8::1\x1b[0m,"));
}

TEST(Output, SaysWhyThereIsNoResponse)
{
   EXPECT_EQ(format_result({.host = "example.com",
                            .outcome = Outcome::failed,
                            .error = "[h3 example.com:443] timed out",
                            .notes = {"some note"}},
                           false),
             "example.com: failed: [h3 example.com:443] timed out\n  note: some note\n");
   EXPECT_EQ(format_result({.host = "example.com", .outcome = Outcome::out_of_reach}, false),
             "example.com: out of reach\n");
   EXPECT_EQ(format_result({.host = "example.com", .outcome = Outcome::not_found}, false),
             "example.com: not found\n");
   EXPECT_EQ(format_result({.host = "example.com", .outcome = Outcome::lookup_failed}, false),
             "example.com: lookup failed\n");
}

TEST(Output, ColoursTheWayCAresAsyncDoes)
{
   const auto text = format_result(answered(), true);

   EXPECT_THAT(text, HasSubstr("\x1b[33mexample.com\x1b[0m:")) << "host names are yellow";
   EXPECT_THAT(text, HasSubstr("ipv4hint=\x1b[35m192.0.2.1\x1b[0m")) << "IPv4 is magenta";
   EXPECT_THAT(text, HasSubstr("ipv6hint=\x1b[34m2001:db8::1\x1b[0m")) << "IPv6 is blue";
}

TEST(Output, LeavesTheCommasOfAnAddressListWithoutColour)
{
   auto result = answered();
   result.records[0].params = {"ipv4hint=192.0.2.1,192.0.2.2", "ipv6hint=2001:db8::1,2001:db8::2"};

   const auto text = format_result(result, true);
   EXPECT_THAT(text, HasSubstr("ipv4hint=\x1b[35m192.0.2.1\x1b[0m,\x1b[35m192.0.2.2\x1b[0m"));
   EXPECT_THAT(text, HasSubstr("ipv6hint=\x1b[34m2001:db8::1\x1b[0m,\x1b[34m2001:db8::2\x1b[0m"));

   EXPECT_THAT(format_result(result, false),
               HasSubstr("ipv4hint=192.0.2.1,192.0.2.2 ipv6hint=2001:db8::1,2001:db8::2"));
}

TEST(Output, ColoursATargetAsAHostName)
{
   auto result = answered();
   result.records[0].target = "svc.example.net";

   EXPECT_THAT(format_result(result, true), HasSubstr("\x1b[33msvc.example.net\x1b[0m"));
   EXPECT_THAT(format_result(result, false), HasSubstr("HTTPS 1 svc.example.net alpn=h3"));
}

TEST(Output, ColoursTheStatusByItsClass)
{
   EXPECT_THAT(format_result(answered(204), true), HasSubstr("\x1b[32m204\x1b[0m"));
   EXPECT_THAT(format_result(answered(301), true), HasSubstr("\x1b[36m301\x1b[0m"));
   EXPECT_THAT(format_result(answered(404), true), HasSubstr("\x1b[31m404\x1b[0m"));
   EXPECT_THAT(format_result(answered(503), true), HasSubstr("\x1b[31m503\x1b[0m"));
}

TEST(Output, HasNoColourWithoutColour)
{
   for (const auto& result :
        {answered(), Result{.host = "h", .outcome = Outcome::failed, .notes = {"n"}},
         Result{.host = "h", .outcome = Outcome::lookup_failed}})
      EXPECT_THAT(format_result(result, false), Not(HasSubstr("\x1b")));

   const Counts counts{{Outcome::answered, 1}, {Outcome::failed, 1}};
   EXPECT_THAT(format_summary(counts, false), Not(HasSubstr("\x1b")));
}

TEST(Output, CountsTheOutcomes)
{
   EXPECT_EQ(format_summary({{Outcome::answered, 3},
                             {Outcome::failed, 1},
                             {Outcome::not_found, 900},
                             {Outcome::lookup_failed, 17}},
                            false),
             "3 answered, 1 failed, 0 out of reach, 900 not found, 17 lookups failed");
}

TEST(Output, DrawsAttentionOnlyToWhatHappened)
{
   const auto text = format_summary({{Outcome::answered, 1}}, true);

   EXPECT_THAT(text, HasSubstr("\x1b[32m1 answered\x1b[0m"));
   EXPECT_THAT(text, HasSubstr(", 0 failed,")) << "nothing to see, no colour";
}
