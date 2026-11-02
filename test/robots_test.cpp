#include "robots.hpp"

#include <gtest/gtest.h>

using crawler::Robots;
using namespace std::chrono_literals;

TEST(Robots, AllowsEverythingByDefault)
{
   EXPECT_TRUE(Robots{}.allowed("/anything"));
   EXPECT_TRUE(Robots::parse("", "crawler").allowed("/anything"));
}

TEST(Robots, DisallowAllAllowsOnlyItself)
{
   EXPECT_FALSE(Robots::disallow_all().allowed("/"));
   EXPECT_TRUE(Robots::disallow_all().allowed("/robots.txt"));
}

TEST(Robots, AppliesTheGroupForEveryone)
{
   const auto robots = Robots::parse("User-agent: *\nDisallow: /private/\n", "crawler");
   EXPECT_FALSE(robots.allowed("/private/x"));
   EXPECT_TRUE(robots.allowed("/public/x"));
}

TEST(Robots, PrefersTheGroupThatNamesTheAgentInAnyCase)
{
   const auto robots = Robots::parse("User-agent: *\nDisallow: /\n\n"
                                     "User-Agent: CRAWLER\nDisallow: /mine/\n",
                                     "crawler");
   EXPECT_TRUE(robots.allowed("/x"));
   EXPECT_FALSE(robots.allowed("/mine/x"));
}

TEST(Robots, IgnoresGroupsForOthers)
{
   EXPECT_TRUE(Robots::parse("User-agent: other\nDisallow: /\n", "crawler").allowed("/x"));
}

TEST(Robots, MergesTheGroupsOfAnAgent)
{
   const auto robots = Robots::parse("User-agent: crawler\nDisallow: /a\n\n"
                                     "User-agent: other\nDisallow: /b\n\n"
                                     "User-agent: crawler\nDisallow: /c\n",
                                     "crawler");
   EXPECT_FALSE(robots.allowed("/a"));
   EXPECT_TRUE(robots.allowed("/b"));
   EXPECT_FALSE(robots.allowed("/c"));
}

TEST(Robots, SharesAGroupBetweenAgents)
{
   const auto robots =
      Robots::parse("User-agent: other\nUser-agent: crawler\nDisallow: /x\n", "crawler");
   EXPECT_FALSE(robots.allowed("/x"));
}

TEST(Robots, LetsTheLongestRuleDecide)
{
   const auto robots =
      Robots::parse("User-agent: *\nDisallow: /a/\nAllow: /a/b/\nDisallow: /a/b/c\n", "crawler");
   EXPECT_FALSE(robots.allowed("/a/x"));
   EXPECT_TRUE(robots.allowed("/a/b/x"));
   EXPECT_FALSE(robots.allowed("/a/b/c"));
}

TEST(Robots, PrefersAllowBetweenRulesOfTheSameLength)
{
   EXPECT_TRUE(Robots::parse("User-agent: *\nDisallow: /a\nAllow: /a\n", "crawler").allowed("/a"));
   EXPECT_TRUE(Robots::parse("User-agent: *\nAllow: /a\nDisallow: /a\n", "crawler").allowed("/a"));
}

TEST(Robots, AnEmptyDisallowDisallowsNothing)
{
   EXPECT_TRUE(Robots::parse("User-agent: *\nDisallow:\n", "crawler").allowed("/x"));
}

TEST(Robots, UnderstandsWildcards)
{
   const auto robots = Robots::parse("User-agent: *\nDisallow: /*.php\n", "crawler");
   EXPECT_FALSE(robots.allowed("/a.php"));
   EXPECT_FALSE(robots.allowed("/a/b.php?x=1"));
   EXPECT_TRUE(robots.allowed("/a.html"));
}

TEST(Robots, UnderstandsTheEndAnchor)
{
   const auto robots =
      Robots::parse("User-agent: *\nDisallow: /*.php$\nDisallow: /exact$\n", "crawler");
   EXPECT_FALSE(robots.allowed("/a.php"));
   EXPECT_FALSE(robots.allowed("/a.php.php"));
   EXPECT_TRUE(robots.allowed("/a.php.bak"));
   EXPECT_FALSE(robots.allowed("/exact"));
   EXPECT_TRUE(robots.allowed("/exactly"));
}

TEST(Robots, ComparesPathsWithCase)
{
   const auto robots = Robots::parse("User-agent: *\nDisallow: /Private\n", "crawler");
   EXPECT_FALSE(robots.allowed("/Private"));
   EXPECT_TRUE(robots.allowed("/private"));
}

TEST(Robots, ChecksTheQueryToo)
{
   const auto robots = Robots::parse("User-agent: *\nDisallow: /*?session=\n", "crawler");
   EXPECT_FALSE(robots.allowed("/a?session=1"));
   EXPECT_TRUE(robots.allowed("/a?page=1"));
}

TEST(Robots, SkipsCommentsAndNoise)
{
   const auto robots = Robots::parse("# about us\r\n"
                                     "User-agent: * # everyone\r\n"
                                     "Sitemap: https://example.com/sitemap.xml\r\n"
                                     "what is this\r\n"
                                     "Disallow: /x # not there\r\n",
                                     "crawler");
   EXPECT_FALSE(robots.allowed("/x"));
   EXPECT_TRUE(robots.allowed("/y"));
}

TEST(Robots, IgnoresRulesAheadOfAnyGroup)
{
   EXPECT_TRUE(Robots::parse("Disallow: /\nUser-agent: *\nAllow: /x\n", "crawler").allowed("/y"));
}

TEST(Robots, AlwaysAllowsItself)
{
   EXPECT_TRUE(Robots::parse("User-agent: *\nDisallow: /\n", "crawler").allowed("/robots.txt"));
}

TEST(Robots, ReadsTheCrawlDelay)
{
   EXPECT_EQ(Robots::parse("User-agent: *\nCrawl-delay: 2\n", "crawler").crawl_delay(), 2s);
   EXPECT_EQ(Robots::parse("User-agent: *\nCrawl-delay: 0.5\n", "crawler").crawl_delay(), 500ms);
   EXPECT_FALSE(Robots::parse("User-agent: *\nDisallow: /\n", "crawler").crawl_delay());
}

TEST(Robots, IgnoresACrawlDelayThatMakesNoSense)
{
   EXPECT_FALSE(Robots::parse("User-agent: *\nCrawl-delay: soon\n", "crawler").crawl_delay());
   EXPECT_FALSE(Robots::parse("User-agent: *\nCrawl-delay: -1\n", "crawler").crawl_delay());
   EXPECT_FALSE(Robots::parse("User-agent: *\nCrawl-delay: inf\n", "crawler").crawl_delay());
}

TEST(Robots, KeepsAHugeCrawlDelayToADay)
{
   EXPECT_EQ(Robots::parse("User-agent: *\nCrawl-delay: 1e30\n", "crawler").crawl_delay(), 24h);
}

TEST(Robots, TakesTheCrawlDelayOfTheChosenGroupOnly)
{
   const auto robots = Robots::parse("User-agent: other\nCrawl-delay: 9\n\n"
                                     "User-agent: crawler\nCrawl-delay: 1\n",
                                     "crawler");
   EXPECT_EQ(robots.crawl_delay(), 1s);
}
