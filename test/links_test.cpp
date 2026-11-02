#include "links.hpp"

#include <boost/url/url.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using crawler::extract_links;
using crawler::resolve_link;
using testing::ElementsAre;
using testing::IsEmpty;

TEST(ExtractLinks, FindsTheTargetsOfAnchors)
{
   EXPECT_THAT(extract_links(R"(<p><a href="/a">a</a> and <A HREF='/b'>b</A> or <a href=/c>c</a>)"),
               ElementsAre("/a", "/b", "/c"));
}

TEST(ExtractLinks, FindsTheTargetAmongOtherAttributes)
{
   EXPECT_THAT(extract_links(R"(<a class="x" disabled href = "/a" id=y>a</a>)"), ElementsAre("/a"));
}

TEST(ExtractLinks, LeavesOutOtherElements)
{
   EXPECT_THAT(extract_links(R"(<link href="/a"><img src="/b"><area href="/c"><abbr href="/d">)"),
               IsEmpty());
}

TEST(ExtractLinks, LeavesOutAnchorsWithoutATarget)
{
   EXPECT_THAT(extract_links(R"(<a name="x">x</a><a data-href="/a">a</a>)"), IsEmpty());
}

TEST(ExtractLinks, IsNotFooledByQuotes)
{
   EXPECT_THAT(extract_links(R"(<a title="<a href='/no'>" href="/yes">x</a>)"),
               ElementsAre("/yes"));
}

TEST(ExtractLinks, IsNotFooledByComments)
{
   EXPECT_THAT(extract_links(R"(<!-- <a href="/no">x</a> --><a href="/yes">y</a>)"),
               ElementsAre("/yes"));
}

TEST(ExtractLinks, IsNotFooledByScriptsAndStyles)
{
   EXPECT_THAT(extract_links(R"(<script>w('<a href="/no">');</script>)"
                             R"(<STYLE>/* <a href="/no"> */</style>)"
                             R"(<a href="/yes">y</a>)"),
               ElementsAre("/yes"));
}

TEST(ExtractLinks, HonoursNofollow)
{
   EXPECT_THAT(extract_links(R"(<a rel="noopener NoFollow" href="/no">x</a><a href="/yes">y</a>)"),
               ElementsAre("/yes"));
}

TEST(ExtractLinks, DecodesAmpersandsAndTrimsSpace)
{
   EXPECT_THAT(extract_links(R"(<a href=" /a?x=1&amp;y=2 ">a</a>)"), ElementsAre("/a?x=1&y=2"));
}

TEST(ExtractLinks, SurvivesWhatIsCutOff)
{
   EXPECT_THAT(extract_links(R"(<a href="/a)"), IsEmpty());
   EXPECT_THAT(extract_links(R"(<a href=)"), ElementsAre(""));
   EXPECT_THAT(extract_links("<a"), IsEmpty());
   EXPECT_THAT(extract_links("<"), IsEmpty());
   EXPECT_THAT(extract_links("<!-- never ends"), IsEmpty());
   EXPECT_THAT(extract_links("<script>never ends"), IsEmpty());
   EXPECT_THAT(extract_links("1 < 2 </p> <a href=/a>"), ElementsAre("/a"));
}

// -------------------------------------------------------------------------------------------------

namespace
{
std::string resolved(std::string_view reference)
{
   const auto url = resolve_link(boost::urls::url("http://example.com/dir/page?q=1"), reference);
   return url ? std::string(url->buffer()) : "(none)";
}
} // namespace

TEST(ResolveLink, ResolvesAgainstTheBase)
{
   EXPECT_EQ(resolved("other"), "http://example.com/dir/other");
   EXPECT_EQ(resolved("/top"), "http://example.com/top");
   EXPECT_EQ(resolved("../up"), "http://example.com/up");
   EXPECT_EQ(resolved("?x=2"), "http://example.com/dir/page?x=2");
   EXPECT_EQ(resolved("//other.example/"), "http://other.example/");
   EXPECT_EQ(resolved("https://other.example/x"), "https://other.example/x");
}

TEST(ResolveLink, MakesTheSameOfTheSamePage)
{
   EXPECT_EQ(resolved("/a#section"), "http://example.com/a");
   EXPECT_EQ(resolved("#section"), "http://example.com/dir/page?q=1");
   EXPECT_EQ(resolved("HTTP://EXAMPLE.com"), "http://example.com/");
   EXPECT_EQ(resolved("/x/../a"), "http://example.com/a");
}

TEST(ResolveLink, TakesNothingButHttpAndHttps)
{
   EXPECT_EQ(resolved("mailto:someone@example.com"), "(none)");
   EXPECT_EQ(resolved("javascript:alert(1)"), "(none)");
   EXPECT_EQ(resolved("ftp://example.com/"), "(none)");
   EXPECT_EQ(resolved("data:text/html,x"), "(none)");
}

TEST(ResolveLink, TakesNoUserInformation)
{
   EXPECT_EQ(resolved("http://user:secret@example.com/"), "(none)");
}

TEST(ResolveLink, TakesNothingThatDoesNotParse)
{
   EXPECT_EQ(resolved("http://exa mple.com/"), "(none)");
}
