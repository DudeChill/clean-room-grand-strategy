// Core primitive tests: RNG, JSON, dates and enum names.
//
// Everything here is deterministic and self-contained: no clocks, no threads, no
// filesystem writes (parse_file is only exercised against a missing path).

#include <cstdint>

#include "test.h"

#include "core/json.h"
#include "core/log.h"
#include "core/rng.h"
#include "core/types.h"

using namespace hoi;

// ---------------------------------------------------------------------------
// RNG
// ---------------------------------------------------------------------------

HOI_TEST(rng_same_seed_identical_sequence) {
    Rng a(0x123456789ABCDEFull);
    Rng b(0x123456789ABCDEFull);
    Rng c(0x123456789ABCDF0ull);
    bool differs = false;
    for (int i = 0; i < 1000; ++i) {
        const uint64_t va = a.next_u64();
        const uint64_t vb = b.next_u64();
        const uint64_t vc = c.next_u64();
        CHECK_EQ(va, vb);
        if (va != vc) differs = true;
    }
    CHECK(differs);
}

HOI_TEST(rng_seed_with_matches_constructor) {
    Rng a(7);
    Rng b;
    b.seed_with(7);
    for (int i = 0; i < 16; ++i) CHECK_EQ(a.next_u64(), b.next_u64());
}

HOI_TEST(rng_state_round_trip) {
    Rng a(99);
    for (int i = 0; i < 5; ++i) (void)a.next_u64();
    uint64_t st[4];
    a.serialize_state(st);
    const uint64_t expected = a.next_u64();
    Rng b;
    b.deserialize_state(st);
    CHECK_EQ(b.next_u64(), expected);
}

HOI_TEST(rng_next_below_bounds) {
    Rng r(4242);
    for (uint32_t bound = 1; bound <= 1000; ++bound) {
        for (int k = 0; k < 50; ++k) {
            const uint32_t v = r.next_below(bound);
            CHECK(v < bound);
            if (bound == 1) CHECK_EQ(v, 0u);
        }
    }
}

HOI_TEST(rng_next_double_in_unit_interval) {
    Rng r(31337);
    for (int i = 0; i < 10000; ++i) {
        const double d = r.next_double();
        CHECK(d >= 0.0);
        CHECK(d <= 1.0);
    }
}

HOI_TEST(rng_range_and_chance_bounds) {
    Rng r(555);
    for (int i = 0; i < 1000; ++i) {
        const double v = r.range(-5.0, 5.0);
        CHECK(v >= -5.0);
        CHECK(v <= 5.0);
    }
    CHECK_EQ(r.range(3.0, 3.0), 3.0);
    CHECK(!r.chance(0.0));
    CHECK(r.chance(1.0));
    CHECK(!r.chance(-2.0));
    CHECK(r.chance(2.0));
}

HOI_TEST(rng_next_u32_is_inside_u64_draw) {
    Rng r(8);
    for (int i = 0; i < 100; ++i) {
        const uint64_t v = r.next_u64();
        Rng q(8);
        for (int j = 0; j < i; ++j) (void)q.next_u64();
        CHECK_EQ(q.next_u32(), static_cast<uint32_t>(v >> 32));
    }
}

HOI_TEST(splitmix64_known_sequence) {
    // Hand-computed reference from the splitmix64 definition (golden gamma
    // 0x9E3779B97F4A7C15, two xor-shift-multiply rounds, xor-shift 31).
    uint64_t state = 0;
    CHECK_EQ(splitmix64(state), 0xE220A8397B1DCDAFull);
    CHECK_EQ(splitmix64(state), 0x6E789E6AA1B965F4ull);
    CHECK_EQ(splitmix64(state), 0x06C45D188009454Full);
    CHECK_EQ(state, 3ull * 0x9E3779B97F4A7C15ull);
}

HOI_TEST(rng_stream_names_round_trip) {
    for (int i = 0; i < static_cast<int>(RngStream::Count); ++i) {
        bool ok = false;
        const RngStream s = rng_stream_from_name(rng_stream_name(static_cast<RngStream>(i)), &ok);
        CHECK(ok);
        CHECK_EQ(static_cast<int>(s), i);
    }
    bool ok = true;
    (void)rng_stream_from_name("nope", &ok);
    CHECK(!ok);
}

HOI_TEST(rngset_streams_are_independent) {
    RngSet a;
    RngSet b;
    a.seed(20240923);
    b.seed(20240923);

    // Heavy use of one stream must not move another stream's sequence.
    for (int i = 0; i < 1000; ++i) (void)a.get(RngStream::Combat).next_u64();

    for (int i = 0; i < 32; ++i) {
        CHECK_EQ(a.get(RngStream::Ai).next_u64(), b.get(RngStream::Ai).next_u64());
    }
    CHECK_EQ(a.get(RngStream::Map).next_u64(), b.get(RngStream::Map).next_u64());
    // The over-used stream itself has diverged from the untouched copy.
    CHECK(!(a.get(RngStream::Combat).next_u64() == b.get(RngStream::Combat).next_u64()));
}

HOI_TEST(rngset_state_hash_tracks_draws) {
    RngSet a;
    RngSet b;
    a.seed(1);
    b.seed(1);
    CHECK_EQ(a.master_seed(), 1ull);
    CHECK_EQ(a.state_hash(), b.state_hash());

    // No draw => hash stable across repeated calls.
    const uint64_t before = a.state_hash();
    CHECK_EQ(a.state_hash(), before);

    (void)a.get(RngStream::Events).next_u64();
    CHECK(a.state_hash() != before);
    CHECK(a.state_hash() != b.state_hash());
    // The untouched copy still matches a fresh set with the same seed.
    RngSet c;
    c.seed(1);
    CHECK_EQ(c.state_hash(), b.state_hash());
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

namespace {

const char* kNestedJson = R"({
  "name": "line\n\"q\"\\tab\t",
  "unicode": "\u00e9\u0041\ud83d\ude00",
  "neg": -12.5,
  "exp": 6.02e23,
  "zero": 0,
  "flag": true,
  "off": false,
  "nil": null,
  "empty_obj": {},
  "empty_arr": [],
  "arr": [1, 2, {"deep": [3, 4]}]
})";

}  // namespace

HOI_TEST(json_parse_nested_values) {
    std::string err;
    const Json j = Json::parse(kNestedJson, &err);
    CHECK(err.empty());
    CHECK(j.is_object());

    CHECK_EQ(j.at("name").as_string(), std::string("line\n\"q\"\\tab\t"));
    CHECK_EQ(j.at("unicode").as_string(), std::string("\xC3\xA9" "A" "\xF0\x9F\x98\x80"));
    CHECK_EQ(j.at("neg").as_double(), -12.5);
    CHECK_EQ(j.at("exp").as_double(), 6.02e23);
    CHECK_EQ(j.at("zero").as_int(), 0);
    CHECK_EQ(j.at("flag").as_bool(), true);
    CHECK_EQ(j.at("off").as_bool(), false);
    CHECK(j.at("nil").is_null());

    CHECK(j.at("empty_obj").is_object());
    CHECK_EQ(j.at("empty_obj").size(), 0u);
    CHECK(j.at("empty_arr").is_array());
    CHECK_EQ(j.at("empty_arr").size(), 0u);

    CHECK(j.at("arr").is_array());
    CHECK_EQ(j.at("arr").size(), 3u);
    CHECK_EQ(j.at("arr").at(0).as_int(), 1);
    CHECK_EQ(j.at("arr").at(2).at("deep").at(1).as_int(), 4);

    CHECK(j.has("name"));
    CHECK(!j.has("missing"));
    CHECK(j.at("missing").is_null());
    CHECK(j.at("arr").at(99).is_null());
    CHECK(j["arr"][1].as_double() == 2.0);
}

HOI_TEST(json_negative_and_exponent_forms) {
    const Json j = Json::parse("[-0.5, -3, 1e-3, 2E+2, 0.125]");
    CHECK(j.is_array());
    CHECK_EQ(j.size(), 5u);
    CHECK_EQ(j.at(0).as_double(), -0.5);
    CHECK_EQ(j.at(1).as_int(), -3);
    CHECK_EQ(j.at(2).as_double(), 1e-3);
    CHECK_EQ(j.at(3).as_double(), 200.0);
    CHECK_EQ(j.at(4).as_double(), 0.125);
}

HOI_TEST(json_dump_parse_dump_idempotent) {
    std::string err;
    const Json first = Json::parse(kNestedJson, &err);
    CHECK(err.empty());

    const std::string compact = first.dump();
    const std::string pretty = first.dump(2);

    const Json reparsed_compact = Json::parse(compact, &err);
    CHECK(err.empty());
    CHECK_EQ(reparsed_compact.dump(), compact);

    const Json reparsed_pretty = Json::parse(pretty, &err);
    CHECK(err.empty());
    CHECK_EQ(reparsed_pretty.dump(2), pretty);

    // Compact output has no pretty whitespace; pretty output is multi-line.
    CHECK(compact.find('\n') == std::string::npos);
    CHECK(pretty.find('\n') != std::string::npos);
    CHECK_EQ(Json::parse("{\"a\":1}").dump(), std::string("{\"a\":1}"));
    CHECK_EQ(Json::parse("{}").dump(), std::string("{}"));
    CHECK_EQ(Json::parse("[]").dump(), std::string("[]"));
}

HOI_TEST(json_control_characters_are_escaped) {
    Json s = Json(std::string("a\bb\fc\nd\re\tf"));
    CHECK_EQ(s.dump(), std::string("\"a\\bb\\fc\\nd\\re\\tf\""));
    Json ctrl = Json(std::string(1, static_cast<char>(0x01)));
    CHECK_EQ(ctrl.dump(), std::string("\"\\u0001\""));
    // Escaped control characters round-trip.
    std::string err;
    const Json back = Json::parse(ctrl.dump(), &err);
    CHECK(err.empty());
    CHECK_EQ(back.as_string(), std::string(1, static_cast<char>(0x01)));
}

HOI_TEST(json_integral_numbers_have_no_decimal_point) {
    Json root = Json::object();
    root.set("count", Json(static_cast<int64_t>(42)));
    root.set("frac", Json(0.5));
    CHECK_EQ(root.dump(), std::string("{\"count\":42,\"frac\":0.5}"));
}

HOI_TEST(json_mutation_and_accessors) {
    Json root = Json::object();
    root.set("a", Json(1));
    root.set("a", Json(2));
    CHECK_EQ(root.size(), 1u);
    CHECK_EQ(root.at("a").as_int(), 2);

    Json arr = Json::array();
    arr.push_back(Json("x"));
    arr.push_back(Json(false));
    CHECK_EQ(arr.size(), 2u);
    CHECK_EQ(arr.at(0).as_string(), "x");
    CHECK_EQ(arr.at(1).as_bool(), false);

    // Accessors fall back on the wrong type instead of asserting.
    CHECK_EQ(arr.at("a").as_int(7), 7);
    CHECK_EQ(root.at(0).as_string("d"), std::string("d"));
    CHECK_EQ(root.at("a").as_bool(true), true);
    CHECK_EQ(Json().size(), 0u);
}

HOI_TEST(json_parse_errors_report_location) {
    std::string err;
    Json::parse("{\n  \"a\": 1,\n  \"b\": }\n", &err);
    CHECK(!err.empty());
    CHECK(err.find("line 3") != std::string::npos);

    err.clear();
    Json::parse("", &err);
    CHECK(err.find("line 1") != std::string::npos);

    err.clear();
    Json::parse("[1, 2", &err);
    CHECK(err.find("line 1") != std::string::npos);
    CHECK(err.find("column") != std::string::npos);

    err.clear();
    Json::parse("{\"a\": 1} trailing", &err);
    CHECK(err.find("line 1") != std::string::npos);

    err.clear();
    Json::parse("\"bad \\q escape\"", &err);
    CHECK(err.find("line 1") != std::string::npos);
}

HOI_TEST(json_parse_file_distinguishes_io_errors) {
    Json out;
    std::string err;
    const bool ok = Json::parse_file("/nonexistent/hoi4clone/definitely-missing.json", &out, &err);
    CHECK(!ok);
    CHECK(err.find("cannot open file") != std::string::npos);
    CHECK(err.find("line") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

HOI_TEST(date_calendar_basics) {
    CHECK(!is_leap_year(1900));
    CHECK(is_leap_year(2000));
    CHECK(is_leap_year(2004));
    CHECK(is_leap_year(1904));
    CHECK(!is_leap_year(1901));
    CHECK(!is_leap_year(2100));

    CHECK_EQ(days_in_month(1900, 2), 28);
    CHECK_EQ(days_in_month(2000, 2), 29);
    CHECK_EQ(days_in_month(1936, 1), 31);
    CHECK_EQ(days_in_month(1936, 4), 30);
    CHECK_EQ(days_in_month(1936, 12), 31);

    CHECK_EQ(date_to_days(GameDate{1900, 1, 1, 0}), 0);
    CHECK_EQ(date_to_days(GameDate{1900, 1, 2, 0}), 1);
    CHECK_EQ(date_to_days(GameDate{1900, 3, 1, 0}), 59);
    CHECK_EQ(date_to_days(GameDate{1901, 1, 1, 0}), 365);
    // 1900 is not a leap year, so the first four years are all 365 days.
    CHECK_EQ(date_to_days(GameDate{1904, 1, 1, 0}), 4 * 365);
}

HOI_TEST(date_days_round_trip_1900_to_2000) {
    GameDate d{1900, 1, 1, 0};
    int64_t days = 0;
    // Walk every single day so leap years (including 1900 = not leap, 2000 = leap)
    // are covered by construction, not sampling.
    while (d.year <= 2000) {
        CHECK_EQ(date_to_days(d), days);
        const GameDate back = days_to_date(days);
        CHECK(back == d);
        ++days;
        GameDate next = days_to_date(days);
        d = next;
    }
    CHECK_EQ(days, date_to_days(GameDate{2000, 12, 31, 0}) + 1);
}

HOI_TEST(date_tick_round_trip) {
    const GameDate start{1936, 1, 1, 0};
    CHECK_EQ(date_to_tick(start, start), 0ull);
    for (Tick t = 0; t < 24ull * 900ull; t += 7) {
        const GameDate d = tick_to_date(t, start);
        CHECK_EQ(date_to_tick(d, start), t);
    }

    // A start date with a non-zero hour still inverts exactly.
    const GameDate odd_start{1936, 6, 15, 13};
    CHECK_EQ(tick_to_date(0, odd_start), odd_start);
    for (Tick t = 0; t < 5000ull; t += 13) {
        CHECK_EQ(date_to_tick(tick_to_date(t, odd_start), odd_start), t);
    }

    // Known values: 24 ticks advance one calendar day.
    CHECK(tick_to_date(24, start) == (GameDate{1936, 1, 2, 0}));
    CHECK(tick_to_date(1, start) == (GameDate{1936, 1, 1, 1}));
    CHECK_EQ(date_to_tick(GameDate{1936, 2, 1, 0}, start), 31ull * 24ull);
}

HOI_TEST(type_names_and_water) {
    CHECK_EQ(std::string(resource_name(Resource::Steel)), std::string("steel"));
    CHECK_EQ(std::string(equipment_category_name(EquipmentCategory::Armor)), std::string("armor"));
    CHECK_EQ(std::string(ideology_name(Ideology::Neutrality)), std::string("neutrality"));
    CHECK_EQ(std::string(terrain_name(Terrain::Mountain)), std::string("mountain"));

    CHECK(terrain_is_water(Terrain::Ocean));
    CHECK(terrain_is_water(Terrain::ShallowSea));
    CHECK(terrain_is_water(Terrain::DeepOcean));
    CHECK(terrain_is_water(Terrain::Lakes));
    CHECK(!terrain_is_water(Terrain::Plains));
    CHECK(!terrain_is_water(Terrain::Marsh));
}

// ---------------------------------------------------------------------------
// Logging (level filter only; no output is written from tests)
// ---------------------------------------------------------------------------

HOI_TEST(log_level_filter_round_trip) {
    const LogLevel saved = log_get_level();
    CHECK(saved == LogLevel::Info);
    log_set_level(LogLevel::Debug);
    CHECK(log_get_level() == LogLevel::Debug);
    log_set_level(LogLevel::Error);
    CHECK(log_get_level() == LogLevel::Error);
    log_set_level(saved);
}
