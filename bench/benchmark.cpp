// ---------------------------------------------------------------------------
// benchmark.cpp — throughput + latency harness for the matching engine
// ---------------------------------------------------------------------------
//
// Two measurements are reported:
//
//   1. Latency  — single-threaded. Each pre-generated command is applied to
//      the book with a steady_clock measurement around it; we report mean and
//      percentile (p50/p99/p99.9) per-command latency. Generation is done up
//      front so only matching cost is timed.
//
//   2. Throughput — end-to-end through the lock-free SPSC pipeline. A producer
//      thread pushes every command across the ring buffer while the engine
//      thread matches; we report commands/sec over the wall-clock span.
//
// Usage:
//   obe_bench [--commands N] [--depth D] [--queue Q] [--seed S]
//             [--warmup W] [--csv] [--header]
//
// With --csv a single machine-readable row is printed (see header() for the
// column order); sweep.sh uses this to build a CSV across a parameter grid.

#include "obe/MatchingEngine.hpp"
#include "obe/OrderGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using obe::Command;

struct Options {
    std::size_t commands = 1'000'000;
    int depth = 50;
    std::size_t queue = 1u << 16;
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;
    std::size_t warmup = 50'000;
    bool csv = false;
    bool header = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if (a == "--commands") o.commands = std::stoull(next());
        else if (a == "--depth") o.depth = std::stoi(next());
        else if (a == "--queue") o.queue = std::stoull(next());
        else if (a == "--seed") o.seed = std::stoull(next());
        else if (a == "--warmup") o.warmup = std::stoull(next());
        else if (a == "--csv") o.csv = true;
        else if (a == "--header") o.header = true;
        else {
            std::cerr << "unknown arg: " << a << "\n";
            std::exit(2);
        }
    }
    return o;
}

double percentile(std::vector<std::uint64_t>& v, double p) {
    if (v.empty()) return 0.0;
    const std::size_t idx = static_cast<std::size_t>(
        p / 100.0 * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx),
                     v.end());
    return static_cast<double>(v[idx]);
}

std::vector<Command> generate(const Options& o) {
    obe::GeneratorConfig cfg;
    cfg.seed = o.seed;
    cfg.depth_levels = o.depth;
    obe::OrderGenerator gen(cfg);
    std::vector<Command> cmds;
    cmds.reserve(o.commands);
    for (std::size_t i = 0; i < o.commands; ++i) {
        cmds.push_back(gen.next());
    }
    return cmds;
}

// ---- Measurement 1: per-command latency (single thread) -------------------
struct LatencyResult {
    double mean_ns = 0, p50 = 0, p99 = 0, p999 = 0;
    std::uint64_t trades = 0;
};

LatencyResult measure_latency(const std::vector<Command>& cmds,
                              const Options& o) {
    obe::MatchingEngine engine(o.queue);
    std::vector<std::uint64_t> samples;
    samples.reserve(cmds.size());

    std::uint64_t total = 0;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const auto t0 = Clock::now();
        engine.apply(cmds[i]);
        const auto t1 = Clock::now();
        if (i >= o.warmup) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                t1 - t0).count();
            samples.push_back(static_cast<std::uint64_t>(ns));
            total += static_cast<std::uint64_t>(ns);
        }
    }

    LatencyResult r;
    r.trades = engine.stats().trades;
    if (!samples.empty()) {
        r.mean_ns = static_cast<double>(total) /
                    static_cast<double>(samples.size());
        r.p50 = percentile(samples, 50.0);
        r.p99 = percentile(samples, 99.0);
        r.p999 = percentile(samples, 99.9);
    }
    return r;
}

// ---- Measurement 2: end-to-end throughput (producer + consumer) -----------
double measure_throughput(const std::vector<Command>& cmds, const Options& o) {
    obe::MatchingEngine engine(o.queue);
    engine.start();

    const auto t0 = Clock::now();
    std::thread producer([&] {
        for (const auto& c : cmds) {
            // Back-pressure: spin until the consumer makes room.
            while (!engine.queue().push(c)) {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    engine.drain_and_join(); // sentinel handshake: drains the queue then joins
    const auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    return secs > 0 ? static_cast<double>(cmds.size()) / secs : 0.0;
}

void header() {
    std::cout << "depth,commands,throughput_ops_sec,mean_ns,p50_ns,p99_ns,"
                 "p999_ns,trades\n";
}

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    if (o.header) {
        header();
        return 0;
    }

    const std::vector<Command> cmds = generate(o);
    const LatencyResult lat = measure_latency(cmds, o);
    const double tput = measure_throughput(cmds, o);

    if (o.csv) {
        std::cout << o.depth << ',' << o.commands << ',' << tput << ','
                  << lat.mean_ns << ',' << lat.p50 << ',' << lat.p99 << ','
                  << lat.p999 << ',' << lat.trades << '\n';
    } else {
        std::cout << "=== order-book-engine benchmark ===\n"
                  << "commands     : " << o.commands << "\n"
                  << "depth levels : " << o.depth << "\n"
                  << "trades       : " << lat.trades << "\n"
                  << "--- latency (single-thread, ns/command) ---\n"
                  << "mean   : " << lat.mean_ns << "\n"
                  << "p50    : " << lat.p50 << "\n"
                  << "p99    : " << lat.p99 << "\n"
                  << "p99.9  : " << lat.p999 << "\n"
                  << "--- throughput (SPSC pipeline) ---\n"
                  << "ops/sec: " << tput << "\n";
    }
    return 0;
}
