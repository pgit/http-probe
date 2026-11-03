//
// The whole program but its command line, against a server and a DNS server of its own on loopback.
// Nothing in here may ever reach for anything else.
//
#include "prober.hpp"
#include "site.hpp"

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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

namespace boost::corosio
{

/// What a failed expectation shows of an endpoint: Corosio has no output operator for one.
inline void PrintTo(const endpoint& endpoint, std::ostream* out)
{
   *out << endpoint.address() << ':' << endpoint.port();
}

} // namespace boost::corosio

namespace
{

corosio::ip_address make_address(std::string_view text)
{
   auto [ec, address] = corosio::make_ip_address(text);
   if (ec)
      throw std::system_error(ec, std::string(text));
   return address;
}

fake_dns_server::svc_param port_param(std::uint16_t port)
{
   return {ARES_SVCB_PARAM_PORT,
           {static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port & 0xff)}};
}

const fake_dns_server::svc_param alpn_h3{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}};
const fake_dns_server::svc_param alpn_h2{ARES_SVCB_PARAM_ALPN, {2, 'h', '2'}};
const fake_dns_server::svc_param alpn_h1{ARES_SVCB_PARAM_ALPN,
                                         {8, 'h', 't', 't', 'p', '/', '1', '.', '1'}};
const fake_dns_server::svc_param no_default_alpn{ARES_SVCB_PARAM_NO_DEFAULT_ALPN, {}};

//
// One protocol and no other, for a test to know which one it is getting. A record with nothing but
// an "alpn" also has the default set, and that means trying HTTP/1.1 as well.
//
std::vector<fake_dns_server::svc_param> only(fake_dns_server::svc_param alpn, std::uint16_t port)
{
   return {std::move(alpn), no_default_alpn, port_param(port)};
}

std::vector<fake_dns_server::svc_param> h3_at(std::uint16_t port) { return only(alpn_h3, port); }
std::vector<fake_dns_server::svc_param> h2_at(std::uint16_t port) { return only(alpn_h2, port); }
std::vector<fake_dns_server::svc_param> h1_at(std::uint16_t port) { return only(alpn_h1, port); }

class Probe : public testing::Test
{
protected:
   Config config(std::vector<std::string> hosts = {"localhost"})
   {
      return {.hosts = std::move(hosts),
              .timeout = 1000ms,
              .dns_server = site.dns_address(),
              .tls_ca_file = site.ca_file().string(),
              .default_port = site.port()};
   }

   void probe(Config config)
   {
      Prober prober(context.get_executor(), std::move(config),
                    [this](const Result& result) { results.push_back(result); });

      //
      // In a variable: the coroutine refers to what the lambda captured, for as long as it runs.
      //
      const auto run = [&]() -> capy::task<> {
         co_await prober.run();
         site.stop();
      };

      std::exception_ptr failed;
      capy::run_async(
         context.get_executor(), []() noexcept {},
         [&](std::exception_ptr ex) noexcept {
            failed = ex;
            context.stop();
         })(run());
      context.run();
      if (failed)
         std::rethrow_exception(failed);
   }

   //
   // A port where nothing is ever going to answer, on any address that "localhost" may be.
   //
   corosio::udp_socket silent_udp_socket()
   {
      corosio::udp_socket socket(context);
      if (const auto ec = socket.open(corosio::family::v6))
         throw std::system_error(ec, "open");
      if (const auto ec = socket.bind({corosio::ipv6_address::any(), 0}))
         throw std::system_error(ec, "bind");
      return socket;
   }

   corosio::tcp_acceptor silent_tcp_acceptor()
   {
      return corosio::tcp_acceptor(context, {corosio::ipv6_address::any(), 0});
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
   corosio::io_context context;
   Site site{context.get_executor()};
   std::vector<Result> results;
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
   EXPECT_EQ(results[0].outcome, Result::Outcome::not_found);
   EXPECT_EQ(results[1].host, "localhost");
   EXPECT_EQ(results[1].status, 200);
}

// =================================================================================================
// A list of hosts
// =================================================================================================

TEST_F(Probe, DoesNotConnectToANameThatDoesNotExist)
{
   //
   // Which would also take the lookup of its address to the resolver of the system, and away from
   // loopback, for every test that has a name like this.
   //
   site.dns().add_address("localhost", "127.0.0.1");
   site.page("/", {.body = "hello"});

   auto configuration = config({"nowhere.example.test", "localhost"});
   configuration.concurrency = 1;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 2u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::not_found);
   EXPECT_EQ(results[1].outcome, Result::Outcome::answered) << notes(results[1]);
   EXPECT_THAT(site.targets(), ElementsAre("/"));
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
   auto silent = silent_udp_socket();
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
   auto silent = silent_udp_socket();
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

TEST_F(Probe, SpeaksHttp2OverTlsWhereTheRecordSaysSo)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h2 localhost:{}", site.port()));
   EXPECT_EQ(results[0].body_size, 5u);
   ASSERT_EQ(site.requests().size(), 1u);
   EXPECT_EQ(site.requests()[0].authority, "localhost");
   EXPECT_THAT(site.requests()[0].user_agent, HasSubstr("http-probe/"));
}

TEST_F(Probe, SpeaksHttp1OverTlsWhereTheRecordSaysSo)
{
   site.dns().add_https("localhost", 1, ".", h1_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h1 localhost:{}", site.port()));
   EXPECT_EQ(results[0].body_size, 5u);
   ASSERT_EQ(site.requests().size(), 1u);
   EXPECT_THAT(site.requests()[0].authority, HasSubstr("localhost"));
}

TEST_F(Probe, PrefersHttp3OfWhatARecordOffers)
{
   site.dns().add_https("localhost", 1, ".", {alpn_h3, port_param(site.port())});
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].route, std::format("h3 localhost:{}", site.port())) << notes(results[0]);
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, GoesOverTcpToTheDefaultPortWithoutARecord)
{
   site.dns().add_address("localhost", "127.0.0.1");
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h2 localhost:{}", site.port()));
   EXPECT_THAT(results[0].records, IsEmpty());
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, DoesNotFollowARedirectOverHttp2)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.status = 302, .location = "/moved"});
   site.page("/moved", {.body = "not asked for"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 302) << notes(results[0]);
   EXPECT_THAT(results[0].fields, Contains(Pair("location", "/moved")));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, DoesNotFollowARedirectOverHttp1)
{
   site.dns().add_https("localhost", 1, ".", h1_at(site.port()));
   site.page("/", {.status = 302, .location = "/moved"});
   site.page("/moved", {.body = "not asked for"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 302) << notes(results[0]);
   // As it came, and only HTTP/2 and HTTP/3 have field names in lower case on the wire.
   EXPECT_THAT(results[0].fields, Contains(Pair("Location", "/moved")));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, FallsBackToTheNextRecordWhenAConnectionFails)
{
   //
   // Nothing answers on this port. The socket is ours for the length of the test, so that nothing
   // else will either.
   //
   auto silent = silent_udp_socket();

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

TEST_F(Probe, FallsBackFromHttp3ToTcpWhenAConnectionFails)
{
   auto silent = silent_udp_socket();

   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));
   site.dns().add_https("localhost", 2, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.timeout = 200ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h2 localhost:{}", site.port()));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, TriesNoOtherRouteOnceARequestHasGoneOut)
{
   //
   // The server has it, it is just not done with it. Everything after this would be a second
   // request to the same host: over HTTP/1.1 on the same record, and over HTTP/2 on the next.
   //
   site.dns().add_https("localhost", 1, ".", {alpn_h3, port_param(site.port())});
   site.dns().add_https("localhost", 2, ".", h2_at(site.port()));
   site.page("/", {.body = "too late", .delay = 400ms});

   auto configuration = config();
   configuration.timeout = 100ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("timed out"));
   EXPECT_THAT(results[0].error, HasSubstr("h3"));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, TriesNoOtherRouteOnceARequestHasGoneOutOverTcp)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.dns().add_https("localhost", 2, ".", h1_at(site.port()));
   site.page("/", {.body = "too late", .delay = 400ms});

   auto configuration = config();
   configuration.timeout = 100ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(site.targets(), ElementsAre("/"));
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

TEST_F(Probe, SaysWhichAddressAnswered)
{
   site.dns().add_https("localhost", 1, ".", {alpn_h3, port_param(site.port())});
   site.dns().add_https("localhost", 2, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   ASSERT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_TRUE(make_address(results[0].address).is_loopback()) << results[0].address;
}

TEST_F(Probe, SaysWhichAddressAnsweredOverTcp)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   ASSERT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_TRUE(make_address(results[0].address).is_loopback()) << results[0].address;
}

//
// No other resolver knows this name, so that getting as far as the certificate, which is not for
// this name, is what it takes to know that the addresses came from the DNS server.
//
TEST_F(Probe, ConnectsToTheAddressInDnsAndSaysWhichOne)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("site.example.test", "127.0.0.1");

   probe(config({"site.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_EQ(results[0].address, "127.0.0.1");
   EXPECT_THAT(results[0].error, HasSubstr("connect"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, ConnectsToAnIPv6AddressInDns)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("site.example.test", "::1");

   probe(config({"site.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_EQ(results[0].address, "::1");
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, TriesTheNextAddressWhenAConnectionFails)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("site.example.test", "127.0.0.1");
   site.dns().add_address("site.example.test", "::1");

   probe(config({"site.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(notes(results[0]), HasSubstr("at 127.0.0.1"));
   EXPECT_THAT(notes(results[0]), HasSubstr("at ::1"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

//
// This one would hold without prefer_ipv6(), as c-ares puts ::1 ahead of 127.0.0.1 by itself. An
// address that it would put behind IPv4 is not on loopback, so the order is tested on its own
// below.
//
TEST_F(Probe, TriesIPv6First)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("site.example.test", "127.0.0.1");
   site.dns().add_address("site.example.test", "::1");

   probe(config({"site.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   const auto text = notes(results[0]);
   ASSERT_THAT(text, HasSubstr("at ::1"));
   ASSERT_THAT(text, HasSubstr("at 127.0.0.1"));
   EXPECT_LT(text.find("at ::1"), text.find("at 127.0.0.1"));
   EXPECT_EQ(results[0].address, "127.0.0.1"); // the last one tried
}

corosio::endpoint at(const char* address) { return {make_address(address), 443}; }

TEST(PreferIpv6, MovesIPv6ToTheFrontAndKeepsTheOrderWithinEachFamily)
{
   std::vector addresses{at("192.0.2.1"), at("2001:db8::1"), at("192.0.2.2"), at("2001:db8::2")};

   prefer_ipv6(addresses);

   EXPECT_THAT(addresses,
               ElementsAre(at("2001:db8::1"), at("2001:db8::2"), at("192.0.2.1"), at("192.0.2.2")));
}

TEST(PreferIpv6, LeavesASingleFamilyAlone)
{
   std::vector addresses{at("192.0.2.2"), at("192.0.2.1")};

   prefer_ipv6(addresses);

   EXPECT_THAT(addresses, ElementsAre(at("192.0.2.2"), at("192.0.2.1")));
}

TEST_F(Probe, DoesNotConnectWithoutAnAddress)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));

   probe(config({"site.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
   EXPECT_THAT(results[0].address, IsEmpty());
   EXPECT_THAT(notes(results[0]), HasSubstr("address lookup"));
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

TEST_F(Probe, RefusesACertificateItDoesNotTrustOverTcp)
{
   Site other(context.get_executor());
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
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
   site.page("/", {.body = "hello"});

   probe(config());

   //
   // As if it had no record at all (section 3.1), which leaves the default port.
   //
   ASSERT_EQ(results.size(), 1u);
   EXPECT_THAT(notes(results[0]), HasSubstr("too long or has a loop"));
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h2 localhost:{}", site.port()));
}

TEST_F(Probe, IgnoresServiceRecordsNextToAnAlias)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::out_of_reach)
      << "the alias has no records of its own";
   EXPECT_TRUE(results[0].aliased);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, TakesAHostsWordForItThatTheServiceIsNotAvailable)
{
   site.dns().add_https("localhost", 0, ".");
   site.page("/", {.body = "hello"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::out_of_reach);
   EXPECT_THAT(notes(results[0]), HasSubstr("not available"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

// =================================================================================================
// What can be seen of it
// =================================================================================================

TEST_F(Probe, LogsTheHttpAndTheLookupsAtDebugLevel)
{
   //
   // What -v is for. The wording is up to the libraries, so this is only about it being there.
   //
   std::ostringstream log;
   const auto previous = spdlog::default_logger();
   spdlog::set_default_logger(std::make_shared<spdlog::logger>(
      "test", std::make_shared<spdlog::sinks::ostream_sink_mt>(log)));
   spdlog::set_level(spdlog::level::debug);

   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});
   probe(config());

   spdlog::set_default_logger(previous);
   quiet();

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200);

   EXPECT_THAT(log.str(), HasSubstr("asking for the HTTPS records of 'localhost'")) << "our own";
   EXPECT_THAT(log.str(), HasSubstr("GET https://localhost/")) << "anyhttp's request";
   EXPECT_THAT(log.str(), HasSubstr("content-type")) << "anyhttp's response";
}

// =================================================================================================
// What can go wrong
// =================================================================================================

TEST_F(Probe, GivesUpOnAServerThatDoesNotAnswer)
{
   auto silent = silent_udp_socket();
   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));

   auto configuration = config();
   configuration.timeout = 100ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("timed out"));
}

TEST_F(Probe, GivesUpOnATcpServerThatDoesNotAnswer)
{
   //
   // It takes connections, as the kernel does that for it, and leaves the handshake hanging.
   //
   auto silent = silent_tcp_acceptor();
   site.dns().add_https("localhost", 1, ".", h2_at(silent.local_endpoint().port()));

   auto configuration = config();
   configuration.timeout = 100ms;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("timed out"));
}

TEST_F(Probe, ReportsAPortThatIsClosed)
{
   std::uint16_t closed = 0;
   {
      auto acceptor = silent_tcp_acceptor();
      closed = acceptor.local_endpoint().port();
   }
   site.dns().add_https("localhost", 1, ".", h2_at(closed));

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("connect"));
}

TEST_F(Probe, ReportsANameThatDoesNotExist)
{
   probe(config({"nowhere.example.test"}));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::not_found);
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
