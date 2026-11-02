//
// The whole program but its command line, against a server and a DNS server of its own on loopback.
// Nothing in here may ever reach for anything else.
//
#include "prober.hpp"
#include "site.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <spdlog/spdlog.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <format>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace http_probe;
using namespace std::chrono_literals;
using http_probe::test::Site;
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Pair;

namespace
{

std::vector<fake_dns_server::svc_param> h3_at(std::uint16_t port)
{
   return {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}},
           {ARES_SVCB_PARAM_PORT,
            {static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port & 0xff)}}};
}

const fake_dns_server::svc_param alpn_h2{ARES_SVCB_PARAM_ALPN, {2, 'h', '2'}};

class Probe : public testing::Test
{
protected:
   Config config(std::vector<std::string> hosts = {"localhost"})
   {
      return {.hosts = std::move(hosts),
              .timeout = 1000ms,
              .dns_server = site.dns_address(),
              .tls_ca_file = site.ca_file().string()};
   }

   void probe(Config config)
   {
      Prober prober(context.get_executor(), std::move(config),
                    [this](const Result& result) { results.push_back(result); });

      boost::asio::co_spawn(
         context,
         [&]() -> anyhttp::Task<void> {
            co_await prober.run();
            site.stop();
            //
            // For the tests with a QUIC endpoint that never answers. anyhttp does not close a
            // session whose connect() was cancelled: it goes on sending until its idle timeout of
            // 30 seconds, and the context does not run dry before that. To go once that is fixed.
            //
            if (abandon_quic_sessions)
               context.stop();
         },
         [](const std::exception_ptr& ex) {
            if (ex)
               std::rethrow_exception(ex);
         });
      context.run();
   }

   //
   // What the tests that do not get as far as run() still have to do.
   //
   void finish()
   {
      site.stop();
      context.run();
   }

   static int quiet()
   {
      spdlog::set_level(spdlog::level::off); // anyhttp logs through the default logger
      return 0;
   }

   static std::string notes(const Result& result)
   {
      std::string text;
      for (const auto& note : result.notes)
         text += note + "\n";
      return text;
   }

   int quiet_ = quiet(); // ahead of the server, which already has something to say when it starts
   boost::asio::io_context context;
   Site site{context.get_executor()};
   std::vector<Result> results;
   bool abandon_quic_sessions = false;
};

} // namespace

// =================================================================================================
// What it refuses
// =================================================================================================

TEST_F(Probe, RefusesToRunWithoutHosts)
{
   EXPECT_THROW(probe(config({})), std::invalid_argument);
   finish();
}

TEST_F(Probe, RefusesAConcurrencyOfZero)
{
   auto configuration = config();
   configuration.concurrency = 0;
   EXPECT_THROW(probe(std::move(configuration)), std::invalid_argument);
   finish();
}

TEST_F(Probe, RefusesAnythingButAHostName)
{
   for (const auto* host : {"https://localhost/", "localhost/x", "localhost:443", "*.localhost",
                            "user@localhost", "", "127.0.0.1", "::1"})
      EXPECT_THROW(probe(config({host})), std::invalid_argument) << host;
   finish();
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST(ReadHosts, TakesNamesAndTheHostsOfUrls)
{
   std::istringstream list("example.com\n"
                           "  www.example.com \r\n"
                           "\n"
                           "# a comment\n"
                           "https://www.example.net/some/path?query\n"
                           "http://example.org:8080/\n");
   std::vector<std::string> skipped;

   EXPECT_THAT(read_hosts(list, skipped),
               ElementsAre("example.com", "www.example.com", "www.example.net", "example.org"));
   EXPECT_THAT(skipped, IsEmpty());
}

TEST(ReadHosts, SkipsWhatIsNeitherAndSaysSo)
{
   std::istringstream list("example.com\nnot a host\nhttps://127.0.0.1/\nexample.net\n");
   std::vector<std::string> skipped;

   EXPECT_THAT(read_hosts(list, skipped), ElementsAre("example.com", "example.net"));
   EXPECT_THAT(skipped, ElementsAre("not a host", "https://127.0.0.1/"));
}

// =================================================================================================
// One request, and what is reported of it
// =================================================================================================

TEST_F(Probe, RequestsTheHostAtThePortOfItsRecord)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   const auto& result = results[0];
   EXPECT_EQ(result.outcome, Result::Outcome::answered) << notes(result);
   EXPECT_EQ(result.host, "localhost");
   EXPECT_EQ(result.status, 200);
   EXPECT_EQ(result.route, std::format("h3 localhost:{}", site.port()));
   EXPECT_EQ(result.body_size, 5u);
   EXPECT_THAT(result.fields, Contains(Pair("content-type", "text/html")));
   ASSERT_EQ(result.records.size(), 1u);
   EXPECT_THAT(to_text(result.records[0]), HasSubstr("alpn=h3"));

   ASSERT_EQ(site.requests().size(), 1u);
   EXPECT_EQ(site.requests()[0].authority, "localhost") << "the host, not where it went";
   EXPECT_THAT(site.requests()[0].user_agent, HasSubstr("http-probe/"));
}

TEST_F(Probe, RequestsNothingButTheRoot)
{
   //
   // No robots.txt either: that is about crawling, and this does not.
   //
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = R"(<a href="/a">a</a> <a href="https://localhost/b">b</a>)"});
   site.page("/a", {.body = "not asked for"});
   site.page("/b", {.body = "not asked for"});

   probe(config());

   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, ReportsARedirectAndDoesNotFollowIt)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.status = 302, .location = "/moved"});
   site.page("/moved", {.body = "not asked for"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 302);
   EXPECT_THAT(results[0].fields, Contains(Pair("location", "/moved")));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, ReportsAnErrorStatusAsAResponse)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.status = 503});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 503);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered);
}

TEST_F(Probe, RequestsEachHostOnce)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config({"localhost", "LOCALHOST", "localhost."}));

   EXPECT_EQ(results.size(), 1u);
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, GoesOnAfterAHostThatFails)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config({"nowhere.example.test", "localhost"}));

   ASSERT_EQ(results.size(), 2u);
   EXPECT_EQ(results[0].host, "nowhere.example.test");
   EXPECT_EQ(results[0].outcome, Result::Outcome::no_http3);
   EXPECT_EQ(results[1].host, "localhost");
   EXPECT_EQ(results[1].status, 200);
}

// =================================================================================================
// A list of hosts
// =================================================================================================

TEST_F(Probe, ConnectsToTheHostsWithHttp3AndToNoOthers)
{
   //
   // The other names exist, with records or without, and their addresses are the server's. It takes
   // no more than connecting to any of them to show up in its requests.
   //
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.dns().add_address("h2.example.test", "127.0.0.1");
   site.dns().add_https("h2.example.test", 1, ".", {alpn_h2});
   site.dns().add_address("plain.example.test", "127.0.0.1");
   site.page("/", {.body = "hello"});

   auto configuration = config({"h2.example.test", "plain.example.test", "localhost"});
   configuration.concurrency = 1;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 3u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::no_http3);
   EXPECT_EQ(results[1].outcome, Result::Outcome::no_http3);
   EXPECT_EQ(results[2].outcome, Result::Outcome::answered) << notes(results[2]);
   EXPECT_THAT(site.targets(), ElementsAre("/"));
   EXPECT_EQ(site.requests()[0].authority, "localhost");
}

TEST_F(Probe, ReportsEachHostOnceHoweverManyThereAre)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   std::vector<std::string> hosts{"localhost"};
   for (int i = 0; i < 100; ++i)
      hosts.push_back(std::format("host{}.example.test", i));

   auto configuration = config(hosts);
   configuration.concurrency = 7;
   probe(std::move(configuration));

   std::set<std::string> reported;
   for (const auto& result : results)
      reported.insert(result.host);
   EXPECT_EQ(results.size(), hosts.size());
   EXPECT_EQ(reported.size(), hosts.size());
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, DoesNotLetASlowHostHoldUpTheOthers)
{
   boost::asio::ip::udp::socket silent(context, {boost::asio::ip::udp::v6(), 0});
   abandon_quic_sessions = true;
   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));

   auto configuration = config({"localhost", "nowhere.example.test"});
   configuration.timeout = 200ms;
   configuration.concurrency = 2;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 2u);
   EXPECT_EQ(results[0].host, "nowhere.example.test");
   EXPECT_EQ(results[1].host, "localhost");
}

TEST_F(Probe, WorksOnOneHostAtATimeIfToldTo)
{
   boost::asio::ip::udp::socket silent(context, {boost::asio::ip::udp::v6(), 0});
   abandon_quic_sessions = true;
   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));

   auto configuration = config({"localhost", "nowhere.example.test"});
   configuration.timeout = 200ms;
   configuration.concurrency = 1;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 2u);
   EXPECT_EQ(results[0].host, "localhost");
   EXPECT_EQ(results[1].host, "nowhere.example.test");
}

// =================================================================================================
// How the HTTPS records decide about the connection
// =================================================================================================

TEST_F(Probe, SkipsARecordItCanNotUse)
{
   site.dns().add_https("localhost", 1, ".", {alpn_h2});
   site.dns().add_https("localhost", 2, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_THAT(notes(results[0]), HasSubstr("alpn=h2"));
   EXPECT_THAT(notes(results[0]), HasSubstr("TLS over TCP"));
}

TEST_F(Probe, FallsBackToTheNextRecordWhenAConnectionFails)
{
   //
   // Nothing answers on this port. The socket is ours for the length of the test, so that nothing
   // else will either.
   //
   boost::asio::ip::udp::socket silent(context, {boost::asio::ip::udp::v6(), 0});
   abandon_quic_sessions = true;

   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));
   site.dns().add_https("localhost", 2, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.timeout = 200ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h3 localhost:{}", site.port()));
   EXPECT_THAT(notes(results[0]), HasSubstr("timed out"));
}

TEST_F(Probe, FailsWithoutARecord)
{
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::no_http3);
   EXPECT_THAT(notes(results[0]), HasSubstr("TLS over TCP"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, DoesNotConnectToATargetOtherThanTheHost)
{
   //
   // The server would answer: it is the certificate that could not be right for this name.
   //
   site.dns().add_https("localhost", 1, "elsewhere.example.test", h3_at(site.port()));
   site.dns().add_address("elsewhere.example.test", "127.0.0.1");

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::out_of_reach);
   EXPECT_THAT(notes(results[0]), HasSubstr("elsewhere.example.test"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, RefusesACertificateItDoesNotTrust)
{
   Site other(context.get_executor()); // has a certificate of its own
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.tls_ca_file = other.ca_file().string();
   other.stop();
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("connect"));
   EXPECT_THAT(site.requests(), IsEmpty()) << "no request survives the handshake";
}

TEST_F(Probe, FollowsAnAliasToItsRecords)
{
   //
   // At the end of the chain it is the name of the alias's target that counts. So this is a route
   // only for an alias that leads back to the host, by way of another name.
   //
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.dns().add_https("alias.example.test", 1, "localhost", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_EQ(site.dns_queries("alias.example.test"), 1u);
}

TEST_F(Probe, RecognizesAnAliasLoop)
{
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.dns().add_https("alias.example.test", 0, "localhost");

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::no_http3);
   EXPECT_THAT(notes(results[0]), HasSubstr("too long or has a loop"));
}

TEST_F(Probe, IgnoresServiceRecordsNextToAnAlias)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::no_http3)
      << "the alias has no records of its own";
   EXPECT_TRUE(results[0].aliased);
   EXPECT_THAT(site.requests(), IsEmpty());
}

// =================================================================================================
// What can go wrong
// =================================================================================================

TEST_F(Probe, GivesUpOnAServerThatDoesNotAnswer)
{
   boost::asio::ip::udp::socket silent(context, {boost::asio::ip::udp::v6(), 0});
   abandon_quic_sessions = true;
   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));

   auto configuration = config();
   configuration.timeout = 100ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("timed out"));
}

TEST_F(Probe, ReportsANameThatDoesNotExist)
{
   probe(config({"nowhere.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::no_http3);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, ReportsAFailedLookup)
{
   site.dns().set_rcode(ARES_RCODE_SERVFAIL);
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed) << "not the same as no HTTP/3";
   EXPECT_THAT(notes(results[0]), HasSubstr("HTTPS lookup"));
   EXPECT_THAT(site.requests(), IsEmpty());
}
