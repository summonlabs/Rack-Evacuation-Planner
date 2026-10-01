// Rack Evacuation Planner - DCCP boundary 55
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Tests for the canonical byte encoding: fixed width little endian integers,
// explicit booleans, length prefixed variable length data, domain separated
// digests, and the container helper.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "rep/canonical.hpp"
#include "rep/digest.hpp"
#include "rep/obligation.hpp"
#include "rep/types.hpp"

#include "testkit.hpp"

namespace {

[[nodiscard]] std::string hex_of_writer(rep::CanonicalWriter& writer) { return writer.to_hex(); }

template <class T>
[[nodiscard]] std::string encoded_hex(std::string_view tag, const T& value) {
  rep::CanonicalWriter writer(tag);
  value.encode(writer);
  return writer.to_hex();
}

template <class T>
[[nodiscard]] rep::Digest encoded_digest(std::string_view tag, const T& value) {
  rep::CanonicalWriter writer(tag);
  value.encode(writer);
  return writer.finish();
}

// A minimal record type used to exercise encode_sequence() without going
// through a library payload.
struct Item {
  std::uint16_t code{0};
  std::string name;

  void encode(rep::CanonicalWriter& writer) const {
    writer.u16(code);
    writer.text(name);
  }
  friend bool operator==(const Item& a, const Item& b) { return a.code == b.code && a.name == b.name; }
};

}  // namespace

REP_TEST(Canonical, IntegerWidthsAreLittleEndianAndFixed) {
  const std::uint64_t u64_max = (std::numeric_limits<std::uint64_t>::max)();
  const std::uint32_t u32_max = (std::numeric_limits<std::uint32_t>::max)();
  const auto i32_min = (std::numeric_limits<std::int32_t>::min)();
  const auto i32_max = (std::numeric_limits<std::int32_t>::max)();
  const auto i64_min = (std::numeric_limits<std::int64_t>::min)();
  const auto i64_max = (std::numeric_limits<std::int64_t>::max)();

  {
    rep::CanonicalWriter writer("t");
    writer.u8(0).u8(1).u8(0x80).u8(0xff);
    REP_CHECK_EQ(writer.size(), std::size_t{4});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("000180ff"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.u16(0).u16(1).u16(0x0102).u16(0xffff);
    REP_CHECK_EQ(writer.size(), std::size_t{8});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("000001000201ffff"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.u32(0).u32(1).u32(0x01020304).u32(u32_max);
    REP_CHECK_EQ(writer.size(), std::size_t{16});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("000000000100000004030201ffffffff"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.u64(0).u64(1).u64(0x0102030405060708ull).u64(u64_max);
    REP_CHECK_EQ(writer.size(), std::size_t{32});
    REP_CHECK_EQ(hex_of_writer(writer),
                 std::string("000000000000000001000000000000000807060504030201ffffffffffffffff"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.i32(0).i32(-1).i32(-2).i32(i32_min).i32(i32_max);
    REP_CHECK_EQ(writer.size(), std::size_t{20});
    REP_CHECK_EQ(hex_of_writer(writer),
                 std::string("00000000fffffffffeffffff00000080ffffff7f"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.i64(0).i64(-1).i64(i64_min).i64(i64_max);
    REP_CHECK_EQ(writer.size(), std::size_t{32});
    REP_CHECK_EQ(hex_of_writer(writer),
                 std::string("0000000000000000ffffffffffffffff0000000000000080"
                             "ffffffffffffff7f"));
  }
}

REP_TEST(Canonical, BooleansAreOneByte) {
  {
    rep::CanonicalWriter writer("t");
    writer.boolean(false).boolean(true);
    REP_CHECK_EQ(writer.size(), std::size_t{2});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("0001"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.boolean(true);
    REP_CHECK_EQ(writer.size(), std::size_t{1});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("01"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.boolean(false);
    REP_CHECK_EQ(hex_of_writer(writer), std::string("00"));
  }
}

REP_TEST(Canonical, TextAndBytesAreLengthPrefixed) {
  {
    rep::CanonicalWriter writer("t");
    writer.text("");
    REP_CHECK_EQ(writer.size(), std::size_t{8});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("0000000000000000"));
  }
  {
    rep::CanonicalWriter writer("t");
    writer.text("abc");
    REP_CHECK_EQ(writer.size(), std::size_t{11});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("0300000000000000616263"));
  }
  {
    // The count is a 64-bit value even for a payload that needs one byte.
    rep::CanonicalWriter writer("t");
    writer.text("a");
    REP_CHECK_EQ(hex_of_writer(writer), std::string("010000000000000061"));
  }
  {
    // 300 bytes: 0x012c little endian.
    const std::string long_text(300, 'x');
    rep::CanonicalWriter writer("t");
    writer.text(long_text);
    REP_CHECK_EQ(writer.size(), std::size_t{308});
    REP_CHECK_EQ(hex_of_writer(writer).substr(0, 16), std::string("2c01000000000000"));
    REP_CHECK_EQ(hex_of_writer(writer).substr(16, 8), std::string("78787878"));
  }
  {
    const std::vector<std::byte> blob{std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}};
    rep::CanonicalWriter writer("t");
    writer.bytes(blob);
    REP_CHECK_EQ(writer.size(), std::size_t{11});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("030000000000000000ff7f"));
  }
  {
    const std::vector<std::byte> blob{std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}};
    rep::CanonicalWriter writer("t");
    writer.raw(blob);
    REP_CHECK_EQ(writer.size(), std::size_t{3});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("00ff7f"));
  }
  {
    const std::vector<std::byte> empty;
    rep::CanonicalWriter writer("t");
    writer.bytes(empty).byte(std::byte{0x7f});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("00000000000000007f"));
  }
  {
    // A digest is written raw: 32 bytes, no length prefix.
    rep::Digest value{};
    for (std::size_t i = 0; i < value.bytes.size(); ++i) {
      value.bytes[i] = static_cast<std::uint8_t>(i);
    }
    rep::CanonicalWriter writer("t");
    writer.digest(value);
    REP_CHECK_EQ(writer.size(), std::size_t{32});
    REP_CHECK_EQ(hex_of_writer(writer),
                 std::string("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"));
  }
}

REP_TEST(Canonical, DomainTagIsLengthPrefixedAndHashed) {
  // Identical encoded bytes under two tags, one of which is a prefix of the
  // other: the u64 tag length in front of the tag keeps them apart.
  rep::CanonicalWriter short_tag("a");
  short_tag.text("b");
  rep::CanonicalWriter long_tag("ab");
  long_tag.text("b");
  REP_CHECK_EQ(short_tag.to_hex(), long_tag.to_hex());
  REP_CHECK_NE(short_tag.finish(), long_tag.finish());

  rep::CanonicalWriter empty_tag("");
  empty_tag.text("b");
  REP_CHECK_NE(empty_tag.finish(), short_tag.finish());

  // The tag is hashed with its length: hashing tag || payload without the
  // length would be ambiguous with hashing a longer tag.
  rep::CanonicalWriter tagged("abc");
  tagged.text("def");
  const rep::Digest without_length = rep::sha256_of("abcdef");
  const rep::Digest with_length = tagged.finish();
  REP_CHECK_NE(without_length, with_length);

  // The tag is part of the writer's observable identity.
  REP_CHECK_EQ(tagged.domain_tag(), std::string("abc"));
  REP_CHECK_EQ(short_tag.domain_tag(), std::string("a"));
  REP_CHECK(empty_tag.domain_tag().empty());

  // finish() is const: it neither consumes nor perturbs the buffer, and bytes
  // written after a finish() are included in the next digest.
  const rep::Digest first = tagged.finish();
  const std::vector<std::byte> bytes_before = tagged.buffer();
  REP_CHECK_EQ(tagged.finish(), first);
  REP_CHECK_EQ(tagged.buffer(), bytes_before);
  tagged.text("");
  REP_CHECK_NE(tagged.finish(), first);
}

REP_TEST(Canonical, DeterminismAndIndependenceFromHistory) {
  const rep::Digest digest_a = encoded_digest("rep.test.det.v1", rep::DestinationRef{
                                                                     rep::DestinationKind::RackSlot,
                                                                     rep::DestinationId("rack-1")});
  const rep::Digest digest_b = encoded_digest("rep.test.det.v1", rep::DestinationRef{
                                                                     rep::DestinationKind::RackSlot,
                                                                     rep::DestinationId("rack-1")});
  REP_CHECK_EQ(digest_a, digest_b);

  // Interleaving unrelated encodings does not change the result.
  rep::DestinationRef first{rep::DestinationKind::FabricEndpoint, rep::DestinationId("fc-9")};
  const std::string hex_first = encoded_hex("rep.test.det.v1", first);
  rep::CanonicalWriter noise("rep.test.other.v1");
  noise.text("unrelated").u64(12345).boolean(true);
  REP_CHECK(!noise.to_hex().empty());
  const std::string hex_second = encoded_hex("rep.test.det.v1", first);
  REP_CHECK_EQ(hex_first, hex_second);

  // The same value reached by two different construction paths encodes
  // identically: parsed and hand built.
  const auto parsed = rep::DestinationRef::parse("fabric_endpoint:fc-9");
  REP_REQUIRE(parsed.ok());
  REP_CHECK_EQ(encoded_hex("rep.test.det.v1", parsed.value()), hex_first);
  REP_CHECK_EQ(encoded_digest("rep.test.det.v1", parsed.value()),
               encoded_digest("rep.test.det.v1", first));

  // A copy encodes like the original, and encoding does not mutate the value.
  const rep::DestinationRef copy = first;
  REP_CHECK_EQ(encoded_hex("rep.test.det.v1", copy), hex_first);
  REP_CHECK_EQ(first, copy);
}

REP_TEST(Canonical, EncodeSequenceWritesCountThenElements) {
  {
    const std::vector<Item> empty;
    rep::CanonicalWriter writer("t");
    rep::encode_sequence(writer, empty);
    REP_CHECK_EQ(writer.size(), std::size_t{8});
    REP_CHECK_EQ(hex_of_writer(writer), std::string("0000000000000000"));
  }
  {
    const std::vector<Item> items{{1, "a"}, {2, "b"}};
    rep::CanonicalWriter writer("t");
    rep::encode_sequence(writer, items);
    REP_CHECK_EQ(hex_of_writer(writer),
                 std::string("0200000000000000" "0100" "010000000000000061" "0200"
                             "010000000000000062"));
  }
  {
    const std::vector<Item> single{{7, ""}};
    rep::CanonicalWriter writer("t");
    rep::encode_sequence(writer, single);
    REP_CHECK_EQ(hex_of_writer(writer), std::string("010000000000000007000000000000000000"));
  }
  {
    // encode_sequence over a library type: the ids are written in the order
    // they appear, each as a length prefixed text.
    const auto first = rep::ObligationId::parse("a");
    const auto second = rep::ObligationId::parse("bb");
    REP_REQUIRE(first.ok());
    REP_REQUIRE(second.ok());
    const std::vector<rep::ObligationId> ids{second.value(), first.value()};
    rep::CanonicalWriter writer("t");
    rep::encode_sequence(writer, ids);
    REP_CHECK_EQ(hex_of_writer(writer),
                 std::string("0200000000000000" "02000000000000006262" "010000000000000061"));
  }
}

REP_TEST(Canonical, ByteForByteRegressionPin) {
  // Regression pin.  The literals below are the canonical bytes and the
  // definition digest that this library produced when the test was written;
  // they are recorded here so that any accidental change to the encoding of
  // an obligation (text, enum, resource vector, boolean, sequence) fails
  // loudly instead of silently invalidating every stored digest.  The digest
  // was reproduced by an independent SHA-256 over
  // u64le(len(tag)) || tag || bytes, so the pin also documents the domain
  // separation rule.  Byte layout: text "ob-1", u16 workload, text "rack-a",
  // u64 count 2 {cpu_millicores:4, memory_bytes:1024}, boolean true,
  // u64 count 1 {text "ob-0"}.
  const auto demand = rep::ResourceVector::make(
      {{rep::ResourceClass::CpuMillicores, 4}, {rep::ResourceClass::MemoryBytes, 1024}});
  REP_REQUIRE(demand.ok());
  const auto obligation =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), demand.value(), true,
                            {rep::ObligationId("ob-0")});
  REP_REQUIRE(obligation.ok());

  rep::CanonicalWriter writer(rep::domains::kObligation);
  obligation.value().encode(writer);
  REP_CHECK_EQ(writer.to_hex(),
               std::string("04000000000000006f622d31010006000000000000007261636b2d61"
                           "0200000000000000010004000000000000000200000400000000000001"
                           "010000000000000004000000000000006f622d30"));
  REP_CHECK_EQ(obligation.value().definition_digest().to_hex(),
               std::string("e646368bb33ea2f6213b6bce6452d72d471d0c52a402e81cc921062ffe0a2c48"));

  // The pin is reproducible: re-encoding the same value gives the same bytes.
  REP_CHECK_EQ(encoded_hex(rep::domains::kObligation, obligation.value()), writer.to_hex());
  REP_CHECK_EQ(encoded_digest(rep::domains::kObligation, obligation.value()),
               obligation.value().definition_digest());

  // A single field change moves both the bytes and the digest.
  const auto changed =
      rep::Obligation::make(rep::ObligationId("ob-1"), rep::ObligationKind::Workload,
                            rep::RackId("rack-a"), demand.value(), false,
                            {rep::ObligationId("ob-0")});
  REP_REQUIRE(changed.ok());
  REP_CHECK_NE(encoded_hex(rep::domains::kObligation, changed.value()), writer.to_hex());
  REP_CHECK_NE(changed.value().definition_digest(), obligation.value().definition_digest());
}

REP_TEST_MAIN()
