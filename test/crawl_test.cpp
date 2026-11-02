//
// The whole crawl, against a server and a DNS server of its own on loopback. Nothing in here may
// ever reach for anything else.
//
#include "crawler.hpp"
#include "site.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#include <spdlog/spdlog.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <string>
#include <vector>

using namespace crawler;
using namespace std::chrono_literals;
using crawler::test::Site;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

namespace
{

std::vector<fake_dns_server::svc_param> h3_at(std::uint16_t port)
{
   return {{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}},
           {ARES_SVCB_PARAM_PORT,
            {static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port & 0xff)}}};
}

const fake_dns_server::svc_param alpn_h3{ARES_SVCB_PARAM_ALPN, {2, 'h', '3'}};
const fake_dns_server::svc_param alpn_h2{ARES_SVCB_PARAM_ALPN, {2, 'h', '2'}};

class Crawl : public testing::Test
{
protected:
   explicit Crawl(const std::string& listen_address = "::")
      : site{context.get_executor(), listen_address}
   {
   }

   void SetUp() override { site.page("/robots.txt", {.status = 404}); }

   Config config(std::vector<std::string> seeds, std::vector<std::string> allowed = {"localhost"})
   {
      return {.allowed_hosts = std::move(allowed),
              .seeds = std::move(seeds),
              .max_pages = 100,
              .delay = 0ms,
              .timeout = 1000ms,
              .dns_server = site.dns_address(),
              .tls_ca_file = site.ca_file().string()};
   }

   void crawl(Config config)
   {
      Crawler crawler(context.get_executor(), std::move(config),
                      [this](const Event& event) { events.push_back(event); });

      boost::asio::co_spawn(
         context,
         [&]() -> anyhttp::Task<void> {
            co_await crawler.run();
            stop();
         },
         [](const std::exception_ptr& ex) {
            if (ex)
               std::rethrow_exception(ex);
         });
      context.run();
   }

   //
   // For the tests with a QUIC endpoint that never answers. anyhttp does not close a session whose
   // connect() was cancelled: it goes on sending until its idle timeout of 30 seconds, and the
   // context does not run dry before that. To go once that is fixed in anyhttp.
   //
   bool abandon_quic_sessions = false;

   void stop()
   {
      site.stop();
      if (abandon_quic_sessions)
         context.stop();
   }

   std::vector<Event> of_kind(Event::Kind kind) const
   {
      std::vector<Event> result;
      std::ranges::copy_if(events, std::back_inserter(result),
                           [&](const Event& event) { return event.kind == kind; });
      return result;
   }

   std::vector<std::string> urls(Event::Kind kind) const
   {
      std::vector<std::string> result;
      for (const auto& event : of_kind(kind))
         result.push_back(event.url);
      return result;
   }

   std::string notes() const
   {
      std::string result;
      for (const auto& event : of_kind(Event::Kind::note))
         result += event.detail + "\n";
      return result;
   }

   std::string local_url(std::string_view host, std::string_view path = "/") const
   {
      return std::format("http://{}:{}{}", host, site.port(), path);
   }

   static int quiet()
   {
      spdlog::set_level(spdlog::level::off); // anyhttp logs through the default logger
      return 0;
   }

   int quiet_ = quiet(); // ahead of the server, which already has something to say when it starts
   boost::asio::io_context context;
   Site site;
   std::vector<Event> events;
};

} // namespace

// =================================================================================================
// What it refuses to do
// =================================================================================================

TEST_F(Crawl, RefusesToRunWithoutAnAllowlist)
{
   EXPECT_THROW(crawl(config({"http://localhost/"}, {})), std::invalid_argument);
   EXPECT_THAT(site.requests(), IsEmpty());
   site.stop();
   context.run();
}

TEST_F(Crawl, RefusesASeedOutsideTheAllowlist)
{
   EXPECT_THROW(crawl(config({"http://www.example.test/"})), std::invalid_argument);
   site.stop();
   context.run();
}

TEST_F(Crawl, RefusesAWildcard)
{
   EXPECT_THROW(crawl(config({"http://localhost/"}, {"*"})), std::invalid_argument);
   EXPECT_THROW(crawl(config({"http://localhost/"}, {"*.localhost"})), std::invalid_argument);
   site.stop();
   context.run();
}

TEST_F(Crawl, RefusesWhatIsNotAUrlToCrawl)
{
   EXPECT_THROW(crawl(config({"ftp://localhost/"})), std::invalid_argument);
   EXPECT_THROW(crawl(config({"localhost/index.html"})), std::invalid_argument);
   EXPECT_THROW(crawl(config({})), std::invalid_argument);
   site.stop();
   context.run();
}

// =================================================================================================
// How the HTTPS records decide about the connection
// =================================================================================================

TEST_F(Crawl, ConnectsOverHttp3ToThePortOfTheRecord)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   crawl(config({"https://localhost/"}));

   const auto fetched = of_kind(Event::Kind::fetched);
   ASSERT_EQ(fetched.size(), 1u) << notes();
   EXPECT_EQ(fetched[0].url, "https://localhost/");
   EXPECT_EQ(fetched[0].status, 200);
   EXPECT_EQ(fetched[0].route, std::format("h3 localhost:{}", site.port()));

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
   EXPECT_EQ(site.requests().back().authority, "localhost") << "the origin, not where it went";
   EXPECT_THAT(site.requests().back().user_agent, HasSubstr("crawler/"));
}

TEST_F(Crawl, FindsTheRecordOfAnotherPortUnderItsPrefix)
{
   site.dns().add_https(std::format("_{}._https.localhost", site.port()), 1, "localhost",
                        {alpn_h3});
   site.page("/", {.body = "hello"});

   crawl(config({std::format("https://localhost:{}/", site.port())}));

   EXPECT_THAT(urls(Event::Kind::fetched),
               ElementsAre(std::format("https://localhost:{}/", site.port())))
      << notes();
}

TEST_F(Crawl, SkipsARecordItCanNotUse)
{
   site.dns().add_https("localhost", 1, ".", {alpn_h2});
   site.dns().add_https("localhost", 2, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   crawl(config({"https://localhost/"}));

   ASSERT_EQ(of_kind(Event::Kind::fetched).size(), 1u) << notes();
   EXPECT_THAT(notes(), HasSubstr("alpn=h2"));
   EXPECT_THAT(notes(), HasSubstr("TLS over TCP"));
}

TEST_F(Crawl, FallsBackToTheNextRecordWhenAConnectionFails)
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

   auto configuration = config({"https://localhost/"});
   configuration.timeout = 200ms;
   crawl(std::move(configuration));

   const auto fetched = of_kind(Event::Kind::fetched);
   ASSERT_EQ(fetched.size(), 1u) << notes();
   EXPECT_EQ(fetched[0].route, std::format("h3 localhost:{}", site.port()));
}

TEST_F(Crawl, FailsWithoutARecordOnAnHttpsOrigin)
{
   site.page("/", {.body = "hello"});

   crawl(config({"https://localhost/"}));

   EXPECT_THAT(urls(Event::Kind::failed), ElementsAre("https://localhost/"));
   EXPECT_THAT(notes(), HasSubstr("TLS over TCP"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Crawl, DoesNotConnectToATargetOtherThanTheOrigin)
{
   //
   // The server would answer: it is the certificate that could not be right for this name.
   //
   site.dns().add_https("localhost", 1, "elsewhere.example.test", h3_at(site.port()));
   site.dns().add_address("elsewhere.example.test", "127.0.0.1");

   crawl(config({"https://localhost/"}));

   EXPECT_THAT(urls(Event::Kind::failed), ElementsAre("https://localhost/"));
   EXPECT_THAT(notes(), HasSubstr("elsewhere.example.test"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Crawl, RefusesACertificateItDoesNotTrust)
{
   Site other(context.get_executor()); // has a certificate of its own
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   auto configuration = config({"https://localhost/"});
   configuration.tls_ca_file = other.ca_file().string();
   other.stop();
   crawl(std::move(configuration));

   EXPECT_THAT(urls(Event::Kind::fetched), IsEmpty());
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre("https://localhost/"));
   EXPECT_THAT(notes(), HasSubstr("robots.txt could not be read"));
   EXPECT_THAT(site.requests(), IsEmpty()) << "no request survives the handshake";
}

TEST_F(Crawl, FollowsAnAliasToItsRecords)
{
   //
   // At the end of the chain it is the name of the alias's target that counts. So this is a route
   // only for an alias that leads back to the origin, by way of another name.
   //
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.dns().add_https("alias.example.test", 1, "localhost", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   crawl(config({"https://localhost/"}));

   EXPECT_THAT(urls(Event::Kind::fetched), ElementsAre("https://localhost/")) << notes();
   EXPECT_EQ(site.dns_queries("alias.example.test"), 1u);
}

TEST_F(Crawl, RecognizesAnAliasLoop)
{
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.dns().add_https("alias.example.test", 0, "localhost");

   crawl(config({"https://localhost/"}));

   EXPECT_THAT(notes(), HasSubstr("too long or has a loop"));
   EXPECT_THAT(urls(Event::Kind::failed), ElementsAre("https://localhost/"));
}

TEST_F(Crawl, IgnoresServiceRecordsNextToAnAlias)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.dns().add_https("localhost", 0, "alias.example.test");
   site.page("/", {.body = "hello"});

   crawl(config({"https://localhost/"}));

   EXPECT_THAT(urls(Event::Kind::fetched), IsEmpty()) << "the alias has no records of its own";
   EXPECT_THAT(site.requests(), IsEmpty());
}

// -------------------------------------------------------------------------------------------------

TEST_F(Crawl, ConnectsInTheClearToTheAddressesOfAnHttpOrigin)
{
   site.dns().add_address("www.example.test", "127.0.0.1");
   site.page("/", {.body = "hello"});

   crawl(config({local_url("www.example.test")}, {"www.example.test"}));

   const auto fetched = of_kind(Event::Kind::fetched);
   ASSERT_EQ(fetched.size(), 1u) << notes();
   EXPECT_EQ(fetched[0].route, std::format("h1 www.example.test:{}", site.port()));

   //
   // A name that only our DNS server knows, so this is the proof that the address came from there,
   // and the request still names the host.
   //
   EXPECT_EQ(site.requests().back().authority, std::format("www.example.test:{}", site.port()));
}

TEST_F(Crawl, UpgradesAnHttpOriginThatHasARecord)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = "hello"});

   crawl(config({"http://localhost/"}));

   const auto fetched = of_kind(Event::Kind::fetched);
   ASSERT_EQ(fetched.size(), 1u) << notes();
   EXPECT_EQ(fetched[0].url, "https://localhost/");
   EXPECT_THAT(fetched[0].route, HasSubstr("h3"));
}

TEST_F(Crawl, DoesNotGoBackToCleartextAfterAnUpgrade)
{
   //
   // The server would take cleartext on this port. It is not to be asked.
   //
   site.dns().add_https(std::format("_{}._https.localhost", site.port()), 1, ".", {alpn_h2});
   site.page("/", {.body = "hello"});

   crawl(config({local_url("localhost")}));

   EXPECT_THAT(urls(Event::Kind::fetched), IsEmpty());
   EXPECT_THAT(urls(Event::Kind::failed),
               ElementsAre(std::format("https://localhost:{}/", site.port())));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Crawl, TreatsBothFormsOfAnUpgradedPageAsOne)
{
   site.dns().add_https("localhost", 1, ".", h3_at(site.port()));
   site.page("/", {.body = R"(<a href="http://localhost/">again</a>)"});

   crawl(config({"https://localhost/"}));

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
}

TEST_F(Crawl, DoesNotAskForTheRecordsOfAnAddress)
{
   site.page("/", {.body = "hello"});

   crawl(config({local_url("127.0.0.1")}, {"127.0.0.1"}));

   EXPECT_THAT(urls(Event::Kind::fetched), ElementsAre(local_url("127.0.0.1"))) << notes();
   EXPECT_EQ(site.dns_queries("127.0.0.1"), 0u);
   EXPECT_EQ(site.dns_queries(std::format("_{}._https.127.0.0.1", site.port())), 0u);
}

//
// A server on 127.0.0.2 alone, which leaves nothing listening on 127.0.0.3 at the same port.
//
class CrawlOneAddress : public Crawl
{
protected:
   CrawlOneAddress() : Crawl("127.0.0.2") {}
};

TEST_F(CrawlOneAddress, TriesTheNextAddressOfAHost)
{
   site.dns().add_address("www.example.test", "127.0.0.3");
   site.dns().add_address("www.example.test", "127.0.0.2");
   site.page("/", {.body = "hello"});

   crawl(config({local_url("www.example.test")}, {"www.example.test"}));

   EXPECT_THAT(urls(Event::Kind::fetched), ElementsAre(local_url("www.example.test"))) << notes();
}

// =================================================================================================
// What it follows
// =================================================================================================

class CrawlInTheClear : public Crawl
{
protected:
   void SetUp() override
   {
      Crawl::SetUp();
      site.dns().add_address(host, "127.0.0.1");
   }

   std::string url(std::string_view path = "/") const { return local_url(host, path); }
   Config host_config() { return config({url()}, {host}); }

   const std::string host = "www.example.test";
};

TEST_F(CrawlInTheClear, FollowsLinksBreadthFirstAndOnlyOnce)
{
   site.page("/", {.body = R"(<a href="/a">a</a> <a href="/b">b</a> <a href="/a#again">a</a>)"});
   site.page("/a", {.body = R"(<a href="/c">c</a> <a href="/">home</a>)"});
   site.page("/b", {.body = R"(<a href="c">c</a>)"});
   site.page("/c", {.body = "the end"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/", "/a", "/b", "/c"));
}

TEST_F(CrawlInTheClear, DoesNotFollowLinksOutOfTheAllowlist)
{
   site.dns().add_address("other.example.test", "127.0.0.1");
   site.page("/", {.body = std::format(R"(<a href="{}">other</a>)",
                                       local_url("other.example.test", "/secret"))});
   site.page("/secret", {.body = "not for us"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
   EXPECT_THAT(urls(Event::Kind::outside_allowlist),
               ElementsAre(local_url("other.example.test", "/secret")));
   EXPECT_EQ(site.dns_queries("other.example.test"), 0u) << "not even a lookup";
}

TEST_F(CrawlInTheClear, DoesNotTakeASubdomainForTheHost)
{
   site.dns().add_address("sub.www.example.test", "127.0.0.1");
   site.page("/", {.body = std::format(R"(<a href="{}">sub</a>)",
                                       local_url("sub.www.example.test", "/x"))});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
}

TEST_F(CrawlInTheClear, FollowsARedirectWithinTheAllowlist)
{
   site.page("/", {.status = 302, .location = "/moved"});
   site.page("/moved", {.body = "here"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/", "/moved"));
}

TEST_F(CrawlInTheClear, DoesNotFollowARedirectOutOfTheAllowlist)
{
   site.dns().add_address("other.example.test", "127.0.0.1");
   site.page("/", {.status = 302, .location = local_url("other.example.test", "/secret")});
   site.page("/secret", {.body = "not for us"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
   EXPECT_THAT(urls(Event::Kind::outside_allowlist),
               ElementsAre(local_url("other.example.test", "/secret")));
}

TEST_F(CrawlInTheClear, ReadsLinksOutOfHtmlOnly)
{
   site.page("/", {.body = R"(<a href="/no">no</a>)", .content_type = "text/plain"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
}

TEST_F(CrawlInTheClear, ReadsNoLinksOutOfAnError)
{
   site.page("/", {.status = 500, .body = R"(<a href="/no">no</a>)"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/"));
   ASSERT_EQ(of_kind(Event::Kind::fetched).size(), 1u);
   EXPECT_EQ(of_kind(Event::Kind::fetched)[0].status, 500);
}

TEST_F(CrawlInTheClear, StopsAtTheLimitOfPages)
{
   site.page("/", {.body = R"(<a href="/a">a</a> <a href="/b">b</a> <a href="/c">c</a>)"});
   site.page("/a", {.body = "a"});
   site.page("/b", {.body = "b"});
   site.page("/c", {.body = "c"});

   auto configuration = host_config();
   configuration.max_pages = 2;
   crawl(std::move(configuration));

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/", "/a"));
}

// =================================================================================================
// How it behaves towards a host
// =================================================================================================

TEST_F(CrawlInTheClear, ReadsRobotsTxtFirstAndOnce)
{
   site.page("/", {.body = R"(<a href="/a">a</a>)"});
   site.page("/a", {.body = "a"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/", "/a"));
}

TEST_F(CrawlInTheClear, HonoursRobotsTxt)
{
   site.page("/robots.txt",
             {.body = "User-agent: *\nDisallow: /private/\n", .content_type = "text/plain"});
   site.page("/", {.body = R"(<a href="/private/a">a</a> <a href="/public">b</a>)"});
   site.page("/private/a", {.body = "not for us"});
   site.page("/public", {.body = "fine"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/", "/public"));
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre(url("/private/a")));
}

TEST_F(CrawlInTheClear, CrawlsNothingOnAnErrorForRobotsTxt)
{
   site.page("/robots.txt", {.status = 503});
   site.page("/", {.body = "not yet"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt"));
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre(url()));
}

TEST_F(CrawlInTheClear, FollowsARedirectOfRobotsTxtOnTheOrigin)
{
   site.page("/robots.txt", {.status = 301, .location = "/real-robots.txt"});
   site.page("/real-robots.txt",
             {.body = "User-agent: *\nDisallow: /\n", .content_type = "text/plain"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt", "/real-robots.txt"));
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre(url()));
}

TEST_F(CrawlInTheClear, CrawlsNothingOnARedirectOfRobotsTxtToElsewhere)
{
   site.page("/robots.txt", {.status = 301, .location = "http://other.example.test/robots.txt"});
   site.page("/", {.body = "not yet"});

   crawl(host_config());

   EXPECT_THAT(site.targets(), ElementsAre("/robots.txt"));
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre(url()));
}

TEST_F(CrawlInTheClear, CrawlsNothingOnARedirectLoopOfRobotsTxt)
{
   site.page("/robots.txt", {.status = 301, .location = "/robots.txt"});
   site.page("/", {.body = "not yet"});

   crawl(host_config());

   EXPECT_EQ(site.targets().size(), 6u) << "the first one and five redirects";
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre(url()));
}

TEST_F(CrawlInTheClear, SpacesItsRequests)
{
   site.page("/", {.body = R"(<a href="/a">a</a> <a href="/b">b</a>)"});
   site.page("/a", {.body = "a"});
   site.page("/b", {.body = "b"});

   auto configuration = host_config();
   configuration.delay = 50ms;
   crawl(std::move(configuration));

   const auto& requests = site.requests();
   ASSERT_EQ(requests.size(), 4u);
   for (std::size_t i = 1; i < requests.size(); ++i)
      EXPECT_GE(requests[i].time - requests[i - 1].time, 50ms) << requests[i].target;
}

TEST_F(CrawlInTheClear, HonoursALongerCrawlDelay)
{
   site.page("/robots.txt",
             {.body = "User-agent: *\nCrawl-delay: 0.1\n", .content_type = "text/plain"});
   site.page("/", {.body = R"(<a href="/a">a</a>)"});
   site.page("/a", {.body = "a"});

   auto configuration = host_config();
   configuration.delay = 1ms;
   crawl(std::move(configuration));

   const auto& requests = site.requests();
   ASSERT_EQ(requests.size(), 3u);
   EXPECT_GE(requests[2].time - requests[1].time, 100ms);
}

// =================================================================================================
// What can go wrong
// =================================================================================================

TEST_F(Crawl, GivesUpOnAServerThatDoesNotAnswerOverTcp)
{
   //
   // Accepted by the kernel, and never heard of again.
   //
   boost::asio::ip::tcp::acceptor silent(context, {boost::asio::ip::make_address("127.0.0.1"), 0});
   site.dns().add_address("www.example.test", "127.0.0.1");

   const auto seed = std::format("http://www.example.test:{}/", silent.local_endpoint().port());
   auto configuration = config({seed}, {"www.example.test"});
   configuration.timeout = 100ms;
   crawl(std::move(configuration));

   EXPECT_THAT(urls(Event::Kind::fetched), IsEmpty());
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre(seed));
   EXPECT_THAT(notes(), HasSubstr("timed out"));
}

TEST_F(Crawl, GivesUpOnAServerThatDoesNotAnswerOverQuic)
{
   boost::asio::ip::udp::socket silent(context, {boost::asio::ip::udp::v6(), 0});
   abandon_quic_sessions = true;
   site.dns().add_https("localhost", 1, ".", h3_at(silent.local_endpoint().port()));

   auto configuration = config({"https://localhost/"});
   configuration.timeout = 100ms;
   crawl(std::move(configuration));

   EXPECT_THAT(urls(Event::Kind::fetched), IsEmpty());
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre("https://localhost/"));
   EXPECT_THAT(notes(), HasSubstr("timed out"));
}

TEST_F(Crawl, ReportsANameThatDoesNotExist)
{
   crawl(config({"http://nowhere.example.test/"}, {"nowhere.example.test"}));

   EXPECT_THAT(urls(Event::Kind::fetched), IsEmpty());
   EXPECT_THAT(urls(Event::Kind::disallowed), ElementsAre("http://nowhere.example.test/"));
   EXPECT_THAT(site.requests(), IsEmpty());
}

TEST_F(Crawl, GoesOnWhenTheRecordLookupFails)
{
   //
   // Nothing is protected here, so not getting an answer is no reason not to try without one
   // (RFC 9460, section 3.1). Every question fails with this, so it has to be a name that needs no
   // server for its address.
   //
   site.dns().set_rcode(ARES_RCODE_SERVFAIL);
   site.page("/", {.body = "hello"});

   crawl(config({local_url("localhost")}));

   EXPECT_THAT(urls(Event::Kind::fetched), ElementsAre(local_url("localhost"))) << notes();
   EXPECT_THAT(notes(), HasSubstr("HTTPS lookup"));
}
