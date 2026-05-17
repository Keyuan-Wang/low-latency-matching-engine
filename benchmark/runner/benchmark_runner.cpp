/**
 * @file benchmark_runner.cpp
 * @brief Measurement harness: CLI parsing, warmup loop, timing/PMC loop, and output.
 *
 * Contains ParseArgs() (all supported --key flags) and RunScenario() which
 * drives the full benchmark lifecycle for any IBenchScenario implementation.
 * This file has zero knowledge of individual scenario logic — it only calls
 * PrepareAndRun() and Name() through the virtual interface.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>

namespace benchmark_runner {
namespace {

using Clock = std::chrono::steady_clock;

/**
 * @brief Parse CLI arguments into an Args struct.
 *
 * Supported flags: --metric, --orders, --levels, --batch-size, --warmup-iters,
 * --iters, --trial-id, --seed, --version-tag, --commit-sha, --out.
 * Unknown flags are silently ignored. On fatal parse errors the process exits
 * with code 2.
 *
 * @param argc  Argument count from main().
 * @param argv  Argument vector from main().
 * @return Populated Args with defaults applied for any unspecified flags.
 */
Args ParseArgs(int argc, char** argv) {
  Args args{};
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&](std::uint64_t& v) { v = std::stoull(argv[++i]); };
    if (s == "--metric") {
      const std::string mode = argv[++i];
      if (mode == "latency") args.metric = MetricMode::Latency;
      else if (mode == "pmc") args.metric = MetricMode::Pmc;
      else {
        std::cerr << "unknown metric: " << mode << "\n";
        std::exit(2);
      }
    } else if (s == "--orders") next(args.orders);
    else if (s == "--levels") next(args.levels);
    else if (s == "--batch-size") next(args.batch_size);
    else if (s == "--warmup-iters") next(args.warmup_iters);
    else if (s == "--iters") next(args.iters);
    else if (s == "--trial-id") next(args.trial_id);
    else if (s == "--seed") next(args.seed);
    else if (s == "--version-tag") args.version_tag = argv[++i];
    else if (s == "--commit-sha") args.commit_sha = argv[++i];
    else if (s == "--out") args.out_csv = argv[++i];
  }
  return args;
}

}  // namespace

int RunScenario(const IBenchScenario& scenario, int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  std::uint64_t ok = 0;
  std::uint64_t op_counter = 0;

  // --- warmup phase: execute without recording to stabilise CPU / cache ---
  for (std::uint64_t i = 0; i < args.warmup_iters; ++i) {
    for (std::uint64_t b = 0; b < args.batch_size; ++b) {
      if (!scenario.PrepareAndRun(args, op_counter++, ok)) return 2;
    }
  }

  std::vector<double> latency_per_op_ns;
  latency_per_op_ns.reserve(args.iters);
  std::array<std::uint64_t, 5> totals{};
  std::uint64_t measured_ops = 0;

  PerfGroup perf{};
  if (args.metric == MetricMode::Pmc && !perf.Open()) {
    std::cerr << "failed to initialize PMCs (Linux perf support/permissions required)\n";
    return 3;
  }

  // --- measurement phase ---
  // Each iteration runs batch_size operations. Latency mode records the
  // wall-clock duration and divides by batch_size for per-op amortised cost.
  // PMC mode wraps the batch with perf enable/disable so counters only
  // accumulate during the measured operations.
  for (std::uint64_t i = 0; i < args.iters; ++i) {
    if (args.metric == MetricMode::Pmc && !perf.ResetEnable()) return 3;

    const auto t0 = Clock::now();
    for (std::uint64_t b = 0; b < args.batch_size; ++b) {
      if (!scenario.PrepareAndRun(args, op_counter++, ok)) return 2;
    }
    const auto t1 = Clock::now();

    if (args.metric == MetricMode::Latency) {
      const double ns = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      latency_per_op_ns.push_back(ns / static_cast<double>(args.batch_size));
    } else {
      if (!perf.Disable()) return 3;
      std::array<std::uint64_t, 5> vals{};
      if (!perf.ReadValues(vals)) return 3;
      for (std::size_t j = 0; j < totals.size(); ++j) totals[j] += vals[j];
      measured_ops += args.batch_size;
    }
  }

  const char* scenario_name = scenario.Name();

  // --- latency output ---
  if (args.metric == MetricMode::Latency) {
    const double avg = std::accumulate(latency_per_op_ns.begin(), latency_per_op_ns.end(), 0.0) /
                       static_cast<double>(latency_per_op_ns.size());
    const double p50 = Percentile(latency_per_op_ns, 0.50);
    const double p95 = Percentile(latency_per_op_ns, 0.95);
    const double p99 = Percentile(latency_per_op_ns, 0.99);
    const double ops_s = (avg > 0.0) ? (1e9 / avg) : 0.0;

    std::cout << "mode=latency"
              << " scenario=" << scenario_name
              << " version_tag=" << args.version_tag
              << " commit_sha=" << args.commit_sha
              << " trial_id=" << args.trial_id
              << " orders=" << args.orders
              << " levels=" << args.levels
              << " batch_size=" << args.batch_size
              << " warmup_iters=" << args.warmup_iters
              << " iters=" << args.iters
              << " avg_ns=" << avg
              << " p50_ns=" << p50
              << " p95_ns=" << p95
              << " p99_ns=" << p99
              << " ops_s=" << ops_s
              << " ok=" << ok << "\n";

    if (!args.out_csv.empty()) {
      EnsureCsvHeader(
          args.out_csv,
          "mode,scenario,version_tag,commit_sha,trial_id,orders,levels,batch_size,"
          "warmup_iters,iters,seed,avg_ns,p50_ns,p95_ns,p99_ns,ops_s,ok");
      std::ofstream f(args.out_csv, std::ios::app);
      f << "latency," << scenario_name << "," << args.version_tag << "," << args.commit_sha << ","
        << args.trial_id << "," << args.orders << "," << args.levels << "," << args.batch_size << ","
        << args.warmup_iters << "," << args.iters << "," << args.seed << ","
        << avg << "," << p50 << "," << p95 << "," << p99 << "," << ops_s << "," << ok << "\n";
    }
    return 0;
  }

  // --- PMC output ---
  const double denom = static_cast<double>(measured_ops);
  const double cycles = totals[0] / denom;
  const double instructions = totals[1] / denom;
  const double branches = totals[2] / denom;
  const double branch_misses = totals[3] / denom;
  const double cache_misses = totals[4] / denom;
  const double cpi = (instructions > 0.0) ? (cycles / instructions) : 0.0;
  const double branch_miss_rate = (branches > 0.0) ? (branch_misses / branches) : 0.0;

  std::cout << "mode=pmc"
            << " scenario=" << scenario_name
            << " version_tag=" << args.version_tag
            << " commit_sha=" << args.commit_sha
            << " trial_id=" << args.trial_id
            << " orders=" << args.orders
            << " levels=" << args.levels
            << " batch_size=" << args.batch_size
            << " warmup_iters=" << args.warmup_iters
            << " iters=" << args.iters
            << " cycles_per_op=" << cycles
            << " instructions_per_op=" << instructions
            << " branches_per_op=" << branches
            << " branch_misses_per_op=" << branch_misses
            << " cache_misses_per_op=" << cache_misses
            << " cpi=" << cpi
            << " branch_miss_rate=" << branch_miss_rate
            << " ok=" << ok << "\n";

  if (!args.out_csv.empty()) {
    EnsureCsvHeader(
        args.out_csv,
        "mode,scenario,version_tag,commit_sha,trial_id,orders,levels,batch_size,"
        "warmup_iters,iters,seed,cycles_per_op,instructions_per_op,branches_per_op,"
        "branch_misses_per_op,cache_misses_per_op,cpi,branch_miss_rate,ok");
    std::ofstream f(args.out_csv, std::ios::app);
    f << "pmc," << scenario_name << "," << args.version_tag << "," << args.commit_sha << ","
      << args.trial_id << "," << args.orders << "," << args.levels << "," << args.batch_size << ","
      << args.warmup_iters << "," << args.iters << "," << args.seed << ","
      << cycles << "," << instructions << "," << branches << ","
      << branch_misses << "," << cache_misses << "," << cpi << ","
      << branch_miss_rate << "," << ok << "\n";
  }

  return 0;
}

}  // namespace benchmark_runner
