/**
 * @file benchmark_runner.hpp
 * @brief Modular benchmark runner interface.
 *
 * Defines the IBenchScenario abstraction, configuration structs, and the
 * entry point RunScenario(). Each benchmark scenario inherits from
 * IBenchScenario, supplies its own PrepareAndRun() logic, and plugs into
 * the shared measurement infrastructure without modifying the runner.
 */

#pragma once

#include <cstdint>
#include <string>

namespace benchmark_runner {

/** @brief Measurement mode: wall-clock latency or hardware performance counters. */
enum class MetricMode { Latency, Pmc };

/**
 * @brief Aggregated command-line arguments with safe defaults.
 *
 * Every field corresponds to a @c --key CLI flag. All numeric fields have
 * reasonable defaults so the benchmark can run with zero arguments.
 */
struct Args {
  MetricMode metric = MetricMode::Latency;       ///< Measurement mode (latency | pmc)
  std::uint64_t orders = 10000;                  ///< Number of orders to prefill the book with
  std::uint64_t levels = 100;                    ///< Number of price levels in the prefilled book
  std::uint64_t batch_size = 64;                 ///< Operations per measurement sample (amortises timer/PMC overhead)
  std::uint64_t warmup_iters = 100;              ///< Warmup iterations (not recorded)
  std::uint64_t iters = 1000;                    ///< Number of measurement iterations
  std::uint64_t trial_id = 1;                    ///< Trial identifier for multi-trial campaigns
  std::uint64_t seed = 42;                       ///< PRNG seed (reserved for future use)
  std::string version_tag = "baseline";           ///< Human-readable version label (e.g. "baseline", "v2-skiplist")
  std::string commit_sha = "unknown";             ///< Git commit SHA for reproducibility
  std::string out_csv;                            ///< Optional path to append CSV results (header auto-written if file is new/empty)
};

/**
 * @brief Abstract benchmark scenario interface.
 *
 * Each concrete scenario must implement Name() and PrepareAndRun().
 * The runner calls these via virtual dispatch — it has no knowledge of
 * what the scenario does internally.
 */
class IBenchScenario {
 public:
  virtual ~IBenchScenario() = default;

  /** @return C-string scenario name used in stdout and CSV output. */
  virtual const char* Name() const = 0;

  /**
   * @brief Run one measured operation.
   * @param args    Parsed CLI arguments (prefill size, batch configuration, etc.).
   * @param op_idx  Globally monotonically increasing operation index. Use this
   *                to generate unique order IDs so that iterations never collide.
   * @param ok      [in,out] Incremented when the operation completes with the
   *                expected status code (e.g. Success for a resting limit order,
   *                UnknownOrderId for a cancel-miss).
   * @return true if the scenario was dispatched successfully, false on fatal error.
   */
  virtual bool PrepareAndRun(const Args& args, std::uint64_t op_idx,
                             std::uint64_t& ok) const = 0;
};

/**
 * @brief Run a full benchmark campaign for the given scenario.
 *
 * Parses CLI arguments, executes a warmup phase, then measures either
 * wall-clock latency or hardware PMC counters over @p iters iterations.
 * Results are printed to stdout and optionally appended to a CSV file.
 *
 * @param scenario  Concrete scenario implementing IBenchScenario.
 * @param argc      Argument count (forwarded from main()).
 * @param argv      Argument vector (forwarded from main()).
 * @return 0 on success, 2 on bad arguments / unknown scenario, 3 on PMC init error.
 */
int RunScenario(const IBenchScenario& scenario, int argc, char** argv);

}  // namespace benchmark_runner
