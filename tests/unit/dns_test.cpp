// youtube-tv - DNS query building and answer reading (CNAME chains included).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/dns.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <cstring>
#include <vector>

namespace
{

// www.youtube.com -> CNAME youtube-ui.l.google.com -> two A records, with the
// name compression real resolvers use.
const std::vector<std::uint8_t> kYouTubeAnswer = {
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x03, 0x77, 0x77,
    0x77, 0x07, 0x79, 0x6f, 0x75, 0x74, 0x75, 0x62, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00,
    0x01, 0x00, 0x01, 0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x19,
    0x0a, 0x79, 0x6f, 0x75, 0x74, 0x75, 0x62, 0x65, 0x2d, 0x75, 0x69, 0x01, 0x6c, 0x06, 0x67,
    0x6f, 0x6f, 0x67, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00, 0xc0, 0x2d, 0x00, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x04, 0x8e, 0xfa, 0x48, 0x6e, 0xc0, 0x2d, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x01, 0x2c, 0x00, 0x04, 0x8e, 0xfa, 0x41, 0x4e};

TEST(Dns, BuildsAnAQueryWithRecursion)
{
    std::uint8_t query[DNS_MAX_PACKET];
    const int length = dns_build_query(query, sizeof(query), 0xBEEF, "www.youtube.com");
    ASSERT_EQ(length, 12 + 17 + 4);
    EXPECT_EQ(query[0], 0xBE);
    EXPECT_EQ(query[1], 0xEF);
    EXPECT_EQ(query[2], 0x01); // RD
    EXPECT_EQ(query[5], 1);    // one question
    EXPECT_EQ(std::memcmp(query + 12,
                          "\x03www\x07youtube\x03"
                          "com\0",
                          17),
              0);
    EXPECT_EQ(query[length - 3], 1); // type A
    EXPECT_EQ(query[length - 1], 1); // class IN
}

TEST(Dns, RejectsBadNames)
{
    std::uint8_t query[DNS_MAX_PACKET];
    EXPECT_EQ(dns_build_query(query, sizeof(query), 1, ""), -1);
    EXPECT_EQ(dns_build_query(query, sizeof(query), 1, "a..b"), -1);
    EXPECT_EQ(dns_build_query(query, sizeof(query), 1, "bad host"), -1);
    EXPECT_EQ(dns_build_query(query, 20, 1, "www.youtube.com"), -1);
}

TEST(Dns, ReadsTheAddressesBehindACnameChain)
{
    std::uint32_t addresses[4] = {};
    const int found =
        dns_parse_a(kYouTubeAnswer.data(), kYouTubeAnswer.size(), 0x1234, addresses, 4);
    ASSERT_EQ(found, 2);
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addresses[0], text, sizeof(text));
    EXPECT_STREQ(text, "142.250.72.110");
    inet_ntop(AF_INET, &addresses[1], text, sizeof(text));
    EXPECT_STREQ(text, "142.250.65.78");
}

TEST(Dns, RefusesForeignTruncatedAndFailedAnswers)
{
    std::uint32_t addresses[4] = {};
    EXPECT_EQ(dns_parse_a(kYouTubeAnswer.data(), kYouTubeAnswer.size(), 0x9999, addresses, 4), -1);
    // Cut inside the CNAME record: nothing usable yet.
    EXPECT_EQ(dns_parse_a(kYouTubeAnswer.data(), 40, 0x1234, addresses, 4), -1);
    // Cut inside the second A record: the first one is still read.
    EXPECT_EQ(dns_parse_a(kYouTubeAnswer.data(), kYouTubeAnswer.size() - 2, 0x1234, addresses, 4),
              1);
    std::vector<std::uint8_t> nxdomain(kYouTubeAnswer.begin(), kYouTubeAnswer.begin() + 33);
    nxdomain[3] = 0x83; // rcode 3
    nxdomain[7] = 0;    // no answers
    EXPECT_EQ(dns_parse_a(nxdomain.data(), nxdomain.size(), 0x1234, addresses, 4), -5);
    std::vector<std::uint8_t> loop = {0x12, 0x34, 0x81, 0x80, 0, 1, 0, 1, 0, 0, 0, 0, 0xC0, 0x0C};
    EXPECT_EQ(dns_parse_a(loop.data(), loop.size(), 0x1234, addresses, 4), -1);
}

} // namespace
