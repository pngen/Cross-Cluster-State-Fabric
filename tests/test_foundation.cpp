// Cross-Cluster State Fabric - foundation proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ccsf/bytes.hpp"
#include "ccsf/cost.hpp"
#include "ccsf/crc32c.hpp"
#include "ccsf/digest.hpp"
#include "ccsf/error.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/taxonomy.hpp"
#include "framework.hpp"

namespace {

std::vector<std::byte> pattern(std::uint64_t length, std::uint64_t multiplier,
                               std::uint64_t addend) {
  std::vector<std::byte> data(static_cast<std::size_t>(length));
  for (std::uint64_t index = 0; index < length; ++index) {
    data[static_cast<std::size_t>(index)] =
        static_cast<std::byte>((index * multiplier + addend) % 256U);
  }
  return data;
}

std::vector<std::byte> from_text(const std::string& text) {
  std::vector<std::byte> data(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    data[index] = static_cast<std::byte>(static_cast<unsigned char>(text[index]));
  }
  return data;
}

}  // namespace

CCSF_CASE(sha256, rfc_vectors) {
  const std::array<std::pair<std::string, std::string>, 4> vectors{{
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {"The quick brown fox jumps over the lazy dog",
       "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
  }};
  for (const auto& vector : vectors) {
    const std::vector<std::byte> data = from_text(vector.first);
    ctx.check(ccsf::sha256(data).hex() == vector.second, "sha256 mismatch for RFC vector");
  }
}

CCSF_CASE(sha256, million_a) {
  std::vector<std::byte> data(1000000, std::byte{0x61});
  ctx.check(ccsf::sha256(data).hex() ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "sha256 of one million 'a' bytes is wrong");
}

CCSF_CASE(sha256, padding_boundaries) {
  struct Vector {
    std::uint64_t length;
    const char* hex;
  };
  const std::array<Vector, 28> vectors{{
      {0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {1U, "ca358758f6d27e6cf45272937977a748fd88391db679ceda7dc7bf1f005ee879"},
      {2U, "140d811b81973993df99b8b1742b383ab83f6f52bf7af850812e7bba02ff11da"},
      {3U, "647674a296197442f518bcca323ec605dd8d098b2d4f22ee1fdcdd2bb753a189"},
      {31U, "5e5f9fa56d6337115e86a2508477e87c7d5296d0b0743ecfde2d0caeed2db37d"},
      {32U, "8e889f10b21cdd1b3ad72f740317a827d76e1b5b3f721e33c566f06d1deff8ea"},
      {33U, "93462e85c42aa037bc727a8c283497594aa5c844f0e5bccaa52fec34dee9f44e"},
      {54U, "c802146d5788fb540fbf29d8ff485730ad10f4f13b78961c032e78691b582647"},
      {55U, "8aa994584139d128848eeebc4e815639ba5ab6e6e39574195a63ac4f14f7c43b"},
      {56U, "ad574708f75c044c9b85de64cb568ee7711ff4f36448c6242f053ba8f6cc2b63"},
      {57U, "5b46e502092be01b1100193e089fdda95638c12e19a1d24f308eb2c3d3ae849d"},
      {58U, "b077ebeb8236a3aadb7f9f3fac9bf78df7e2ae0e8ca49d19f36914c66c2421ea"},
      {63U, "280ed3e8ff1df845b2e7dfe6ac6cee817bef20e783cc65abc41b818b4d2fe076"},
      {64U, "c6ab9724ade5b6a7a1edfffb12f3aa9181351355af8fd08c919952ad211339dd"},
      {65U, "788367c73c7ddf4c53f65e68cc0d943e6227ab55b0e78ba63ace822b1c6301c0"},
      {66U, "c24f29299c40d868cd7b1f5de6af00827b1a8454ed22256f8fad0a23651d8cb3"},
      {119U, "3d610547d68216dedf7435a4fb6260353911f6b3fd3f18805ddb8be285d726fe"},
      {120U, "1f80156a804cb7862ad113e8200e9d74499723e7c7854d5f48776d3148e09656"},
      {127U, "192409cd280e14b743642ad1343fbd3e82d9305de72c078117745a679210cc3d"},
      {128U, "cc548ca2dec1f6fe4f58b2e27aa9c7521607df1130d140b55a4dad0665302356"},
      {129U, "81e89a7b2911aaa7795f9e3d4910cb47d6cd2b00d83b8399481527261a1a7519"},
      {191U, "2a30958d124d569d0a4832c608c772181557edbae684ff368be6592d3bf500c7"},
      {192U, "6e3a9b4ecba7af3a46e4f5c90fe02c99b5715144444b38049a42ac8313b30346"},
      {193U, "87746ac61c76c535aff44356de173c446cae1a259ea678f42af728c00b9aa287"},
      {255U, "c9241bc45a34a7e4028b10346f6edc6336c11c4e2484dff4e4cff74a003b6da1"},
      {256U, "c8c6e02d597fa6c407a5fec30c981c7bbad08972240eea89841b8f37e2fbf32c"},
      {257U, "442d170f22f13ae4fc17edbd88c498df9072b962790944f588ffaeee2a9ec0eb"},
      {1000U, "5097e7d587352f5097062ae679f37bda5802d9f875aba14c8cb4d1a188ada179"},
  }};
  for (const Vector& vector : vectors) {
    const std::vector<std::byte> data = pattern(vector.length, 31U, 7U);
    const std::string actual = ccsf::sha256(data).hex();
    if (actual != vector.hex) {
      ctx.check(false, "sha256 boundary mismatch at length " + std::to_string(vector.length) +
                           ": got " + actual + " want " + vector.hex);
    }
  }
}

CCSF_CASE(sha256, streaming_matches_one_shot) {
  struct Vector {
    std::uint64_t length;
    const char* hex;
  };
  const std::array<Vector, 14> vectors{{
      {0U, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {1U, "e7cf46a078fed4fafd0b5e3aff144802b853f8ae459a4f0c14add3314b7cc3a6"},
      {55U, "4559b8ac3cdcaf0dd666ad4c5181036a9752e87f216636a5399687a9f46179d2"},
      {56U, "bb2a337953dd5e50b427f00817e6fb99255b52c9aa3a9455a3c69978c3b677d7"},
      {57U, "81d344a269f719051b6fa2f07e1f23ce515d9a800455b29d404a48ef2e3e0efe"},
      {63U, "9789851884035c3f7d5cd8aab3b59964e8f215a1fc422ffa9cf73ac306d6dc14"},
      {64U, "882dac4170fae8746454b9b9abf5b967c32e2ae9875ed519e5a6b36791def2a3"},
      {65U, "86d286ff4245451f5004033c84486b61193048bd9216f7d8bf8291b4a8b85b8c"},
      {127U, "82cd7050896b7550166b0a274b13825e827809448d878bb7c11be093e4192ebf"},
      {128U, "096f884000c5e8fe2b476ecbffd05b35a90da240b9cd8e9685d8885f00cf2135"},
      {129U, "e8a15b310aeb634614d02010d3eeb2af3ad70f37d637f4f00c8d0406cfdcd82e"},
      {1000U, "a50cebe86421f168486c38783480ce41812b1d91c302f6a122209b9045b83d63"},
      {4096U, "9a897a25a59d9ae6b380c47ec1e608bd1bf803781b5183eaf509325678b48347"},
      {100000U, "25809b0819ad77a19d5644efa0989d7f37fa0b4b1e8e325d86c43853c83a75a4"},
  }};
  for (const Vector& vector : vectors) {
    const std::vector<std::byte> data = pattern(vector.length, 131U, 11U);
    ctx.check(ccsf::sha256(data).hex() == vector.hex,
              "one-shot sha256 mismatch at length " + std::to_string(vector.length));
    // Chunked streaming, including chunk boundaries that land on every residue
    // of the 64-byte block so that padding is exercised from inside the update.
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64},
                              std::size_t{65}, std::size_t{127}, std::size_t{4096}}) {
      ccsf::Sha256 hasher;
      std::size_t offset = 0;
      while (offset < data.size()) {
        const std::size_t take = std::min(chunk, data.size() - offset);
        hasher.update(ccsf::ByteSpan(data.data() + offset, take));
        offset += take;
      }
      ctx.check(hasher.finalize().hex() == vector.hex,
                "streamed sha256 mismatch at length " + std::to_string(vector.length) +
                    " with chunk " + std::to_string(chunk));
    }
  }
}

CCSF_CASE(sha256, hex_round_trip) {
  const ccsf::Digest digest = ccsf::sha256(std::string_view("cross-cluster"));
  const std::string hex = digest.hex();
  auto parsed = ccsf::Digest::from_hex(hex);
  ctx.check(parsed.has_value(), "digest hex did not parse");
  ctx.check(parsed.value() == digest, "digest hex round trip changed the digest");
  ctx.check(!ccsf::Digest::from_hex("zz").has_value(), "short/invalid hex must be rejected");
  ctx.check(!ccsf::Digest::from_hex(std::string(64, 'g')).has_value(),
            "non-hex characters must be rejected");
  ctx.check(ccsf::Digest{}.is_zero(), "default digest must be the zero digest");
}

CCSF_CASE(crc32c, known_vectors) {
  const std::string check = "123456789";
  const std::vector<std::byte> data = from_text(check);
  ctx.check(ccsf::crc32c(data) == 0xE3069283U, "CRC-32C check value is wrong");
  ctx.check(ccsf::crc32c(ccsf::ByteSpan{}) == 0U, "CRC-32C of empty input must be zero");
  // Extension must agree with a single pass over the concatenation.
  const std::vector<std::byte> left = from_text("1234");
  const std::vector<std::byte> right = from_text("56789");
  const std::uint32_t partial = ccsf::crc32c_extend(0U, left);
  ctx.check(ccsf::crc32c_extend(partial, right) == 0xE3069283U,
            "CRC-32C extension disagrees with the one-shot value");
}

CCSF_CASE(bytes, round_trip_and_bounds) {
  ccsf::ByteWriter writer;
  writer.u8(0xABU);
  writer.u16(0x1234U);
  writer.u32(0xDEADBEEFU);
  writer.u64(0x0123456789ABCDEFULL);
  writer.i64(-42);
  writer.boolean(true);
  writer.str("fabric");
  writer.blob(ccsf::ByteSpan(reinterpret_cast<const std::byte*>("xy"), 2));

  ccsf::ByteReader reader(writer.data());
  std::uint8_t u8 = 0;
  std::uint16_t u16 = 0;
  std::uint32_t u32 = 0;
  std::uint64_t u64 = 0;
  std::int64_t i64 = 0;
  bool flag = false;
  std::string text;
  std::vector<std::byte> blob;
  ctx.check(reader.u8(&u8) && u8 == 0xABU, "u8 round trip");
  ctx.check(reader.u16(&u16) && u16 == 0x1234U, "u16 big-endian round trip");
  ctx.check(reader.u32(&u32) && u32 == 0xDEADBEEFU, "u32 big-endian round trip");
  ctx.check(reader.u64(&u64) && u64 == 0x0123456789ABCDEFULL, "u64 big-endian round trip");
  ctx.check(reader.i64(&i64) && i64 == -42, "i64 round trip");
  ctx.check(reader.boolean(&flag) && flag, "boolean round trip");
  ctx.check(reader.str(64, &text) && text == "fabric", "string round trip");
  ctx.check(reader.blob(64, &blob) && blob.size() == 2U, "blob round trip");
  ctx.check(reader.require_end(), "reader must be at end");

  // Truncated reads must fail rather than read out of bounds.
  ccsf::ByteReader truncated(ccsf::ByteSpan(writer.data().data(), 3));
  std::uint32_t value = 0;
  ctx.check(truncated.u32(&value) == false, "truncated u32 must fail");
  ctx.check(!truncated.ok(), "reader must latch the failure");
  std::uint8_t ignored = 0;
  ctx.check(truncated.u8(&ignored) == false, "a failed reader stays failed");

  // Declared lengths beyond the bound are rejected before allocation.
  ccsf::ByteWriter hostile;
  hostile.u32(0xFFFFFFFFU);
  ccsf::ByteReader hostile_reader(hostile.data());
  std::vector<std::byte> big;
  ctx.check(hostile_reader.blob(1024U, &big) == false,
            "oversized declared blob length must be rejected before allocation");
  ctx.check(hostile_reader.error().code == ccsf::ErrorCode::RESOURCE_EXHAUSTED,
            "oversized blob must report RESOURCE_EXHAUSTED");

  // Trailing bytes are detected.
  ccsf::ByteReader trailing(writer.data());
  ctx.check(!trailing.require_end(), "trailing bytes must be detected");
}

CCSF_CASE(bytes, checked_arithmetic) {
  std::uint64_t out = 0;
  ctx.check(ccsf::checked_add(1, 2, &out) && out == 3U, "checked_add basic");
  ctx.check(!ccsf::checked_add(UINT64_MAX, 1, &out), "checked_add overflow");
  ctx.check(ccsf::checked_mul(3, 4, &out) && out == 12U, "checked_mul basic");
  ctx.check(!ccsf::checked_mul(UINT64_MAX, 2, &out), "checked_mul overflow");
}

CCSF_CASE(ids, parse_and_render) {
  const ccsf::StateId state = ccsf::StateId::from_value(1234U);
  ctx.check(state.str() == "1234", "state id rendering");
  auto parsed = ccsf::StateId::parse("1234");
  ctx.check(parsed.has_value() && parsed.value() == state, "state id parse round trip");
  ctx.check(!ccsf::StateId::parse("").has_value(), "empty identity text must be rejected");
  ctx.check(!ccsf::StateId::parse("12x").has_value(), "non numeric identity text must be rejected");
  ctx.check(!ccsf::StateId::parse("-1").has_value(), "negative identity text must be rejected");
  ctx.check(!ccsf::StateId{}.valid(), "a default identity is invalid");
  const ccsf::ReplicaId replica = ccsf::ReplicaId::from_value(1234U);
  ctx.check(replica.str() == state.str(),
            "distinct identity kinds may share a numeric value but never a type");
}

CCSF_CASE(taxonomy, render_validate_parse) {
  for (std::uint16_t raw = 0; raw <= 16U; ++raw) {
    const auto value = static_cast<ccsf::ReplicaLifecycle>(raw);
    if (raw <= 16U) {
      ctx.check(ccsf::is_valid(value), "every declared replica lifecycle must be valid");
      auto parsed = ccsf::parse_replica_lifecycle(ccsf::to_string(value));
      ctx.check(parsed.has_value() && parsed.value() == value,
                "replica lifecycle string round trip");
    }
  }
  ctx.check(!ccsf::is_valid(static_cast<ccsf::ReplicaLifecycle>(60000U)),
            "out of range enum values must be invalid");
  ctx.check(!ccsf::parse_state_class("NOT_A_CLASS").has_value(),
            "unknown enum text must be rejected");
  ctx.check(ccsf::parse_state_class("TENSOR_STATE").value() == ccsf::StateClass::TENSOR_STATE,
            "state class parse");
  for (std::uint16_t raw = 0; raw <= 10U; ++raw) {
    ctx.check(ccsf::is_valid(static_cast<ccsf::StateClass>(raw)), "declared state classes");
  }
}

CCSF_CASE(cost, unknown_never_becomes_zero) {
  const ccsf::CostValue unknown = ccsf::CostValue::unknown();
  const ccsf::CostValue priced = ccsf::CostValue::from_policy(180000);
  ctx.check(!unknown.known(), "unknown cost stays unknown");
  ctx.check(!ccsf::cost_add(unknown, priced).known(), "unknown absorbs in addition");
  ctx.check(!ccsf::cost_scale(unknown, 4, 1).known(), "unknown absorbs in scaling");
  ctx.check(!ccsf::cost_scale(priced, 0, 0).known(), "division by zero yields unknown");
  ctx.check(ccsf::cost_compare(unknown, priced) > 0, "unknown is worse than any known cost");
  ctx.check(ccsf::cost_compare(priced, unknown) < 0, "known cost beats unknown");
  ctx.check(ccsf::cost_compare(unknown, unknown) == 0, "two unknowns compare equal");
  const ccsf::CostValue summed = ccsf::cost_add(ccsf::CostValue::measured(100),
                                               ccsf::CostValue::estimated(200));
  ctx.check(summed.known() && summed.micro_units == 300, "known costs add exactly");
  ctx.check(summed.source == ccsf::CostSource::ESTIMATED,
            "combined provenance keeps the weaker evidence");
  const ccsf::CostValue overflow = ccsf::cost_scale(ccsf::CostValue::from_policy(INT64_MAX), 2, 1);
  ctx.check(!overflow.known(), "overflowing scaling yields unknown rather than a wrong number");
  ctx.check(ccsf::describe(unknown).find("UNKNOWN") == 0U, "unknown renders as UNKNOWN");
  const ccsf::ReuseValue unset;
  ctx.check(!unset.known(), "default reuse value is unknown");
  ctx.check(ccsf::ReuseValue::measured(14U, 500U).expected_value_milli() == 7000U,
            "expected reuse value is exact integer arithmetic");
}
