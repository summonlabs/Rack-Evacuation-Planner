// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Tests for SHA-256 (FIPS 180-4) and for the Digest value type.
//
// The expected digests are the published NIST/FIPS 180-4 vectors, cross
// checked against an independent implementation; the million-'a' vector is the
// one printed in the FIPS 180-4 example set.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/digest.hpp"

#include "testkit.hpp"

namespace {

[[nodiscard]] std::string sha256_hex(std::string_view text) {
  return rep::sha256_of(text).to_hex();
}

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// Hashes a message by feeding it in fixed size chunks, so the buffering and
// block boundaries of the streaming implementation are exercised.
[[nodiscard]] std::string sha256_chunked_hex(std::string_view text, std::size_t chunk) {
  rep::Sha256 hasher;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t remaining = text.size() - offset;
    const std::size_t take = remaining < chunk ? remaining : chunk;
    hasher.update(text.substr(offset, take));
    offset += take;
  }
  return rep::Digest{hasher.finish()}.to_hex();
}

// Hashes a message by feeding it as byte spans in fixed size chunks.
[[nodiscard]] std::string sha256_chunked_bytes_hex(std::string_view text, std::size_t chunk) {
  rep::Sha256 hasher;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t remaining = text.size() - offset;
    const std::size_t take = remaining < chunk ? remaining : chunk;
    hasher.update(as_bytes(text.substr(offset, take)));
    offset += take;
  }
  return rep::Digest{hasher.finish()}.to_hex();
}

// A deterministic, non-repeating-looking payload built without any clock or
// random source.
[[nodiscard]] std::string patterned(std::size_t length) {
  std::string text;
  text.reserve(length);
  std::uint32_t state = 0x12345678u;
  for (std::size_t i = 0; i < length; ++i) {
    state = state * 1664525u + 1013904223u;
    text.push_back(static_cast<char>('a' + ((state >> 16u) % 26u)));
  }
  return text;
}

constexpr std::string_view kEmptyHex =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view kAbcHex =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view kTwoBlockHex =
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

}  // namespace

REP_TEST(Digest, PublishedFipsVectors) {
  REP_CHECK_EQ(sha256_hex(""), std::string(kEmptyHex));
  REP_CHECK_EQ(sha256_hex("abc"), std::string(kAbcHex));
  REP_CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
               std::string(kTwoBlockHex));
  // The 896-bit two-block message from the FIPS 180-4 example set.
  REP_CHECK_EQ(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                          "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
               std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
  // A 64-byte message: exactly one full block, so the length fills its own
  // padding block.
  REP_CHECK_EQ(sha256_hex(std::string(64, 'b')),
               std::string("a0fab1377f49a759b57f63318262ebe89fabfc990e8e93ceac2984561482b9d4"));
}

REP_TEST(Digest, PublishedRepeatedA_Vectors) {
  // These are the classic one-character boundary vectors: 55 and 56 'a'
  // bracket the 56-byte padding boundary inside the first block.
  REP_CHECK_EQ(sha256_hex(std::string(55, 'a')),
               std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
  REP_CHECK_EQ(sha256_hex(std::string(56, 'a')),
               std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
  REP_CHECK_EQ(sha256_hex(std::string(57, 'a')),
               std::string("f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"));
  REP_CHECK_EQ(sha256_hex(std::string(63, 'a')),
               std::string("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"));
  REP_CHECK_EQ(sha256_hex(std::string(64, 'a')),
               std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
  REP_CHECK_EQ(sha256_hex(std::string(65, 'a')),
               std::string("635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"));
  REP_CHECK_EQ(sha256_hex(std::string(1000, 'a')),
               std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

REP_TEST(Digest, PublishedOneMillionA_Vector) {
  const std::string message(1000000, 'a');
  const std::string expected =
      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
  REP_CHECK_EQ(sha256_hex(message), expected);
  REP_CHECK_EQ(sha256_chunked_hex(message, 997), expected);
}

REP_TEST(Digest, IncrementalChunkingMatchesOneShot) {
  const std::size_t chunk_sizes[] = {1, 7, 63, 64, 65, 127};
  const std::size_t lengths[] = {0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 128, 129, 1000};
  for (const std::size_t length : lengths) {
    const std::string message = patterned(length);
    const std::string one_shot = sha256_hex(message);
    for (const std::size_t chunk : chunk_sizes) {
      REP_CHECK_EQ(sha256_chunked_hex(message, chunk), one_shot);
      REP_CHECK_EQ(sha256_chunked_bytes_hex(message, chunk), one_shot);
    }
  }
  // A single-chunk split of a message shorter than one block.
  REP_CHECK_EQ(sha256_chunked_hex("abc", 1024), std::string(kAbcHex));
}

REP_TEST(Digest, IncrementalSplitCrossesPaddingBoundary) {
  // Every two-part split of every message below, including the splits at 55,
  // 56 and 57 bytes that land on and around the length field of the final
  // block.  A one-shot hash of the same bytes must agree in every case.
  const std::size_t lengths[] = {55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129};
  for (const std::size_t length : lengths) {
    const std::string message = patterned(length);
    const std::string one_shot = sha256_hex(message);
    for (std::size_t split = 0; split <= length; ++split) {
      rep::Sha256 hasher;
      hasher.update(std::string_view(message).substr(0, split));
      hasher.update(std::string_view(message).substr(split));
      REP_CHECK_EQ(rep::Digest{hasher.finish()}.to_hex(), one_shot);
    }
  }
  // Three parts, with an empty middle section, around a block boundary.
  const std::string message = patterned(200);
  rep::Sha256 hasher;
  hasher.update(std::string_view(message).substr(0, 56));
  hasher.update(std::string_view(message).substr(56, 0));
  hasher.update(std::string_view(message).substr(56, 64));
  hasher.update(std::string_view(message).substr(120));
  REP_CHECK_EQ(rep::Digest{hasher.finish()}.to_hex(), sha256_hex(message));
}

REP_TEST(Digest, ByteSpanAndStringViewOverloadsAgree) {
  const std::string message = patterned(130);
  rep::Sha256 from_text;
  from_text.update(std::string_view(message));
  rep::Sha256 from_bytes;
  from_bytes.update(as_bytes(message));
  REP_CHECK_EQ(rep::Digest{from_bytes.finish()}, rep::Digest{from_text.finish()});
  REP_CHECK_EQ(rep::sha256_of(message), rep::sha256_of(as_bytes(message)));
  // An empty update is a no-op and must not disturb the state.
  rep::Sha256 with_empty;
  with_empty.update(std::string_view());
  with_empty.update(std::span<const std::byte>());
  with_empty.update("abc");
  REP_CHECK_EQ(rep::Digest{with_empty.finish()}.to_hex(), std::string(kAbcHex));
}

REP_TEST(Digest, FinishIsSingleUse) {
  rep::Sha256 hasher;
  hasher.update("abc");
  const rep::Digest first{hasher.finish()};
  REP_CHECK_EQ(first.to_hex(), std::string(kAbcHex));
  // The state was consumed: a second finish reports the all-zero digest and
  // further updates are ignored.
  hasher.update("more bytes");
  REP_CHECK(rep::Digest{hasher.finish()}.is_zero());
}

REP_TEST(Digest, HexRoundTripAndStrictDecoding) {
  const rep::Digest computed = rep::sha256_of("abc");
  const std::string text = computed.to_hex();
  REP_CHECK_EQ(text.size(), std::size_t{64});
  const auto parsed = rep::Digest::from_hex(text);
  REP_REQUIRE(parsed.has_value());
  REP_CHECK_EQ(*parsed, computed);

  std::array<char, 64> scratch{};
  REP_CHECK_EQ(computed.to_hex(scratch), std::string_view(text));

  // Uppercase hex decodes to the same digest.
  std::string upper = text;
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  const auto parsed_upper = rep::Digest::from_hex(upper);
  REP_REQUIRE(parsed_upper.has_value());
  REP_CHECK_EQ(*parsed_upper, computed);
  REP_CHECK_EQ(parsed_upper->to_hex(), text);

  // Wrong lengths.
  REP_CHECK(!rep::Digest::from_hex("").has_value());
  REP_CHECK(!rep::Digest::from_hex(std::string(63, '0')).has_value());
  REP_CHECK(!rep::Digest::from_hex(std::string(65, '0')).has_value());
  REP_CHECK(!rep::Digest::from_hex(text + "00").has_value());
  REP_CHECK(!rep::Digest::from_hex("0x" + text.substr(2)).has_value());

  // Non-hex characters in either nibble, including a trailing newline.
  std::string bad_high = text;
  bad_high[0] = 'g';
  REP_CHECK(!rep::Digest::from_hex(bad_high).has_value());
  std::string bad_low = text;
  bad_low[1] = 'z';
  REP_CHECK(!rep::Digest::from_hex(bad_low).has_value());
  std::string bad_last = text;
  bad_last[63] = ' ';
  REP_CHECK(!rep::Digest::from_hex(bad_last).has_value());
  REP_CHECK(!rep::Digest::from_hex(text + "\n").has_value());
  std::string bad_middle = text;
  bad_middle[32] = '-';
  REP_CHECK(!rep::Digest::from_hex(bad_middle).has_value());

  // The all-zero digest round trips and is recognised.
  const auto zeros = rep::Digest::from_hex(std::string(64, '0'));
  REP_REQUIRE(zeros.has_value());
  REP_CHECK(zeros->is_zero());
  REP_CHECK_EQ(zeros->to_hex(), std::string(64, '0'));
}

REP_TEST(Digest, ZeroDigestDetection) {
  const rep::Digest zero{};
  REP_CHECK(zero.is_zero());
  REP_CHECK_EQ(zero.to_hex(), std::string(64, '0'));

  rep::Digest one_byte_set{};
  one_byte_set.bytes[31] = 1;
  REP_CHECK(!one_byte_set.is_zero());
  one_byte_set.bytes[31] = 0;
  one_byte_set.bytes[0] = 0x80;
  REP_CHECK(!one_byte_set.is_zero());

  REP_CHECK(!rep::sha256_of("").is_zero());
  REP_CHECK(!rep::sha256_of("abc").is_zero());

  // Ordering is by the raw byte array, so the zero digest sorts first and
  // the first differing byte decides.
  const auto low = rep::Digest::from_hex(
      "0000000000000000000000000000000000000000000000000000000000000001");
  const auto high = rep::Digest::from_hex(
      "0100000000000000000000000000000000000000000000000000000000000000");
  REP_REQUIRE(low.has_value());
  REP_REQUIRE(high.has_value());
  REP_CHECK(zero < *low);
  REP_CHECK(*low < *high);
  REP_CHECK(rep::sha256_of("") != zero);
}

REP_TEST(Digest, DomainSeparation) {
  const rep::Digest bare = rep::sha256_of("abc");
  const rep::Digest tagged = rep::CanonicalWriter("tag").text("abc").finish();
  REP_CHECK_NE(bare, tagged);

  // The same bytes under two different tags never collide, even when one tag
  // is a prefix of the other: the tag length is part of the hash input.
  const rep::Digest tag_a = rep::CanonicalWriter("a").text("bc").finish();
  const rep::Digest tag_ab = rep::CanonicalWriter("ab").text("c").finish();
  const rep::Digest tag_abc = rep::CanonicalWriter("abc").text("").finish();
  REP_CHECK_NE(tag_a, tag_ab);
  REP_CHECK_NE(tag_ab, tag_abc);
  REP_CHECK_NE(tag_a, tag_abc);

  // The empty tag is a domain of its own, and the tag is really hashed: an
  // untagged encode of the same payload differs.
  const rep::Digest empty_tag = rep::CanonicalWriter("").text("abc").finish();
  REP_CHECK_NE(empty_tag, bare);
  REP_CHECK_NE(empty_tag, tagged);

  // Determinism: the same tag and payload always produce the same digest.
  REP_CHECK_EQ(rep::CanonicalWriter("tag").text("abc").finish(), tagged);
  REP_CHECK_EQ(rep::CanonicalWriter("tag").text("abc").to_hex(),
               rep::CanonicalWriter("tag").text("abc").to_hex());
}

REP_TEST_MAIN()
