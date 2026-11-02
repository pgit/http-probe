//
// What the records of a name mean for how to connect, without any I/O.
//
#include "routes.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace http_probe;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

namespace
{

c_ares_async::https_record record(unsigned short priority, std::string target,
                                  std::vector<std::string> params = {})
{
   return {.priority = priority, .target = std::move(target), .params = std::move(params)};
}

ServiceRecords found(std::vector<c_ares_async::https_record> records,
                     std::string owner = "example.com", bool aliased = false)
{
   return {.owner = std::move(owner), .aliased = aliased, .records = std::move(records)};
}

std::vector<std::string> routes(const Plan& plan)
{
   std::vector<std::string> result;
   for (const auto& route : plan.routes)
      result.push_back(route.to_string());
   return result;
}

} // namespace

// =================================================================================================

TEST(NormalizeHost, ComparesHostsTheWayDnsDoes)
{
   EXPECT_EQ(normalize_host("EXAMPLE.com."), "example.com");
}

TEST(ToText, ReadsLikeAZoneFile)
{
   EXPECT_EQ(to_text(record(1, ".", {"alpn=h3", "port=8443"})), "HTTPS 1 . alpn=h3 port=8443");
   EXPECT_EQ(to_text(record(0, "other.example")), "HTTPS 0 other.example");
   EXPECT_EQ(to_text(record(1, "")), "HTTPS 1 .");
}

// -------------------------------------------------------------------------------------------------

TEST(Plan, ConnectsOverHttp3WhereTheRecordOffersIt)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3,h2"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
}

TEST(Plan, GoesToThePortOfTheRecord)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3", "port=8443"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443"));
}

TEST(Plan, TriesTheLowestPriorityFirst)
{
   const auto plan = plan_routes("example.com", found({record(3, ".", {"alpn=h3", "port=3"}),
                                                       record(1, ".", {"alpn=h3", "port=1"}),
                                                       record(2, ".", {"alpn=h3", "port=2"})}));

   EXPECT_THAT(routes(plan),
               ElementsAre("h3 example.com:1", "h3 example.com:2", "h3 example.com:3"));
}

TEST(Plan, RejectsARecordWithoutHttp3)
{
   const auto plan =
      plan_routes("example.com",
                  found({record(1, ".", {"alpn=h2"}), record(2, ".", {"alpn=h3", "port=8443"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443"));
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("alpn=h2"));
   EXPECT_THAT(plan.rejected[0], HasSubstr("TLS over TCP"));
}

TEST(Plan, HasNoRouteWithoutARecord)
{
   const auto plan = plan_routes("example.com", found({}));

   EXPECT_THAT(plan.routes, IsEmpty());
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("TLS over TCP"));
}

TEST(Plan, RejectsATargetOtherThanTheHost)
{
   //
   // The certificate has to be valid for the origin (section 9.1), and anyhttp would check it
   // against what it connects to.
   //
   const auto plan = plan_routes("example.com", found({record(1, "cdn.example.net", {"alpn=h3"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("cdn.example.net"));
   EXPECT_TRUE(plan.h3_out_of_reach);
}

TEST(Plan, DoesNotCallItOutOfReachWithoutHttp3)
{
   const auto plan = plan_routes("example.com", found({record(1, "cdn.example.net", {"alpn=h2"})}));

   EXPECT_FALSE(plan.h3_out_of_reach);
}

TEST(Plan, UsesTheRecordItCanOfAMixedSet)
{
   //
   // As facebook.com has it.
   //
   const auto plan = plan_routes(
      "example.com",
      found({record(1, ".", {"alpn=h2,h3"}), record(2, "fallback.example.net", {"alpn=h2,h3"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("fallback.example.net"));
}

TEST(Plan, DoesNotTakeSomethingThatOnlyStartsWithH3)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3-29,h2"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

TEST(Plan, IgnoresWhatItDoesNotKnowUnlessItIsMandatory)
{
   //
   // key13930 is real, seen on one of the hosts in c-ares-async's list.
   //
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h3", "key13930=01", "ech=dead"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
}

TEST(Plan, TakesAnExplicitTargetThatIsTheHost)
{
   const auto plan = plan_routes("example.com", found({record(1, "EXAMPLE.com.", {"alpn=h3"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
}

TEST(Plan, TakesTheOwnNameOfARecordAtTheEndOfAnAliasAsThatName)
{
   //
   // "." is the owner name, section 2.5.2, and that is not the host any more.
   //
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h3"})}, "other.example", true));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_THAT(plan.rejected.front(), HasSubstr("other.example"));
}

TEST(Plan, SaysSoAboutTheTargetOfAnAlias)
{
   const auto plan = plan_routes("example.com", found({}, "other.example", true));

   EXPECT_THAT(plan.routes, IsEmpty());
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("other.example"));
}

TEST(Plan, HasNoDefaultSetToFallBackOnWithNoDefaultAlpn)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"no-default-alpn"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

TEST(Plan, IgnoresARecordWithAMandatoryKeyItDoesNotKnow)
{
   const auto plan =
      plan_routes("example.com",
                  found({record(1, ".", {"mandatory=alpn,key65280", "alpn=h3", "key65280=01"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_THAT(plan.rejected.front(), HasSubstr("key65280"));
}

TEST(Plan, IgnoresARecordThatRequiresEch)
{
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"mandatory=ech", "alpn=h3", "ech=dead"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

TEST(Plan, AcceptsMandatoryKeysItCanLeaveAlone)
{
   const auto plan = plan_routes(
      "example.com",
      found({record(1, ".", {"mandatory=alpn,ipv4hint", "alpn=h3", "ipv4hint=192.0.2.1"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
}

TEST(Plan, IgnoresARecordWithAnInvalidPort)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3", "port=x"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

TEST(Plan, PassesOnWhatWentWrongInTheLookup)
{
   auto records = found({});
   records.notes.push_back("HTTPS lookup failed");

   EXPECT_THAT(plan_routes("example.com", records).rejected.front(), "HTTPS lookup failed");
}
