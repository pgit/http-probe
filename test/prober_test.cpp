//
// The whole program but its command line, against a server and a DNS server of its own on loopback.
// Nothing in here may ever reach for anything else.
//
#include "prober.hpp"
#include "site.hpp"

#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <ostream>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace http_probe;
using namespace std::chrono_literals;
using http_probe::test::Site;
using testing::Contains;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using testing::Pair;

namespace boost::corosio
{

/// What a failed expectation shows of an endpoint: Corosio has no output operator for one.
static void PrintTo(const endpoint& endpoint, std::ostream* out)
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
   return {
      .key = ARES_SVCB_PARAM_PORT,
      .value = {static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port & 0xff)}};
}

const fake_dns_server::svc_param alpn_h3{.key = ARES_SVCB_PARAM_ALPN, .value = {2, 'h', '3'}};
const fake_dns_server::svc_param alpn_h2{.key = ARES_SVCB_PARAM_ALPN, .value = {2, 'h', '2'}};
const fake_dns_server::svc_param alpn_h1{.key = ARES_SVCB_PARAM_ALPN,
                                         .value = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'}};
const fake_dns_server::svc_param no_default_alpn{.key = ARES_SVCB_PARAM_NO_DEFAULT_ALPN,
                                                 .value = {}};

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
      const auto run = [&] -> capy::task<> {
         co_await prober.run();
         stop_sites();
      };

      std::exception_ptr failed;
      capy::run_async(
         context.get_executor(), [] noexcept {},
         [&](std::exception_ptr ex) noexcept {
            failed = std::move(ex);
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
      return {context, {corosio::ipv6_address::any(), 0}};
   }

   //
   // What the tests that do not get as far as run() still have to do.
   //
   void finish()
   {
      stop_sites();
      context.run();
   }

   virtual void stop_sites() { site.stop(); }

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
   EXPECT_EQ(results[0].location, "/moved"); // as it came, not resolved
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
   EXPECT_EQ(results[0].location, "/moved"); // as it came, not resolved
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
   EXPECT_EQ(results[0].location, "/moved");
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, HasADestinationForARedirectOnly)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.status = 201, .location = "/created"});

   probe(config());

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 201) << notes(results[0]);
   EXPECT_THAT(results[0].fields, Contains(Pair("location", "/created")));
   EXPECT_THAT(results[0].location, IsEmpty());
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

static corosio::endpoint at(const char* address) { return {make_address(address), 443}; }

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
   // What -vv is for. The wording is up to the libraries, so this is only about it being there.
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

TEST_F(Probe, KeepsAnyhttpOutOfALoggerOfItsOwn)
{
   //
   // What sets -v apart from -vv: anyhttp sticks to the default logger, and everything else goes to
   // the one in the configuration.
   //
   std::ostringstream library_log;
   const auto previous = spdlog::default_logger();
   spdlog::set_default_logger(std::make_shared<spdlog::logger>(
      "test", std::make_shared<spdlog::sinks::ostream_sink_mt>(library_log)));
   spdlog::set_level(spdlog::level::debug);

   std::ostringstream own_log;
   auto configuration = config();
   configuration.logger = std::make_shared<spdlog::logger>(
      "own", std::make_shared<spdlog::sinks::ostream_sink_mt>(own_log));
   configuration.logger->set_level(spdlog::level::debug);

   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});
   probe(std::move(configuration));

   spdlog::set_default_logger(previous);
   quiet();

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200);

   EXPECT_THAT(own_log.str(), HasSubstr("asking for the HTTPS records of 'localhost'"));
   EXPECT_THAT(own_log.str(), Not(HasSubstr("GET https://localhost/")));
   EXPECT_THAT(library_log.str(), HasSubstr("GET https://localhost/"));
   EXPECT_THAT(library_log.str(), Not(HasSubstr("asking for the HTTPS records")));
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

// =================================================================================================
// --h3, --h2 and --h1
// =================================================================================================

TEST_F(Probe, H3RequestsAHostWithARecordThatOffersIt)
{
   site.dns().add_https("localhost", 1, ".", {alpn_h3, port_param(site.port())});
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h3 localhost:{}", site.port()));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, H3LeavesOutAHostWithoutARecord)
{
   site.dns().add_address("localhost", "127.0.0.1");
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::left_out);
   EXPECT_THAT(results[0].address, IsEmpty());
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, H3LeavesOutAHostWhoseRecordsDoNotOfferIt)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::left_out);
   EXPECT_EQ(results[0].records.size(), 1u) << "for --all to say why";
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, H3DoesNotFallBackToTcp)
{
   auto silent = silent_udp_socket();

   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));
   site.dns().add_https("localhost", 2, ".", {alpn_h2, port_param(site.port())});
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.timeout = 200ms;
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("timed out"));
   EXPECT_THAT(notes(results[0]), HasSubstr("offers no h3"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, H3StillSaysThatARecordLeadsElsewhere)
{
   site.dns().add_https("localhost", 1, "elsewhere.example.test", h3_at(site.port()));

   auto configuration = config();
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::out_of_reach);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, H3StillSaysThatANameDoesNotExist)
{
   auto configuration = config({"nowhere.example.test"});
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::not_found);
}

TEST_F(Probe, H3DoesNotTakeAFailedLookupForNoHttp3)
{
   site.dns().set_rcode(ARES_RCODE_SERVFAIL);

   auto configuration = config();
   configuration.protocols = {.h3 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, H2SpeaksNothingElseThatTheRecordOffers)
{
   const fake_dns_server::svc_param alpn_h3_h2{.key = ARES_SVCB_PARAM_ALPN,
                                               .value = {2, 'h', '3', 2, 'h', '2'}};
   site.dns().add_https("localhost", 1, ".", {alpn_h3_h2, port_param(site.port())});
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h2 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h2 localhost:{}", site.port()));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, H1SpeaksHttp1WhereTheRecordOffersHttp2AsWell)
{
   site.dns().add_https("localhost", 1, ".", {alpn_h2, port_param(site.port())});
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h1 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h1 localhost:{}", site.port()));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, H1LeavesOutAHostWhoseRecordsOfferOnlyHttp2)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h1 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::left_out);
   EXPECT_THAT(notes(results[0]), HasSubstr("offers no h1"));
   EXPECT_THAT(results[0].address, IsEmpty());
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, H1GoesOverTcpToTheDefaultPortWithoutARecord)
{
   site.dns().add_address("localhost", "127.0.0.1");
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.protocols = {.h1 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h1 localhost:{}", site.port()));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

TEST_F(Probe, H3AndH2FallBackToTcpWhenAConnectionFails)
{
   auto silent = silent_udp_socket();

   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));
   site.dns().add_https("localhost", 2, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.timeout = 200ms;
   configuration.protocols = {.h3 = true, .h2 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].status, 200) << notes(results[0]);
   EXPECT_EQ(results[0].route, std::format("h2 localhost:{}", site.port()));
   EXPECT_THAT(site.targets(), ElementsAre("/"));
}

namespace
{

//
// A server over TLS that speaks HTTP/1.1 and nothing else, by ALPN, where the site's server takes
// HTTP/2 whenever it is offered. It answers a request with a 204 and keeps what it was sent.
//
class Http1Server : public Probe
{
protected:
   void SetUp() override
   {
      ASSERT_FALSE(tls.use_certificate_chain_file(site.ca_file().string()));
      ASSERT_FALSE(
         tls.use_private_key_file(site.key_file().string(), corosio::tls_file_format::pem));
      ASSERT_FALSE(tls.set_alpn({"http/1.1"}));
      capy::run_async(context.get_executor())(serve());

      port = acceptor.local_endpoint().port(); // which is 0 once it is closed
      site.dns().add_https("localhost", 1, ".", {alpn_h2, port_param(port)});
   }

   void stop_sites() override
   {
      site.stop();
      acceptor.close();
   }

   capy::task<> serve()
   {
      for (;;)
      {
         corosio::tcp_socket socket(context);
         if (auto [ec] = co_await acceptor.accept(socket); ec)
            co_return;
         ++connections;

         corosio::openssl_stream stream(std::move(socket), tls);
         if (auto [ec] = co_await stream.handshake(corosio::tls_role::server); ec)
            continue;

         std::string request;
         std::array<char, 1024> buffer;
         while (!request.contains("\r\n\r\n"))
         {
            auto [ec, n] = co_await stream.read_some(capy::make_buffer(buffer));
            if (ec || n == 0)
               break;
            request.append(buffer.data(), n);
         }
         received += request;

         if (request.contains("\r\n\r\n"))
         {
            constexpr std::string_view response =
               "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
            std::ignore = co_await capy::write(stream, capy::make_buffer(response));
         }
         std::ignore = co_await stream.shutdown();
      }
   }

   corosio::tcp_acceptor acceptor = silent_tcp_acceptor();
   std::uint16_t port = 0;
   corosio::tls_context tls;
   std::string received; ///< all that came in over all connections, once decrypted
   int connections = 0;
};

} // namespace

TEST_F(Http1Server, H2DoesNotSendTheRequestToAServerThatChoosesHttp1)
{
   auto configuration = config();
   configuration.protocols = {.h2 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(results[0].error, HasSubstr("chose h1"));
   EXPECT_GE(connections, 1);
   EXPECT_THAT(received, IsEmpty());
}

TEST_F(Http1Server, H2AndH1LetTheServerChooseHttp1)
{
   auto configuration = config();
   configuration.protocols = {.h2 = true, .h1 = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].status, 204);
   EXPECT_EQ(results[0].route, std::format("h1 localhost:{}", port));
   EXPECT_THAT(received, testing::StartsWith("GET / HTTP/1.1\r\n"));
   EXPECT_EQ(connections, 1);
}

// =================================================================================================
// --dns-only
// =================================================================================================

TEST_F(Probe, DnsOnlyReportsTheRecordsAndTheAddressesInTheOrderOfTrying)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("site.example.test", "127.0.0.1");
   site.dns().add_address("site.example.test", "::1");

   auto configuration = config({"site.example.test"});
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::resolved) << notes(results[0]);
   EXPECT_EQ(results[0].records.size(), 1u);
   EXPECT_THAT(results[0].addresses, ElementsAre("::1", "127.0.0.1"));
   EXPECT_THAT(results[0].address, IsEmpty()) << "that is for what was contacted";
}

TEST_F(Probe, DnsOnlyDoesNotRequestAHostThatWouldAnswer)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::resolved) << notes(results[0]);
   EXPECT_THAT(results[0].addresses, Not(IsEmpty()));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, DnsOnlyDoesNotRequestAHostWithoutARecord)
{
   site.dns().add_address("localhost", "127.0.0.1");
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::resolved) << notes(results[0]);
   EXPECT_THAT(results[0].records, IsEmpty());
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, DnsOnlyStillSaysThatARecordLeadsElsewhere)
{
   site.dns().add_https("localhost", 1, "elsewhere.example.test", h3_at(site.port()));

   auto configuration = config();
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::out_of_reach);
}

TEST_F(Probe, DnsOnlyStillSaysThatANameDoesNotExist)
{
   auto configuration = config({"nowhere.example.test"});
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::not_found);
}

TEST_F(Probe, DnsOnlyStillSaysThatALookupFailed)
{
   site.dns().set_rcode(ARES_RCODE_SERVFAIL);

   auto configuration = config();
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
}

TEST_F(Probe, DnsOnlyStillSaysThatThereIsNoAddress)
{
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));

   auto configuration = config({"site.example.test"});
   configuration.dns_only = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
   EXPECT_THAT(notes(results[0]), HasSubstr("address lookup"));
}

TEST_F(Probe, DnsOnlyGoesWithH3)
{
   site.dns().add_https("offers.example.test", 1, ".", h3_at(site.port()));
   site.dns().add_address("offers.example.test", "127.0.0.1");
   site.dns().add_https("lacks.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("lacks.example.test", "127.0.0.1");

   auto configuration = config({"offers.example.test", "lacks.example.test"});
   configuration.dns_only = true;
   configuration.protocols = {.h3 = true};
   configuration.concurrency = 1;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 2u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::resolved) << notes(results[0]);
   EXPECT_EQ(results[1].outcome, Result::Outcome::left_out);
   EXPECT_THAT(site.requests(), IsEmpty());
}

// =================================================================================================
// --each-address
// =================================================================================================

//
// "localhost" is the one name the certificate is good for, and its addresses are up to /etc/hosts.
// The server listens on all of them, however many that is.
//
TEST_F(Probe, EachAddressSendsOneRequestToEachOfThem)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   ASSERT_THAT(results[0].replies, Not(IsEmpty()));
   for (const auto& reply : results[0].replies)
   {
      EXPECT_THAT(reply.address, Not(IsEmpty()));
      EXPECT_EQ(reply.status, 200);
      EXPECT_EQ(reply.reason, "OK");
      EXPECT_THAT(reply.error, IsEmpty());
   }
   EXPECT_EQ(site.requests().size(), results[0].replies.size());
}

//
// The server takes its time with each of them, so that one after the other, no request would arrive
// before the response to the one before.
//
TEST_F(Probe, EachAddressAsksAllOfThemAtTheSameTime)
{
   constexpr auto delay = 400ms;
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.body = "hello", .delay = delay});

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   if (results[0].replies.size() < 2)
      GTEST_SKIP() << "localhost has only one address here";
   ASSERT_EQ(site.requests().size(), results[0].replies.size());
   const auto [first, last] =
      std::ranges::minmax(site.requests() | std::views::transform(&Site::Request::time));
   EXPECT_LT(last - first, delay);
}

TEST_F(Probe, EachAddressGoesOverHttp3)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   ASSERT_THAT(results[0].replies, Not(IsEmpty()));
   EXPECT_EQ(results[0].replies[0].reason, "OK");
   EXPECT_EQ(site.requests().size(), results[0].replies.size());
}

TEST_F(Probe, EachAddressGoesOverHttp1)
{
   site.dns().add_https("localhost", 1, ".", h1_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   ASSERT_THAT(results[0].replies, Not(IsEmpty()));
   EXPECT_EQ(results[0].replies[0].reason, "OK");
   EXPECT_EQ(site.requests().size(), results[0].replies.size());
}

TEST_F(Probe, EachAddressReportsARedirectAndDoesNotFollowIt)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.page("/", {.status = 301, .location = "/elsewhere"});
   site.page("/elsewhere", {.body = "not to be seen"});

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   ASSERT_THAT(results[0].replies, Not(IsEmpty()));
   for (const auto& reply : results[0].replies)
   {
      EXPECT_EQ(reply.status, 301);
      EXPECT_EQ(reply.location, "/elsewhere");
   }
   EXPECT_THAT(site.targets(), testing::Each("/"));
   EXPECT_EQ(site.requests().size(), results[0].replies.size());
}

TEST_F(Probe, EachAddressGoesOnToTheNextOneAfterAFailure)
{
   //
   // The certificate is not for this name, so that both of them fail at connecting, in order.
   //
   site.dns().add_https("site.example.test", 1, ".", h2_at(site.port()));
   site.dns().add_address("site.example.test", "127.0.0.1");
   site.dns().add_address("site.example.test", "::1");

   auto configuration = config({"site.example.test"});
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   ASSERT_EQ(results[0].replies.size(), 2u);
   EXPECT_EQ(results[0].replies[0].address, "::1");
   EXPECT_EQ(results[0].replies[1].address, "127.0.0.1");
   EXPECT_THAT(results[0].replies[0].error, HasSubstr("connect"));
   EXPECT_THAT(results[0].replies[1].error, HasSubstr("connect"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, EachAddressTriesTheNextRouteOnlyForAConnectionThatFailed)
{
   std::uint16_t closed = 0;
   {
      auto acceptor = silent_tcp_acceptor();
      closed = acceptor.local_endpoint().port();
   }
   site.dns().add_https("localhost", 1, ".", h2_at(closed));
   site.dns().add_https("localhost", 2, ".", h2_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   ASSERT_THAT(results[0].replies, Not(IsEmpty()));
   for (const auto& reply : results[0].replies)
      EXPECT_EQ(reply.status, 200) << reply.error;
   EXPECT_EQ(site.requests().size(), results[0].replies.size());
}

TEST_F(Probe, EachAddressTriesNoOtherRouteOnceARequestHasGoneOut)
{
   site.dns().add_https("localhost", 1, ".", h2_at(site.port()));
   site.dns().add_https("localhost", 2, ".", h1_at(site.port()));
   site.page("/", {.body = "too late", .delay = 400ms});

   auto configuration = config();
   configuration.timeout = 100ms;
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   ASSERT_THAT(results[0].replies, Not(IsEmpty()));
   for (const auto& reply : results[0].replies)
      EXPECT_THAT(reply.error, HasSubstr("timed out"));
   EXPECT_EQ(site.requests().size(), results[0].replies.size());
}

TEST_F(Probe, EachAddressStillSaysThatARecordLeadsElsewhere)
{
   site.dns().add_https("localhost", 1, "elsewhere.example.test", h2_at(site.port()));

   auto configuration = config();
   configuration.each_address = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::out_of_reach);
   EXPECT_THAT(results[0].replies, IsEmpty());
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Probe, EachAddressDoesNotGoWithDnsOnly)
{
   auto configuration = config();
   configuration.each_address = true;
   configuration.dns_only = true;

   EXPECT_THROW(Prober(context.get_executor(), std::move(configuration), [](const Result&) {}),
                std::invalid_argument);
   finish();
}

// =================================================================================================
// --dot
// =================================================================================================

namespace
{

const test_certificate& dns_certificate()
{
   static const auto certificate = make_test_certificate("dns.test");
   return certificate;
}

class DnsOverTls : public Probe
{
protected:
   void SetUp() override
   {
      site.dns().use_tls(dns_certificate());
      site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
      site.page("/", {.body = "hello"});
   }

   void TearDown() override
   {
      ::unsetenv("SSL_CERT_FILE");
      if (!trusted.empty())
         std::filesystem::remove(trusted);
   }

   void stop_sites() override
   {
      site.stop();
      names.stop();
   }

   //
   // The resolver verifies against the default trust store, which is the one thing that it lets
   // anybody say something about. --ca-file does not come into it.
   //
   void trust_the_dns_server()
   {
      trusted = std::filesystem::temp_directory_path() /
                std::format("http-probe-test-dns-{}.pem", ::getpid());
      std::ofstream(trusted) << dns_certificate().certificate_pem;
      ASSERT_EQ(::setenv("SSL_CERT_FILE", trusted.c_str(), 1), 0);
   }

   Config config_over_tls(std::string hostname, bool verify = true)
   {
      auto configuration = config();
      configuration.dns_tls = {.enabled = true, .hostname = std::move(hostname), .verify = verify};
      return configuration;
   }

   /// A server of \p site, by the name that \ref names has for it.
   Config config_by_name(const Site& server)
   {
      names.dns().add_address("dns.test", "127.0.0.1");

      auto configuration = config();
      const auto address = server.dns_address();
      configuration.dns_server = "dns.test" + address.substr(address.rfind(':'));
      configuration.dns_server_lookup = names.dns_address();
      return configuration;
   }

   Site names{context.get_executor()}; // what the system's resolver is to a host name of a server
   std::filesystem::path trusted;
};

} // namespace

TEST_F(DnsOverTls, AsksOverTlsAndNothingElse)
{
   trust_the_dns_server();
   probe(config_over_tls("dns.test"));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(results[0].status, 200);
   EXPECT_GE(site.dns_queries(fake_dns_server::transport::tls), 1u);
   EXPECT_EQ(site.dns_queries(fake_dns_server::transport::udp), 0u);
   EXPECT_EQ(site.dns_queries(fake_dns_server::transport::tcp), 0u);
}

TEST_F(DnsOverTls, DoesNotConnectIfTheCertificateIsForAnotherName)
{
   trust_the_dns_server();
   probe(config_over_tls("other.test"));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(DnsOverTls, DoesNotConnectIfTheCertificateIsNotTrusted)
{
   probe(config_over_tls("dns.test"));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(DnsOverTls, SkipsTheCheckIfToldTo)
{
   probe(config_over_tls("", false));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_GE(site.dns_queries(fake_dns_server::transport::tls), 1u);
}

TEST_F(DnsOverTls, StillVerifiesTheHostsWhenToldToSkipTheCheck)
{
   Site other(context.get_executor()); // has a certificate of its own

   auto configuration = config_over_tls("", false);
   configuration.tls_ca_file = other.ca_file().string();
   other.stop();
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::failed);
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(DnsOverTls, TakesTheNameToVerifyFromTheDnsServer)
{
   trust_the_dns_server();

   auto configuration = config_by_name(site);
   configuration.dns_tls.enabled = true;
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_GE(site.dns_queries(fake_dns_server::transport::tls), 1u);
   EXPECT_EQ(names.dns_queries("dns.test"), names.dns_queries(fake_dns_server::transport::udp))
      << "looking up the server is in the clear, and it is all that the other one was asked";
   EXPECT_GE(names.dns_queries("dns.test"), 1u);
}

TEST_F(DnsOverTls, PrefersTheNameItIsGivenToTheOneOfTheDnsServer)
{
   trust_the_dns_server();

   auto configuration = config_by_name(site);
   configuration.dns_tls = {.enabled = true, .hostname = "other.test", .verify = true};
   probe(std::move(configuration));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::lookup_failed);
}

TEST_F(DnsOverTls, TakesADnsServerByNameWithoutDot)
{
   names.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   probe(config_by_name(names));

   ASSERT_EQ(results.size(), 1u);
   EXPECT_EQ(results[0].outcome, Result::Outcome::answered) << notes(results[0]);
   EXPECT_EQ(site.dns_queries(fake_dns_server::transport::tls), 0u);
}

TEST_F(DnsOverTls, EndsTheRunIfADnsServerCanNotBeLookedUp)
{
   auto configuration = config();
   configuration.dns_server = "missing.test";
   configuration.dns_server_lookup = names.dns_address();

   EXPECT_THROW(probe(std::move(configuration)), std::runtime_error);
   EXPECT_THAT(results, IsEmpty());
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(DnsOverTls, RefusesAnAddressWithoutANameToVerify)
{
   EXPECT_THROW(probe(config_over_tls("")), std::invalid_argument);
   finish();
}

TEST_F(DnsOverTls, RefusesSeveralNamesWithoutOneToVerify)
{
   auto configuration = config_over_tls("");
   configuration.dns_server = "dns1.test,dns2.test";
   EXPECT_THROW(probe(std::move(configuration)), std::invalid_argument);
   finish();
}

TEST_F(DnsOverTls, RefusesToVerifyWithoutDot)
{
   auto configuration = config();
   configuration.dns_tls.hostname = "dns.test";
   EXPECT_THROW(probe(std::move(configuration)), std::invalid_argument);
   finish();
}

TEST_F(Probe, RefusesADnsServerThatIsNoServer)
{
   auto configuration = config();
   configuration.dns_server = "not a server";
   EXPECT_THROW(probe(std::move(configuration)), std::invalid_argument);
   finish();
}

//
// The connection stays open for the next query and the server has no reason to close it, so the
// program would never end if that was keeping the event loop going. This takes a context that has
// nothing else to do, unlike the one of the others, and with that a server on a thread of its own,
// which is only looked at when it is done.
//
TEST(DnsOverTlsConnection, DoesNotKeepTheProgramFromEnding)
{
   spdlog::set_level(spdlog::level::off);

   corosio::io_context site_context;
   Site site(site_context.get_executor());
   site.dns().use_tls(dns_certificate());
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   std::size_t answered = 0;
   {
      std::jthread serving([&] { site_context.run(); });

      corosio::io_context context;
      Prober prober(
         context.get_executor(),
         {.hosts = {"localhost"},
          .timeout = 1000ms,
          .dns_server = site.dns_address(),
          .dns_tls = {.enabled = true, .hostname = "", .verify = false},
          .tls_ca_file = site.ca_file().string(),
          .default_port = site.port()},
         [&](const Result& result) { answered += result.outcome == Result::Outcome::answered; });
      capy::run_async(context.get_executor())(prober.run());
      context.run(); // what hangs if it does not end, for the timeout of the test to find

      const auto stop = [](Site& site) -> capy::task<> {
         site.stop();
         co_return;
      };
      capy::run_async(site_context.get_executor())(stop(site));
   }
   EXPECT_EQ(answered, 1u);
}
