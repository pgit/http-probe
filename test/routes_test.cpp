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
   EXPECT_THAT(routes(plan_routes("example.com", found({}), {}, 8443)),
               ElementsAre("h2 example.com:8443"));
}

TEST(Plan, RecordsThatDoNotSayGoToTheDefaultPort)
{
   EXPECT_THAT(routes(plan_routes("example.com", found({record(1, ".", {"alpn=h2"})}), {}, 8443)),
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

TEST(Plan, PassesOnWhatWentWrongInTheLookup)
{
   auto records = found({});
   records.notes.push_back("HTTPS lookup failed");

   EXPECT_THAT(plan_routes("example.com", records).rejected.front(), "HTTPS lookup failed");
}

// -------------------------------------------------------------------------------------------------
// --h3, --h2 and --h1
// -------------------------------------------------------------------------------------------------

namespace
{
const Protocols h3{.h3 = true};
const Protocols h2{.h2 = true};
const Protocols h1{.h1 = true};
} // namespace

TEST(Protocols, SpeaksThoseGivenOrAllWithoutAny)
{
   EXPECT_TRUE(Protocols{}.speaks(anyhttp::Protocol::h3));
   EXPECT_TRUE(Protocols{}.speaks(anyhttp::Protocol::h1));
   EXPECT_TRUE(h2.speaks(anyhttp::Protocol::h2));
   EXPECT_FALSE(h2.speaks(anyhttp::Protocol::h1));
   EXPECT_FALSE(h3.speaks_tcp());
   EXPECT_TRUE((Protocols{.h3 = true, .h1 = true}).speaks_tcp());
}

TEST(Protocols, SaysWhichTheyAre)
{
   EXPECT_EQ(h2.to_string(), "h2");
   EXPECT_EQ((Protocols{.h3 = true, .h1 = true}).to_string(), "h3 or h1");
   EXPECT_EQ(Protocols{}.to_string(), "h3 or h2 or h1");
}

TEST(Plan, SpeaksOnlyTheProtocolsGiven)
{
   const auto records = found({record(1, ".", {"alpn=h3,h2"})});

   EXPECT_THAT(routes(plan_routes("example.com", records, h3)), ElementsAre("h3 example.com:443"));
   EXPECT_THAT(routes(plan_routes("example.com", records, h2)), ElementsAre("h2 example.com:443"));
   EXPECT_THAT(routes(plan_routes("example.com", records, h1)), ElementsAre("h1 example.com:443"))
      << "the default set";
   EXPECT_THAT(routes(plan_routes("example.com", records, {.h3 = true, .h1 = true})),
               ElementsAre("h3 example.com:443", "h1 example.com:443"));
}

TEST(Plan, LeavesTheChoiceOverTcpToTheServerWithBothOfThem)
{
   const auto plan =
      plan_routes("example.com", found({record(1, ".", {"alpn=h2"})}), {.h2 = true, .h1 = true});

   EXPECT_THAT(routes(plan), ElementsAre("h2 example.com:443"));
}

TEST(Plan, GoesOverTcpWithoutARecordWithTheProtocolGiven)
{
   EXPECT_THAT(routes(plan_routes("example.com", found({}), h2)),
               ElementsAre("h2 example.com:443"));
   EXPECT_THAT(routes(plan_routes("example.com", found({}), h1)),
               ElementsAre("h1 example.com:443"));
   EXPECT_THAT(routes(plan_routes("example.com", found({}), {.h3 = true, .h1 = true})),
               ElementsAre("h1 example.com:443"));
}

TEST(Plan, LeavesOutAHostWithoutARecordForH3Alone)
{
   const auto plan = plan_routes("example.com", found({}), h3);

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_TRUE(plan.left_out);
   EXPECT_FALSE(plan.out_of_reach);
   EXPECT_THAT(plan.rejected, ElementsAre("no usable HTTPS record offers h3"));
}

TEST(Plan, LeavesOutAHostWhoseRecordsOfferNoneOfThem)
{
   const auto plan = plan_routes("example.com",
                                 found({record(1, ".", {"alpn=h3", "no-default-alpn"}),
                                        record(2, ".", {"alpn=h3-29,h2", "no-default-alpn"})}),
                                 h1);

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_TRUE(plan.left_out);
   EXPECT_FALSE(plan.out_of_reach);
   EXPECT_THAT(plan.rejected, ElementsAre(HasSubstr("offers no h1"), HasSubstr("offers no h1")));
}

TEST(Plan, SaysWhichRecordsOfferNoneOfThem)
{
   const auto plan = plan_routes(
      "example.com", found({record(1, ".", {"alpn=h2"}), record(2, ".", {"alpn=h3", "port=8443"})}),
      h3);

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443"));
   EXPECT_THAT(plan.rejected, ElementsAre("HTTPS 1 . alpn=h2: offers no h3"));
}

TEST(Plan, LeavesOutOnlyByWhatTheRecordsOfferWhereverTheyPoint)
{
   //
   // A record that offers the protocol, on a target that can't be used, is out of reach as ever.
   //
   const auto offered =
      plan_routes("example.com", found({record(1, "other.example", {"alpn=h3"})}), h3);
   EXPECT_TRUE(offered.out_of_reach);
   EXPECT_FALSE(offered.left_out);

   const auto not_offered = plan_routes(
      "example.com", found({record(1, "other.example", {"alpn=h2", "no-default-alpn"})}), h3);
   EXPECT_TRUE(not_offered.left_out);
   EXPECT_FALSE(not_offered.out_of_reach);
}

TEST(Plan, TakesARecordThatHasToBeIgnoredForNoRecordWithTheProtocolsGiven)
{
   for (const auto& params : std::vector<std::vector<std::string>>{
           {"alpn=h3", "mandatory=ech", "ech=dead"}, {"alpn=h3", "port=none"}})
   {
      const auto records = found({record(1, ".", params)});

      EXPECT_TRUE(plan_routes("example.com", records, h3).left_out);
      EXPECT_THAT(routes(plan_routes("example.com", records, h2)),
                  ElementsAre("h2 example.com:443"));
   }
}

TEST(Plan, LeavesOutAnUnavailableServiceWithoutTcp)
{
   auto records = found({});
   records.unavailable = true;

   EXPECT_TRUE(plan_routes("example.com", records, h3).left_out);
   EXPECT_TRUE(plan_routes("example.com", records, h1).out_of_reach);
   EXPECT_FALSE(plan_routes("example.com", records, h1).left_out);
}

TEST(Plan, LeavesOutTheTargetOfAnAliasWithoutTcp)
{
   const auto records = found({}, "other.example", true);

   EXPECT_TRUE(plan_routes("example.com", records, h3).left_out);
   EXPECT_TRUE(plan_routes("example.com", records, h2).out_of_reach);
}

TEST(Plan, LeavesOutNeitherANameThatDoesNotExistNorOneThatWasNotLookedUp)
{
   auto not_found = found({});
   not_found.not_found = true;
   auto lookup_failed = found({});
   lookup_failed.lookup_failed = true;

   for (const auto& records : {not_found, lookup_failed})
   {
      const auto plan = plan_routes("example.com", records, h3);
      EXPECT_FALSE(plan.left_out);
      EXPECT_FALSE(plan.out_of_reach);
   }
}
