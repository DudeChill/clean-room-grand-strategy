// genmap: deterministic standalone world generator.
//
// Emits data/maps/world.json in the schema of ARCHITECTURE.md section 6. The tool
// has no dependency on the engine (no hoi headers) so that a map can always be
// regenerated even while the simulation is mid-refactor, and it never uses rand()
// or wall-clock time: the same --seed always produces a byte-identical file.
//
// Run:  genmap [--seed N] [--size N] [--out PATH]
//   --seed  world seed (default 12345)
//   --size  grid dimension, land target scales with size^2 (default 64)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------ hashing --

uint64_t sm64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Uniform [0,1) from a lattice coordinate and a channel salt.
double hash01(int x, int y, int channel, uint64_t seed) {
    uint64_t s = seed + 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(static_cast<uint32_t>(x)) +
                 0xBF58476D1CE4E5B9ull * static_cast<uint64_t>(static_cast<uint32_t>(y) + 7919u) +
                 0x94D049BB133111EBull * static_cast<uint64_t>(static_cast<uint32_t>(channel) * 131u + 1u);
    return static_cast<double>(sm64(s) >> 11) * (1.0 / 9007199254740992.0);
}

// Uniform integer in [0, bound).
uint32_t hash_below(int x, int y, int channel, uint64_t seed, uint32_t bound) {
    if (bound == 0) return 0;
    return static_cast<uint32_t>(hash01(x, y, channel, seed) * static_cast<double>(bound)) % bound;
}

double smoothstep01(double t) { return t * t * (3.0 - 2.0 * t); }

// Bilinear value noise on an integer lattice.
double value_noise(double x, double y, double freq, int channel, uint64_t seed) {
    const double fx = x * freq;
    const double fy = y * freq;
    const double x0d = std::floor(fx);
    const double y0d = std::floor(fy);
    const int x0 = static_cast<int>(x0d);
    const int y0 = static_cast<int>(y0d);
    const double tx = smoothstep01(fx - x0d);
    const double ty = smoothstep01(fy - y0d);
    const double a = hash01(x0, y0, channel, seed);
    const double b = hash01(x0 + 1, y0, channel, seed);
    const double c = hash01(x0, y0 + 1, channel, seed);
    const double d = hash01(x0 + 1, y0 + 1, channel, seed);
    const double top = a + (b - a) * tx;
    const double bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

double fbm(double x, double y, double base_freq, int octaves, int channel, uint64_t seed) {
    double value = 0.0;
    double amp = 1.0;
    double norm = 0.0;
    double freq = base_freq;
    for (int o = 0; o < octaves; ++o) {
        value += amp * value_noise(x, y, freq, channel * 17 + o, seed);
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return value / norm;
}

// ---------------------------------------------------------------- JSON out --

struct JsonBuf {
    std::string s;

    void put(const char* text) { s += text; }
    void num(long long v) { s += std::to_string(v); }
    void boolean(bool b) { s += b ? "true" : "false"; }
    void quoted(const std::string& text) {
        s += '"';
        for (char ch : text) {
            switch (ch) {
                case '"': s += "\\\""; break;
                case '\\': s += "\\\\"; break;
                case '\n': s += "\\n"; break;
                case '\t': s += "\\t"; break;
                case '\r': s += "\\r"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(ch));
                        s += esc;
                    } else {
                        s += ch;
                    }
            }
        }
        s += '"';
    }
};

// ------------------------------------------------------------------ naming --

const char* const kPre[] = {"Val", "Kel", "Dor", "Bran", "Mor",  "Sil", "Tar", "Ven",
                            "Hal", "Cor", "Mir", "Ash",  "Bel",  "Dur", "Fen", "Gar",
                            "Ith", "Jor", "Lyn", "Nar",  "Os",   "Pel", "Quen", "Rha",
                            "Sar", "Thal", "Ulm", "Vor", "Wes",  "Xan", "Yor", "Zel"};
const char* const kMid[] = {"a", "e", "i", "o", "u", "an", "er", "in", "or", "al", "yn", "eth"};
const char* const kSuf[] = {"ia",   "land", "mark",  "stead", "holm",  "gard",  "reach",
                            "fell", "vale", "moor",  "wick",  "ford",  "shire", "ridge",
                            "port", "grave"};

constexpr int kPreCount = static_cast<int>(sizeof(kPre) / sizeof(kPre[0]));
constexpr int kMidCount = static_cast<int>(sizeof(kMid) / sizeof(kMid[0]));
constexpr int kSufCount = static_cast<int>(sizeof(kSuf) / sizeof(kSuf[0]));

std::string make_name(int x, int y, int channel, uint64_t seed, int parts) {
    std::string out = kPre[hash_below(x, y, channel + 1, seed, kPreCount)];
    if (parts >= 3) out += kMid[hash_below(x, y, channel + 2, seed, kMidCount)];
    out += kSuf[hash_below(x, y, channel + 3, seed, kSufCount)];
    return out;
}

// Terrain enum order mirrors hoi::Terrain (Plains..Jungle are used here).
enum TerrainId {
    T_PLAINS = 0,
    T_FOREST,
    T_HILLS,
    T_MOUNTAIN,
    T_URBAN,
    T_MARSH,
    T_DESERT,
    T_JUNGLE,
    T_COUNT
};

const char* terrain_key(int t) {
    switch (t) {
        case T_PLAINS: return "plains";
        case T_FOREST: return "forest";
        case T_HILLS: return "hills";
        case T_MOUNTAIN: return "mountain";
        case T_URBAN: return "urban";
        case T_MARSH: return "marsh";
        case T_DESERT: return "desert";
        case T_JUNGLE: return "jungle";
        default: return "plains";
    }
}

const int kDx4[4] = {1, -1, 0, 0};
const int kDy4[4] = {0, 0, 1, -1};
// The four true diagonal steps; both directions are walked and the gate below is
// computed from the canonical corner of the pair, so the result is symmetric.
const int kDx8[4] = {1, 1, -1, -1};
const int kDy8[4] = {1, -1, 1, -1};

struct Args {
    uint64_t seed = 12345;
    int size = 64;
    std::string out = "data/maps/world.json";
};

bool parse_args(int argc, char** argv, Args* args) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--seed") == 0 && i + 1 < argc) {
            args->seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--size") == 0 && i + 1 < argc) {
            args->size = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--out") == 0 && i + 1 < argc) {
            args->out = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return false;
        }
    }
    if (args->size < 16 || args->size > 256) {
        std::fprintf(stderr, "--size must be in [16, 256]\n");
        return false;
    }
    return true;
}

struct Gen {
    Args args;
    int W = 0;
    int H = 0;

    std::vector<uint8_t> land;      // 1 = land cell
    std::vector<uint8_t> sea_prov;  // 1 = shallow sea cell that becomes a province
    std::vector<int> terrain;       // per cell, -1 for water
    std::vector<int> province_of;   // per cell, -1 when the cell has no province
    std::vector<int> province_cell;  // per province: the cell it was numbered from
    std::vector<int> state_of;      // per cell, -1 when unassigned
    std::vector<int> region_of_state;

    std::vector<std::vector<int>> province_adj;   // per province: cell indices of grid neighbours
    std::vector<std::vector<int>> state_cells;
    std::vector<std::vector<int>> region_states;
    std::vector<std::vector<int>> sea_region_provinces;

    std::vector<int> province_pop;
    std::vector<int> province_vp;
    std::vector<int> province_infra;
    std::vector<uint8_t> province_hub;
    std::vector<int> province_rail;
    std::vector<int> province_res[6];  // per resource index, amount per province
    std::vector<int> province_state;   // typed state index per province, -1 for sea
    std::vector<int> province_region;

    int idx(int x, int y) const { return y * W + x; }
    bool in_grid(int x, int y) const { return x >= 0 && y >= 0 && x < W && y < H; }
};

// Resource order matches hoi::Resource: oil, steel, aluminium, rubber, chromium,
// tungsten.
enum { R_OIL = 0, R_STEEL, R_ALUMINIUM, R_RUBBER, R_CHROMIUM, R_TUNGSTEN, R_COUNT };
const char* resource_key(int r) {
    switch (r) {
        case R_OIL: return "oil";
        case R_STEEL: return "steel";
        case R_ALUMINIUM: return "aluminium";
        case R_RUBBER: return "rubber";
        case R_CHROMIUM: return "chromium";
        case R_TUNGSTEN: return "tungsten";
        default: return "steel";
    }
}

// ------------------------------------------------------------ land generation --

void generate_land(Gen& g) {
    const uint64_t seed = g.args.seed;
    const int N = g.W * g.H;
    const double nx_scale = 2.0 / static_cast<double>(g.W - 1);
    const double ny_scale = 2.0 / static_cast<double>(g.H - 1);
    const double land_freq = 2.5 * static_cast<double>(g.W) / 64.0;

    std::vector<double> field(static_cast<size_t>(N));
    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const double nx = static_cast<double>(x) * nx_scale - 1.0;
            const double ny = static_cast<double>(y) * ny_scale - 1.0;
            // Continent field: low-frequency fbm gives 2-4 blobs; the edge falloff
            // keeps a deep-ocean margin along every border.
            double v = fbm(static_cast<double>(x), static_cast<double>(y), land_freq, 4, 1, seed);
            const double edge = std::max(std::fabs(nx), std::fabs(ny));
            double falloff = 1.0;
            if (edge > 0.62) {
                const double t = (edge - 0.62) / 0.38;
                falloff = 1.0 - 1.05 * smoothstep01(t > 1.0 ? 1.0 : t);
            }
            field[static_cast<size_t>(g.idx(x, y))] = v * (falloff > 0.0 ? falloff : 0.0);
        }
    }

    // Land is exactly the highest `target` field values. A pure quantile keeps the
    // province scale stable no matter how the noise shifts with the seed.
    const long long target =
        static_cast<long long>(std::llround(0.445 * static_cast<double>(N)));
    auto quantile_mask = [&](const std::vector<double>& values) {
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const size_t pos = static_cast<size_t>(N) - static_cast<size_t>(target > 0 ? target : 1);
        const double threshold = sorted[pos < sorted.size() ? pos : 0];
        std::vector<uint8_t> mask(static_cast<size_t>(N), 0);
        for (int i = 0; i < N; ++i) {
            mask[static_cast<size_t>(i)] = values[static_cast<size_t>(i)] >= threshold ? 1 : 0;
        }
        return mask;
    };

    g.land = quantile_mask(field);

    // Two smoothing iterations compact the coastline without changing the land
    // budget: each iteration blends the continuous field with the fractional land
    // share of the 3x3 neighbourhood, then re-thresholds at the same quantile. Raw
    // noise gives a ragged coast and would emit far more shallow-sea provinces
    // than the target scale allows.
    for (int pass = 0; pass < 1; ++pass) {
        std::vector<double> smoothed(static_cast<size_t>(N));
        for (int y = 0; y < g.H; ++y) {
            for (int x = 0; x < g.W; ++x) {
                int land_count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nxx = x + dx;
                        const int nyy = y + dy;
                        if (!g.in_grid(nxx, nyy)) continue;
                        land_count += g.land[static_cast<size_t>(g.idx(nxx, nyy))] ? 1 : 0;
                    }
                }
                const size_t i = static_cast<size_t>(g.idx(x, y));
                const double share = static_cast<double>(land_count) / 9.0;
                smoothed[i] = 0.60 * field[i] + 0.40 * share;
            }
        }
        g.land = quantile_mask(smoothed);
    }

    // Prune specks, then keep at most four major landmasses.
    auto components = [&](void) {
        std::vector<int> comp(static_cast<size_t>(N), -1);
        std::vector<std::vector<int>> groups;
        for (int y = 0; y < g.H; ++y) {
            for (int x = 0; x < g.W; ++x) {
                const int start = g.idx(x, y);
                if (!g.land[static_cast<size_t>(start)] ||
                    comp[static_cast<size_t>(start)] >= 0) {
                    continue;
                }
                const int id = static_cast<int>(groups.size());
                groups.push_back({});
                std::vector<int> stack{start};
                comp[static_cast<size_t>(start)] = id;
                while (!stack.empty()) {
                    const int cur = stack.back();
                    stack.pop_back();
                    groups[static_cast<size_t>(id)].push_back(cur);
                    const int cx = cur % g.W;
                    const int cy = cur / g.W;
                    for (int d = 0; d < 4; ++d) {
                        const int nxx = cx + kDx4[d];
                        const int nyy = cy + kDy4[d];
                        if (!g.in_grid(nxx, nyy)) continue;
                        const int ni = g.idx(nxx, nyy);
                        if (!g.land[static_cast<size_t>(ni)] ||
                            comp[static_cast<size_t>(ni)] >= 0) {
                            continue;
                        }
                        comp[static_cast<size_t>(ni)] = id;
                        stack.push_back(ni);
                    }
                }
            }
        }
        return groups;
    };

    {
        std::vector<std::vector<int>> groups = components();
        const size_t min_size = static_cast<size_t>(std::max(8, N / 400));
        for (const auto& cells : groups) {
            if (cells.size() < min_size) {
                for (int c : cells) g.land[static_cast<size_t>(c)] = 0;
            }
        }
    }
    {
        std::vector<std::vector<int>> groups = components();
        std::vector<int> order(groups.size());
        for (size_t i = 0; i < groups.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (groups[static_cast<size_t>(a)].size() != groups[static_cast<size_t>(b)].size()) {
                return groups[static_cast<size_t>(a)].size() > groups[static_cast<size_t>(b)].size();
            }
            return a < b;
        });
        const size_t major = static_cast<size_t>(std::max(8, N / 64));
        int kept = 0;
        for (int id : order) {
            const auto& cells = groups[static_cast<size_t>(id)];
            if (cells.size() >= major) {
                if (kept >= 4) {
                    for (int c : cells) g.land[static_cast<size_t>(c)] = 0;
                }
                ++kept;
            }
        }
    }
}

void classify_cells(Gen& g) {
    const uint64_t seed = g.args.seed;
    const int N = g.W * g.H;
    g.sea_prov.assign(static_cast<size_t>(N), 0);
    g.terrain.assign(static_cast<size_t>(N), -1);
    g.province_of.assign(static_cast<size_t>(N), -1);

    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            if (g.land[static_cast<size_t>(i)]) continue;
            bool touches_land = false;
            for (int d = 0; d < 4 && !touches_land; ++d) {
                const int nxx = x + kDx4[d];
                const int nyy = y + kDy4[d];
                if (g.in_grid(nxx, nyy) && g.land[static_cast<size_t>(g.idx(nxx, nyy))]) {
                    touches_land = true;
                }
            }
            if (touches_land) g.sea_prov[static_cast<size_t>(i)] = 1;
        }
    }

    const double terrain_freq = 5.0 * static_cast<double>(g.W) / 64.0;
    const double ridge_freq = 8.0 * static_cast<double>(g.W) / 64.0;
    const double ny_scale = 2.0 / static_cast<double>(g.H - 1);

    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            if (!g.land[static_cast<size_t>(i)]) continue;
            const double lat = std::fabs(static_cast<double>(y) * ny_scale - 1.0);
            const double n = fbm(static_cast<double>(x), static_cast<double>(y), terrain_freq, 3, 2, seed);
            const double raw = value_noise(static_cast<double>(x), static_cast<double>(y), ridge_freq, 3, seed);
            const double ridge = 1.0 - std::fabs(2.0 * raw - 1.0);
            bool coastal = false;
            for (int d = 0; d < 4; ++d) {
                const int nxx = x + kDx4[d];
                const int nyy = y + kDy4[d];
                if (!g.in_grid(nxx, nyy)) continue;
                if (g.sea_prov[static_cast<size_t>(g.idx(nxx, nyy))]) coastal = true;
            }

            int t;
            if (ridge > 0.84) {
                t = T_MOUNTAIN;
            } else if (lat < 0.18 && n > 0.32) {
                t = T_JUNGLE;
            } else if (lat >= 0.12 && lat < 0.34 && n < 0.46) {
                t = T_DESERT;
            } else if (coastal && n < 0.20) {
                t = T_MARSH;
            } else if (n > 0.72) {
                t = T_FOREST;
            } else if (n > 0.55) {
                t = T_HILLS;
            } else {
                t = T_PLAINS;
            }
            g.terrain[static_cast<size_t>(i)] = t;
        }
    }
}

void number_provinces(Gen& g) {
    int next = 0;
    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            if (g.land[static_cast<size_t>(i)] || g.sea_prov[static_cast<size_t>(i)]) {
                g.province_of[static_cast<size_t>(i)] = next++;
                g.province_cell.push_back(i);
            }
        }
    }
    g.province_adj.resize(static_cast<size_t>(next));
    g.province_pop.assign(static_cast<size_t>(next), 0);
    g.province_vp.assign(static_cast<size_t>(next), 0);
    g.province_infra.assign(static_cast<size_t>(next), 0);
    g.province_hub.assign(static_cast<size_t>(next), 0);
    g.province_rail.assign(static_cast<size_t>(next), 0);
    for (int r = 0; r < R_COUNT; ++r) g.province_res[r].assign(static_cast<size_t>(next), 0);
    g.province_state.assign(static_cast<size_t>(next), -1);
    g.province_region.assign(static_cast<size_t>(next), -1);
}

void build_adjacency(Gen& g) {
    const uint64_t seed = g.args.seed;
    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            const int pi = g.province_of[static_cast<size_t>(i)];
            if (pi < 0) continue;
            for (int d = 0; d < 4; ++d) {
                const int nxx = x + kDx4[d];
                const int nyy = y + kDy4[d];
                if (!g.in_grid(nxx, nyy)) continue;
                const int ni = g.idx(nxx, nyy);
                const int pj = g.province_of[static_cast<size_t>(ni)];
                if (pj < 0) continue;
                g.province_adj[static_cast<size_t>(pi)].push_back(pj);
            }
            // A deterministic quarter of the diagonals links as well, which keeps
            // coastlines and fronts from being strictly orthogonal. The gate is
            // keyed on the canonical (min x, min y) corner of the pair and on the
            // absolute shape of the step, so both endpoints decide identically and
            // the emitted graph stays symmetric.
            for (int d = 0; d < 4; ++d) {
                const int nxx = x + kDx8[d];
                const int nyy = y + kDy8[d];
                if (!g.in_grid(nxx, nyy)) continue;
                const int ni = g.idx(nxx, nyy);
                if (!g.land[static_cast<size_t>(i)] || !g.land[static_cast<size_t>(ni)]) continue;
                const int shape = 2 * std::abs(nxx - x) + std::abs(nyy - y);
                const int canon_x = std::min(x, nxx);
                const int canon_y = std::min(y, nyy);
                if (hash_below(canon_x, canon_y, 7 + shape, seed, 4) != 0) continue;
                const int pj = g.province_of[static_cast<size_t>(ni)];
                if (pj < 0 || pj == pi) continue;
                g.province_adj[static_cast<size_t>(pi)].push_back(pj);
            }
        }
    }
    for (auto& list : g.province_adj) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
}

void group_states(Gen& g) {
    const uint64_t seed = g.args.seed;
    const int N = g.W * g.H;
    g.state_of.assign(static_cast<size_t>(N), -1);

    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int start = g.idx(x, y);
            if (!g.land[static_cast<size_t>(start)] || g.state_of[static_cast<size_t>(start)] >= 0) {
                continue;
            }
            const int target = 3 + static_cast<int>(hash_below(x, y, 11, seed, 6));  // 3..8
            const int id = static_cast<int>(g.state_cells.size());
            g.state_cells.push_back({});
            // A cell is added to `state_cells` at the moment it is claimed, so the
            // list length is always the true state size and the cap is exact.
            std::vector<int> frontier{start};
            g.state_of[static_cast<size_t>(start)] = id;
            g.state_cells[static_cast<size_t>(id)].push_back(start);
            for (size_t head = 0; head < frontier.size(); ++head) {
                if (static_cast<int>(g.state_cells[static_cast<size_t>(id)].size()) >= target) break;
                const int cur = frontier[head];
                const int cx = cur % g.W;
                const int cy = cur / g.W;
                for (int d = 0; d < 4; ++d) {
                    const int nxx = cx + kDx4[d];
                    const int nyy = cy + kDy4[d];
                    if (!g.in_grid(nxx, nyy)) continue;
                    const int ni = g.idx(nxx, nyy);
                    if (!g.land[static_cast<size_t>(ni)] || g.state_of[static_cast<size_t>(ni)] >= 0) {
                        continue;
                    }
                    g.state_of[static_cast<size_t>(ni)] = id;
                    g.state_cells[static_cast<size_t>(id)].push_back(ni);
                    frontier.push_back(ni);
                    if (static_cast<int>(g.state_cells[static_cast<size_t>(id)].size()) >= target) {
                        break;
                    }
                }
            }
        }
    }

    // Any state that ended smaller than three provinces is repaired so that every
    // state stays inside 3..8 provinces: first merge into an adjacent state with
    // room, and when no neighbour has room, move boundary provinces across from
    // the smallest adjacent state that can spare them.
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t id = 0; id < g.state_cells.size(); ++id) {
            const size_t size = g.state_cells[id].size();
            if (size == 0 || size >= 3) continue;

            std::vector<int> adjacent;
            for (int cell : g.state_cells[id]) {
                const int cx = cell % g.W;
                const int cy = cell / g.W;
                for (int d = 0; d < 4; ++d) {
                    const int nxx = cx + kDx4[d];
                    const int nyy = cy + kDy4[d];
                    if (!g.in_grid(nxx, nyy)) continue;
                    const int other = g.state_of[static_cast<size_t>(g.idx(nxx, nyy))];
                    if (other < 0 || other == static_cast<int>(id)) continue;
                    if (std::find(adjacent.begin(), adjacent.end(), other) == adjacent.end()) {
                        adjacent.push_back(other);
                    }
                }
            }

            int best = -1;
            for (int other : adjacent) {
                if (g.state_cells[static_cast<size_t>(other)].size() + size > 8) continue;
                if (best < 0 || g.state_cells[static_cast<size_t>(other)].size() <
                                    g.state_cells[static_cast<size_t>(best)].size()) {
                    best = other;
                }
            }
            if (best >= 0) {
                for (int cell : g.state_cells[id]) {
                    g.state_of[static_cast<size_t>(cell)] = best;
                    g.state_cells[static_cast<size_t>(best)].push_back(cell);
                }
                g.state_cells[id].clear();
                merged = true;
                continue;
            }

            const int need = 3 - static_cast<int>(size);
            int donor = -1;
            for (int other : adjacent) {
                if (static_cast<int>(g.state_cells[static_cast<size_t>(other)].size()) >=
                    3 + need) {
                    if (donor < 0 ||
                        g.state_cells[static_cast<size_t>(other)].size() <
                            g.state_cells[static_cast<size_t>(donor)].size()) {
                        donor = other;
                    }
                }
            }
            if (donor < 0) continue;
            // Grow the taken set outwards from the donor's boundary so a small
            // pocket can always be brought up to three provinces.
            std::vector<int> taken;
            while (static_cast<int>(taken.size()) < need) {
                bool progress = false;
                for (int cell : g.state_cells[static_cast<size_t>(donor)]) {
                    if (std::find(taken.begin(), taken.end(), cell) != taken.end()) continue;
                    const int cx = cell % g.W;
                    const int cy = cell / g.W;
                    bool touches = false;
                    for (int d = 0; d < 4 && !touches; ++d) {
                        const int nxx = cx + kDx4[d];
                        const int nyy = cy + kDy4[d];
                        if (!g.in_grid(nxx, nyy)) continue;
                        const int neighbour = g.state_of[static_cast<size_t>(g.idx(nxx, nyy))];
                        if (neighbour == static_cast<int>(id) ||
                            std::find(taken.begin(), taken.end(), g.idx(nxx, nyy)) !=
                                taken.end()) {
                            touches = true;
                        }
                    }
                    if (!touches) continue;
                    taken.push_back(cell);
                    progress = true;
                    if (static_cast<int>(taken.size()) >= need) break;
                }
                if (!progress) break;
            }
            if (static_cast<int>(taken.size()) < need) continue;
            for (int cell : taken) {
                g.state_of[static_cast<size_t>(cell)] = static_cast<int>(id);
                g.state_cells[id].push_back(cell);
            }
            std::vector<int>& donor_cells = g.state_cells[static_cast<size_t>(donor)];
            donor_cells.erase(std::remove_if(donor_cells.begin(), donor_cells.end(),
                                             [&](int cell) {
                                                 return std::find(taken.begin(), taken.end(),
                                                                  cell) != taken.end();
                                             }),
                              donor_cells.end());
            merged = true;
        }
        if (merged) {
            // Compact empty state slots so ids stay dense.
            std::vector<std::vector<int>> compact;
            std::vector<int> remap(g.state_cells.size(), -1);
            for (size_t id = 0; id < g.state_cells.size(); ++id) {
                if (g.state_cells[id].empty()) continue;
                remap[id] = static_cast<int>(compact.size());
                compact.push_back(std::move(g.state_cells[id]));
            }
            g.state_cells = std::move(compact);
            for (int i = 0; i < N; ++i) {
                const int s = g.state_of[static_cast<size_t>(i)];
                if (s >= 0) g.state_of[static_cast<size_t>(i)] = remap[static_cast<size_t>(s)];
            }
            break;  // restart with dense ids
        }
    }
}

void group_regions(Gen& g) {
    const size_t state_count = g.state_cells.size();
    g.region_of_state.assign(state_count, -1);
    g.region_states.clear();
    if (state_count == 0) return;

    // State adjacency comes from land cells whose four-neighbours cross a state
    // border, which is exactly the set of shared province edges.
    std::vector<std::vector<int>> neighbours(state_count);
    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            const int a = g.state_of[static_cast<size_t>(i)];
            if (a < 0) continue;
            for (int d = 0; d < 4; ++d) {
                const int nxx = x + kDx4[d];
                const int nyy = y + kDy4[d];
                if (!g.in_grid(nxx, nyy)) continue;
                const int b = g.state_of[static_cast<size_t>(g.idx(nxx, nyy))];
                if (b < 0 || b == a) continue;
                neighbours[static_cast<size_t>(a)].push_back(b);
            }
        }
    }
    for (auto& list : neighbours) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    const int target_regions =
        static_cast<int>(std::min<size_t>(40, std::max<size_t>(1, state_count / 3)));
    int regions_left = target_regions;
    size_t remaining = state_count;
    for (size_t start = 0; start < state_count; ++start) {
        if (g.region_of_state[start] >= 0) continue;
        const int id = static_cast<int>(g.region_states.size());
        g.region_states.push_back({});
        const size_t want = (remaining + static_cast<size_t>(regions_left) - 1) /
                            static_cast<size_t>(regions_left);
        std::vector<int> frontier{static_cast<int>(start)};
        g.region_of_state[start] = id;
        g.region_states[static_cast<size_t>(id)].push_back(static_cast<int>(start));
        size_t head = 0;
        while (head < frontier.size() &&
               g.region_states[static_cast<size_t>(id)].size() < want) {
            const int cur = frontier[head++];
            for (int nb : neighbours[static_cast<size_t>(cur)]) {
                if (g.region_of_state[static_cast<size_t>(nb)] >= 0) continue;
                g.region_of_state[static_cast<size_t>(nb)] = id;
                g.region_states[static_cast<size_t>(id)].push_back(nb);
                frontier.push_back(nb);
                if (g.region_states[static_cast<size_t>(id)].size() >= want) break;
            }
        }
        remaining -= g.region_states[static_cast<size_t>(id)].size();
        --regions_left;
        if (regions_left <= 0) {
            for (size_t s = 0; s < state_count; ++s) {
                if (g.region_of_state[s] >= 0) continue;
                g.region_of_state[s] = id;
                g.region_states[static_cast<size_t>(id)].push_back(static_cast<int>(s));
            }
            break;
        }
    }
}

// Sea regions are latitude bands: coastal water is too fragmented for a
// connectivity walk to give a stable count, and weather (the only consumer)
// already works from a latitude proxy.
void group_sea_regions(Gen& g, int* out_count) {
    const int bands = std::max(4, g.H / 8);  // 8 bands at the default size
    g.sea_region_provinces.assign(static_cast<size_t>(bands), {});
    const int land_regions = static_cast<int>(g.region_states.size());
    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            if (!g.sea_prov[static_cast<size_t>(i)]) continue;
            const int p = g.province_of[static_cast<size_t>(i)];
            if (p < 0) continue;
            int band = y * bands / g.H;
            if (band >= bands) band = bands - 1;
            g.province_region[static_cast<size_t>(p)] = land_regions + band;
            g.sea_region_provinces[static_cast<size_t>(band)].push_back(p);
        }
    }
    *out_count = bands;
}

void province_attributes(Gen& g) {
    const uint64_t seed = g.args.seed;
    const double infra_freq = 6.0 * static_cast<double>(g.W) / 64.0;
    static const int kPopBase[T_COUNT] = {300, 190, 150, 55, 900, 80, 60, 90};  // thousands

    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            const int p = g.province_of[static_cast<size_t>(i)];
            if (p < 0) continue;
            if (!g.land[static_cast<size_t>(i)]) continue;
            const int t = g.terrain[static_cast<size_t>(i)];
            const int s = g.state_of[static_cast<size_t>(i)];
            g.province_state[static_cast<size_t>(p)] = s;
            if (s >= 0) {
                g.province_region[static_cast<size_t>(p)] = g.region_of_state[static_cast<size_t>(s)];
            }

            const double pop_noise = fbm(static_cast<double>(x), static_cast<double>(y),
                                         infra_freq, 2, 5, seed);
            const double scale = 0.55 + 0.95 * pop_noise;
            int pop = static_cast<int>(static_cast<double>(kPopBase[t]) * scale);
            if (pop < 10) pop = 10;
            g.province_pop[static_cast<size_t>(p)] = pop * 1000;

            const double infra_noise = fbm(static_cast<double>(x), static_cast<double>(y),
                                           infra_freq * 0.5, 2, 6, seed);
            int infra = 1 + static_cast<int>(infra_noise * 6.0);
            if (infra < 1) infra = 1;
            if (infra > 6) infra = 6;
            g.province_infra[static_cast<size_t>(p)] = infra;

            const int amount = 1 + static_cast<int>(hash_below(x, y, 21, seed, 6));
            switch (t) {
                case T_HILLS:
                    g.province_res[R_STEEL][static_cast<size_t>(p)] = amount;
                    g.province_res[R_ALUMINIUM][static_cast<size_t>(p)] = amount + 1;
                    break;
                case T_MOUNTAIN:
                    g.province_res[R_CHROMIUM][static_cast<size_t>(p)] = amount;
                    g.province_res[R_TUNGSTEN][static_cast<size_t>(p)] = amount;
                    if (amount >= 4) g.province_res[R_STEEL][static_cast<size_t>(p)] = amount - 2;
                    break;
                case T_DESERT:
                    g.province_res[R_OIL][static_cast<size_t>(p)] = amount + 1;
                    break;
                case T_MARSH:
                    g.province_res[R_OIL][static_cast<size_t>(p)] = amount;
                    break;
                case T_JUNGLE:
                    g.province_res[R_RUBBER][static_cast<size_t>(p)] = amount + 1;
                    break;
                default:
                    break;
            }
        }
    }

    // One supply hub and a cluster of victory points per state. The hub is the
    // most populous province of the state; ties break on the lower province index.
    for (size_t id = 0; id < g.state_cells.size(); ++id) {
        int hub_p = -1;
        int hub_pop = -1;
        int vp_left = static_cast<int>(g.state_cells[id].size()) >= 6 ? 2 : 1;
        std::vector<int> by_pop;
        for (int cell : g.state_cells[id]) {
            const int p = g.province_of[static_cast<size_t>(cell)];
            if (p < 0) continue;
            by_pop.push_back(p);
            const int pop = g.province_pop[static_cast<size_t>(p)];
            if (pop > hub_pop) {
                hub_pop = pop;
                hub_p = p;
            }
        }
        std::sort(by_pop.begin(), by_pop.end(),
                  [&](int a, int b) { return g.province_pop[static_cast<size_t>(a)] > g.province_pop[static_cast<size_t>(b)]; });
        for (int i = 0; i < vp_left && i < static_cast<int>(by_pop.size()); ++i) {
            const int p = by_pop[static_cast<size_t>(i)];
            const int amount =
                5 + static_cast<int>(hash_below(p, static_cast<int>(id), 31, seed, 21));
            g.province_vp[static_cast<size_t>(p)] = amount;
        }
        if (hub_p >= 0) {
            g.province_hub[static_cast<size_t>(hub_p)] = 1;
            g.province_rail[static_cast<size_t>(hub_p)] =
                3 + static_cast<int>(hash_below(hub_p, 1, 41, seed, 3));
            // Urban terrain marks the industrial heartland.
            for (int cell : g.state_cells[id]) {
                if (g.province_of[static_cast<size_t>(cell)] == hub_p) {
                    g.terrain[static_cast<size_t>(cell)] = T_URBAN;
                }
            }
        }
    }
    // Railways follow population along the coast and in the interior.
    for (int y = 0; y < g.H; ++y) {
        for (int x = 0; x < g.W; ++x) {
            const int i = g.idx(x, y);
            const int p = g.province_of[static_cast<size_t>(i)];
            if (p < 0 || !g.land[static_cast<size_t>(i)]) continue;
            if (g.province_rail[static_cast<size_t>(p)] > 0) continue;
            if (g.province_pop[static_cast<size_t>(p)] > 200000) {
                g.province_rail[static_cast<size_t>(p)] =
                    1 + static_cast<int>(hash_below(x, y, 43, seed, 3));
            }
        }
    }
}

// ------------------------------------------------------------------ writing --

std::string province_key(int index) { return "p" + std::to_string(index + 1); }

std::string write_map(const Gen& g) {
    JsonBuf out;
    const char* nl = "\n";
    out.put("{"); out.put(nl);
    out.put("  \"name\": "); out.quoted("world"); out.put(","); out.put(nl);

    const int land_regions = static_cast<int>(g.region_states.size());
    const int sea_regions = static_cast<int>(g.sea_region_provinces.size());

    out.put("  \"regions\": ["); out.put(nl);
    for (int r = 0; r < land_regions + sea_regions; ++r) {
        out.put("    {\"key\": "); out.quoted("r" + std::to_string(r + 1));
        out.put(", \"name\": ");
        const bool is_sea = r >= land_regions;
        if (is_sea) {
            out.quoted("Sea Reach " + std::to_string(r - land_regions + 1));
        } else {
            out.quoted(make_name(r, 3, 51, g.args.seed, 2));
        }
        out.put(", \"is_sea\": "); out.boolean(is_sea);
        out.put("}");
        if (r + 1 < land_regions + sea_regions) out.put(",");
        out.put(nl);
    }
    out.put("  ],"); out.put(nl);

    out.put("  \"states\": ["); out.put(nl);
    for (size_t s = 0; s < g.state_cells.size(); ++s) {
        long long total_pop = 0;
        for (int cell : g.state_cells[s]) {
            const int p = g.province_of[static_cast<size_t>(cell)];
            if (p >= 0) total_pop += g.province_pop[static_cast<size_t>(p)];
        }
        int slots = 2 + static_cast<int>(total_pop / 220000);
        if (slots > 10) slots = 10;
        const int region = g.region_of_state[s] >= 0 ? g.region_of_state[s] : 0;
        out.put("    {\"key\": "); out.quoted("s" + std::to_string(s + 1));
        out.put(", \"name\": ");
        out.quoted(make_name(static_cast<int>(s), 7, 61, g.args.seed, 3));
        out.put(", \"region\": "); out.quoted("r" + std::to_string(region + 1));
        out.put(", \"building_slots\": "); out.num(slots);
        out.put("}");
        if (s + 1 < g.state_cells.size()) out.put(",");
        out.put(nl);
    }
    out.put("  ],"); out.put(nl);

    out.put("  \"provinces\": ["); out.put(nl);
    const size_t province_count = g.province_adj.size();
    for (size_t p = 0; p < province_count; ++p) {
        const bool is_sea = g.province_state[p] < 0;
        const int cell = g.province_cell[p];
        const int x = cell % g.W;
        const int y = cell / g.W;
        out.put("    {\"key\": "); out.quoted(province_key(static_cast<int>(p)));
        out.put(", \"name\": ");
        out.quoted(is_sea ? std::string("Sea Zone " + std::to_string(p + 1))
                          : make_name(x, y, 71, g.args.seed, 3));
        if (is_sea) {
            out.put(", \"state\": null");
        } else {
            out.put(", \"state\": ");
            out.quoted("s" + std::to_string(g.province_state[p] + 1));
        }
        out.put(", \"region\": ");
        out.quoted("r" + std::to_string(g.province_region[p] + 1));
        out.put(", \"terrain\": ");
        out.quoted(is_sea ? "shallow_sea" : terrain_key(g.terrain[static_cast<size_t>(cell)]));
        out.put(", \"adj\": [");
        const auto& adj = g.province_adj[p];
        for (size_t k = 0; k < adj.size(); ++k) {
            out.quoted(province_key(adj[k]));
            if (k + 1 < adj.size()) out.put(", ");
        }
        out.put("]");
        bool coastal = false;
        if (!is_sea) {
            for (int nb : adj) {
                if (g.province_state[static_cast<size_t>(nb)] < 0) coastal = true;
            }
        }
        out.put(", \"x\": "); out.num(x);
        out.put(", \"y\": "); out.num(y);
        out.put(", \"coastal\": "); out.boolean(coastal);
        out.put(", \"is_sea\": "); out.boolean(is_sea);
        out.put(", \"victory_points\": "); out.num(g.province_vp[p]);
        out.put(", \"infrastructure\": "); out.num(is_sea ? 0 : g.province_infra[p]);
        out.put(", \"population\": "); out.num(is_sea ? 0 : g.province_pop[p]);
        out.put(", \"resources\": {");
        bool first = true;
        for (int r = 0; r < R_COUNT; ++r) {
            if (g.province_res[r][p] <= 0) continue;
            if (!first) out.put(", ");
            first = false;
            out.quoted(resource_key(r));
            out.put(": ");
            out.num(g.province_res[r][p]);
        }
        out.put("}");
        out.put(", \"supply_hub\": "); out.boolean(g.province_hub[p] != 0);
        out.put(", \"railway_level\": "); out.num(is_sea ? 0 : g.province_rail[p]);
        out.put("}");
        if (p + 1 < province_count) out.put(",");
        out.put(nl);
    }
    out.put("  ]"); out.put(nl);
    out.put("}"); out.put(nl);
    return out.s;
}

void report(const Gen& g, int sea_regions) {
    const size_t provinces = g.province_adj.size();
    size_t land = 0;
    size_t sea = 0;
    for (size_t p = 0; p < provinces; ++p) {
        if (g.province_state[p] < 0) {
            ++sea;
        } else {
            ++land;
        }
    }
    std::printf("world: %zu provinces (%zu land, %zu sea), %zu states, %zu regions\n",
                provinces, land, sea, g.state_cells.size(),
                g.region_states.size() + static_cast<size_t>(sea_regions));
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) return 2;

    Gen g;
    g.args = args;
    g.W = args.size;
    g.H = args.size;

    generate_land(g);
    classify_cells(g);
    number_provinces(g);
    build_adjacency(g);
    group_states(g);
    group_regions(g);
    int sea_regions = 0;
    group_sea_regions(g, &sea_regions);
    province_attributes(g);

    const std::string text = write_map(g);
    std::FILE* f = std::fopen(args.out.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "genmap: cannot write %s\n", args.out.c_str());
        return 1;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (written != text.size()) {
        std::fprintf(stderr, "genmap: short write to %s\n", args.out.c_str());
        return 1;
    }
    report(g, sea_regions);
    return 0;
}
