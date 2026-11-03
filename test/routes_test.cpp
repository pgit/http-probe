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

TEST(Plan, ConnectsOverHttp3FirstAndThenOverTcp)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3,h2"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443", "h2 example.com:443"));
}

TEST(Plan, TakesTheDefaultSetIntoAccount)
{
   //
   // "alpn=h3" alone does not mean HTTP/3 alone: the default set is still there (section 7.1.2).
   //
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443", "h1 example.com:443"));
}

TEST(Plan, HasNoTcpWithNoDefaultAlpn)
{
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h3", "no-default-alpn"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
}

TEST(Plan, ConnectsOverTcpWhereTheRecordOffersNoHttp3)
{
   EXPECT_THAT(routes(plan_routes("example.com", found({record(1, ".", {"alpn=h2"})}))),
               ElementsAre("h2 example.com:443"));
   EXPECT_THAT(routes(plan_routes("example.com", found({record(1, ".", {"alpn=http/1.1"})}))),
               ElementsAre("h1 example.com:443"));
   EXPECT_THAT(routes(plan_routes("example.com", found({record(1, ".")}))),
               ElementsAre("h1 example.com:443"));
}

TEST(Plan, GoesToThePortOfTheRecord)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3", "port=8443"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443", "h1 example.com:8443"));
}

TEST(Plan, TriesTheLowestPriorityFirst)
{
   const auto plan =
      plan_routes("example.com", found({record(3, ".", {"alpn=h3", "no-default-alpn", "port=3"}),
                                        record(1, ".", {"alpn=h3", "no-default-alpn", "port=1"}),
                                        record(2, ".", {"alpn=h2", "no-default-alpn", "port=2"})}));

   EXPECT_THAT(routes(plan),
               ElementsAre("h3 example.com:1", "h2 example.com:2", "h3 example.com:3"));
}

TEST(Plan, DoesNotTryTheSameThingTwice)
{
   //
   // Records that differ only in what is of no use here, like their address hints.
   //
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h3", "ipv4hint=192.0.2.1"}),
                                        record(2, ".", {"alpn=h3", "ipv4hint=192.0.2.2"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443", "h1 example.com:443"));
}

TEST(Plan, GoesOverTcpToTheDefaultPortWithoutARecord)
{
   EXPECT_THAT(routes(plan_routes("example.com", found({}))), ElementsAre("h2 example.com:443"));
   EXPECT_THAT(plan_routes("example.com", found({})).rejected, IsEmpty()) << "nothing odd about it";
   EXPECT_THAT(routes(plan_routes("example.com", found({}), 8443)),
               ElementsAre("h2 example.com:8443"));
}

TEST(Plan, RecordsThatDoNotSayGoToTheDefaultPort)
{
   EXPECT_THAT(routes(plan_routes("example.com", found({record(1, ".", {"alpn=h2"})}), 8443)),
               ElementsAre("h2 example.com:8443"));
}

TEST(Plan, HasNoRouteForANameThatDoesNotExist)
{
   auto records = found({});
   records.not_found = true;

   const auto plan = plan_routes("example.com", records);

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_FALSE(plan.out_of_reach);
}

TEST(Plan, HasNoRouteAfterALookupThatFailed)
{
   auto records = found({});
   records.lookup_failed = true;

   const auto plan = plan_routes("example.com", records);

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_FALSE(plan.out_of_reach);
}

TEST(Plan, TakesAHostsWordForItThatTheServiceIsNotAvailable)
{
   auto records = found({});
   records.unavailable = true;

   const auto plan = plan_routes("example.com", records);

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_TRUE(plan.out_of_reach);
}

TEST(Plan, RejectsATargetOtherThanTheHost)
{
   //
   // The certificate has to be valid for the origin (section 9.1), and anyhttp would check it
   // against what it connects to.
   //
   for (const auto* alpn : {"alpn=h3", "alpn=h2"})
   {
      const auto plan = plan_routes("example.com", found({record(1, "cdn.example.net", {alpn})}));

      EXPECT_THAT(plan.routes, IsEmpty());
      ASSERT_EQ(plan.rejected.size(), 1u);
      EXPECT_THAT(plan.rejected[0], HasSubstr("cdn.example.net"));
      EXPECT_TRUE(plan.out_of_reach);
   }
}

TEST(Plan, UsesTheRecordItCanOfAMixedSet)
{
   //
   // As facebook.com has it.
   //
   const auto plan = plan_routes(
      "example.com",
      found({record(1, ".", {"alpn=h2,h3"}), record(2, "fallback.example.net", {"alpn=h2,h3"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443", "h2 example.com:443"));
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("fallback.example.net"));
   EXPECT_FALSE(plan.out_of_reach);
}

TEST(Plan, DoesNotTakeSomethingThatOnlyStartsWithH3)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3-29,h2"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
}

TEST(Plan, IgnoresWhatItDoesNotKnowUnlessItIsMandatory)
{
   //
   // key13930 is real, seen on one of the hosts in c-ares-async's list.
   //
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h3", "key13930=01", "ech=dead"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443", "h1 example.com:443"));
}

TEST(Plan, TakesAnExplicitTargetThatIsTheHost)
{
   const auto plan = plan_routes("example.com", found({record(1, "EXAMPLE.com.", {"alpn=h2"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
}

TEST(Plan, TakesTheOwnNameOfARecordAtTheEndOfAnAliasAsThatName)
{
   //
   // "." is the owner name, section 2.5.2, and that is not the host any more.
   //
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h3"})}, "other.example", true));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_TRUE(plan.out_of_reach);
   EXPECT_THAT(plan.rejected.front(), HasSubstr("other.example"));
}

TEST(Plan, SaysSoAboutTheTargetOfAnAlias)
{
   const auto plan = plan_routes("example.com", found({}, "other.example", true));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_TRUE(plan.out_of_reach);
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("other.example"));
}

TEST(Plan, TreatsARecordWithNoProtocolItSpeaksAsNoRecord)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"no-default-alpn"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
   EXPECT_THAT(plan.rejected.front(), HasSubstr("no protocol"));
}

TEST(Plan, TreatsARecordWithAMandatoryKeyItDoesNotKnowAsNoRecord)
{
   //
   // Section 8: what is left is the name without a record, and that goes for all that follow.
   //
   const auto plan =
      plan_routes("example.com",
                  found({record(1, ".", {"mandatory=alpn,key65280", "alpn=h3", "key65280=01"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
   EXPECT_THAT(plan.rejected.front(), HasSubstr("key65280"));
}

TEST(Plan, TreatsARecordThatRequiresEchAsNoRecord)
{
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"mandatory=ech", "alpn=h3", "ech=dead"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
}

TEST(Plan, AcceptsMandatoryKeysItCanLeaveAlone)
{
   const auto plan = plan_routes(
      "example.com",
      found({record(1, ".", {"mandatory=alpn,ipv4hint", "alpn=h3", "ipv4hint=192.0.2.1"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443", "h1 example.com:443"));
}

TEST(Plan, TreatsARecordWithAnInvalidPortAsNoRecord)
{
   const auto plan = plan_routes("example.com", found({record(1, ".", {"alpn=h3", "port=x"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
}

TEST(HasH3Record, IsTrueForARecordThatOffersIt)
{
   EXPECT_TRUE(has_h3_record(found({record(1, ".", {"alpn=h3"})})));
   EXPECT_TRUE(has_h3_record(found({record(1, ".", {"alpn=h2,h3"})})));
   EXPECT_TRUE(has_h3_record(found({record(1, ".", {"alpn=h2"}), record(2, ".", {"alpn=h3"})})));
}

TEST(HasH3Record, DoesNotCareWhereTheRecordPoints)
{
   EXPECT_TRUE(has_h3_record(found({record(1, "other.example", {"alpn=h3"})})));
}

TEST(HasH3Record, IsFalseForAnythingElse)
{
   EXPECT_FALSE(has_h3_record(found({})));
   EXPECT_FALSE(has_h3_record(found({record(1, ".")})));
   EXPECT_FALSE(has_h3_record(found({record(1, ".", {"alpn=h2"})})));
   EXPECT_FALSE(has_h3_record(found({record(1, ".", {"alpn=h3-29,h2"})})));
}

TEST(HasH3Record, IgnoresARecordThatHasToBeIgnored)
{
   EXPECT_FALSE(has_h3_record(found({record(1, ".", {"alpn=h3", "mandatory=ech", "ech=dead"})})));
   EXPECT_FALSE(has_h3_record(found({record(1, ".", {"alpn=h3", "port=none"})})));
}

TEST(Plan, PassesOnWhatWentWrongInTheLookup)
{
   auto records = found({});
   records.notes.push_back("HTTPS lookup failed");

   EXPECT_THAT(plan_routes("example.com", records).rejected.front(), "HTTPS lookup failed");
}
