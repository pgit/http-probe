//
// What the records of a name mean for how to connect, without any I/O.
//
#include "routes.hpp"

#include <boost/url/url.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace crawler;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

namespace
{

Origin origin(std::string_view url) { return *origin_of(boost::urls::url(url)); }

c_ares_async::https_record record(unsigned short priority, std::string target,
                                  std::vector<std::string> params = {})
{
   return {.priority = priority, .target = std::move(target), .params = std::move(params)};
}

ServiceRecords found(std::string owner, std::vector<c_ares_async::https_record> records,
                     bool aliased = false)
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

TEST(Origin, TakesThePortOfTheScheme)
{
   EXPECT_EQ(origin("http://example.com/x").port, 80);
   EXPECT_EQ(origin("https://example.com/x").port, 443);
   EXPECT_EQ(origin("https://example.com:8443/x").port, 8443);
}

TEST(Origin, ComparesHostsTheWayDnsDoes)
{
   EXPECT_EQ(origin("https://EXAMPLE.com./").host, "example.com");
}

TEST(Origin, KnowsAnAddressFromAName)
{
   EXPECT_FALSE(origin("http://example.com/").is_address);
   EXPECT_TRUE(origin("http://127.0.0.1/").is_address);
   EXPECT_TRUE(origin("http://[::1]/").is_address);
}

TEST(Origin, HasNoneForOtherSchemes)
{
   EXPECT_FALSE(origin_of(boost::urls::url("ftp://example.com/")));
   EXPECT_FALSE(origin_of(boost::urls::url("mailto:someone@example.com")));
}

TEST(QueryName, IsTheHostOnTheDefaultPort)
{
   EXPECT_EQ(https_query_name(origin("https://example.com/")), "example.com");
   EXPECT_EQ(https_query_name(origin("https://example.com:443/")), "example.com");
}

TEST(QueryName, HasAPortPrefixOnAnyOther)
{
   EXPECT_EQ(https_query_name(origin("https://example.com:8443/")), "_8443._https.example.com");
}

TEST(QueryName, IsThatOfTheHttpsFormForHttp)
{
   EXPECT_EQ(https_query_name(origin("http://example.com/")), "example.com");
   EXPECT_EQ(https_query_name(origin("http://example.com:80/")), "example.com");
   EXPECT_EQ(https_query_name(origin("http://example.com:8080/")), "_8080._https.example.com");
}

// -------------------------------------------------------------------------------------------------

TEST(Plan, ConnectsOverHttp3WhereTheRecordOffersIt)
{
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(1, ".", {"alpn=h3,h2"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
   EXPECT_EQ(plan.origin.scheme, "https");
}

TEST(Plan, GoesToThePortOfTheRecord)
{
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(1, ".", {"alpn=h3", "port=8443"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443"));
   EXPECT_EQ(plan.origin.port, 443) << "requests still go to the origin";
}

TEST(Plan, TriesTheLowestPriorityFirst)
{
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(3, ".", {"alpn=h3", "port=3"}),
                                                       record(1, ".", {"alpn=h3", "port=1"}),
                                                       record(2, ".", {"alpn=h3", "port=2"})}));

   EXPECT_THAT(routes(plan),
               ElementsAre("h3 example.com:1", "h3 example.com:2", "h3 example.com:3"));
}

TEST(Plan, RejectsARecordWithoutHttp3)
{
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(1, ".", {"alpn=h2"}),
                                                       record(2, ".", {"alpn=h3", "port=8443"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443"));
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("alpn=h2"));
   EXPECT_THAT(plan.rejected[0], HasSubstr("TLS over TCP"));
}

TEST(Plan, HasNoRouteToAnHttpsOriginWithoutARecord)
{
   const auto plan = plan_routes(origin("https://example.com/"), found("example.com", {}));

   EXPECT_THAT(plan.routes, IsEmpty());
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("TLS over TCP"));
}

TEST(Plan, RejectsATargetOtherThanTheOrigin)
{
   //
   // The certificate has to be valid for the origin (section 9.1), and anyhttp would check it
   // against what it connects to.
   //
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(1, "cdn.example.net", {"alpn=h3"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("cdn.example.net"));
}

TEST(Plan, TakesAnExplicitTargetThatIsTheOrigin)
{
   const auto plan =
      plan_routes(origin("https://example.com:8443/"),
                  found("_8443._https.example.com", {record(1, "EXAMPLE.com.", {"alpn=h3"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:8443"));
}

TEST(Plan, TakesTheOwnNameOfAPortPrefixedRecordLiterally)
{
   //
   // "." is the owner name, section 2.5.2, and that includes the prefix.
   //
   const auto plan = plan_routes(origin("https://example.com:8443/"),
                                 found("_8443._https.example.com", {record(1, ".", {"alpn=h3"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
   ASSERT_EQ(plan.rejected.size(), 1u);
   EXPECT_THAT(plan.rejected[0], HasSubstr("_8443._https.example.com"));
}

TEST(Plan, HasNoDefaultSetToFallBackOnWithNoDefaultAlpn)
{
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(1, ".", {"no-default-alpn"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

TEST(Plan, IgnoresARecordWithAMandatoryKeyItDoesNotKnow)
{
   const auto plan =
      plan_routes(origin("https://example.com/"),
                  found("example.com",
                        {record(1, ".", {"mandatory=alpn,key65280", "alpn=h3", "key65280=01"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_THAT(plan.rejected.front(), HasSubstr("key65280"));
}

TEST(Plan, IgnoresARecordThatRequiresEch)
{
   const auto plan =
      plan_routes(origin("https://example.com/"),
                  found("example.com", {record(1, ".", {"mandatory=ech", "alpn=h3", "ech=dead"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

TEST(Plan, AcceptsMandatoryKeysItCanLeaveAlone)
{
   const auto plan = plan_routes(
      origin("https://example.com/"),
      found("example.com",
            {record(1, ".", {"mandatory=alpn,ipv4hint", "alpn=h3", "ipv4hint=192.0.2.1"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
}

TEST(Plan, IgnoresARecordWithAnInvalidPort)
{
   const auto plan = plan_routes(origin("https://example.com/"),
                                 found("example.com", {record(1, ".", {"alpn=h3", "port=x"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
}

// -------------------------------------------------------------------------------------------------

TEST(Plan, ConnectsInTheClearToAnHttpOriginWithoutARecord)
{
   const auto plan =
      plan_routes(origin("http://example.com:8080/"), found("_8080._https.example.com", {}));

   EXPECT_THAT(routes(plan), ElementsAre("h1 example.com:8080"));
   EXPECT_EQ(plan.origin.scheme, "http");
}

TEST(Plan, UpgradesAnHttpOriginWithARecord)
{
   const auto plan = plan_routes(origin("http://example.com/"),
                                 found("example.com", {record(1, ".", {"alpn=h3"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h3 example.com:443"));
   EXPECT_EQ(plan.origin.scheme, "https");
   EXPECT_EQ(plan.origin.port, 443);
}

TEST(Plan, DoesNotFallBackToCleartextAfterAnUpgrade)
{
   const auto plan = plan_routes(origin("http://example.com/"),
                                 found("example.com", {record(1, ".", {"alpn=h2"})}));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_EQ(plan.origin.scheme, "https");
}

TEST(Plan, DoesNotUpgradeForAnIncompatibleRecord)
{
   //
   // Section 9.5: only a compatible record, or an alias.
   //
   const auto plan = plan_routes(
      origin("http://example.com/"),
      found("example.com", {record(1, ".", {"mandatory=key65280", "alpn=h3", "key65280=01"})}));

   EXPECT_THAT(routes(plan), ElementsAre("h1 example.com:80"));
   EXPECT_EQ(plan.origin.scheme, "http");
}

TEST(Plan, UpgradesAnHttpOriginWithAnAlias)
{
   const auto plan = plan_routes(origin("http://example.com/"),
                                 found("alias.example.net", {}, /* aliased */ true));

   EXPECT_THAT(plan.routes, IsEmpty());
   EXPECT_EQ(plan.origin.scheme, "https");
   EXPECT_THAT(plan.rejected.front(), HasSubstr("alias.example.net"));
}
