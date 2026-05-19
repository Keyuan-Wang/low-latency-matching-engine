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

/**
 * @brief Measurement backend used by the runner.
 */
enum class MetricMode {
	Latency,  ///< Use steady_clock wall time and report ns/op percentiles.
	Pmc       ///< Use Linux perf_event counters and report micro-architectural metrics.
};

/**
 * @brief Aggregated command-line arguments with safe defaults.
 *
 * Every field corresponds to a @c --key CLI flag. All numeric fields have
 * reasonable defaults so the benchmark can run with zero arguments.
 */
struct Args {
	MetricMode metric = MetricMode::Latency;  ///< Measurement mode (latency | pmc).
	std::uint64_t orders = 10000;             ///< Number of resting orders used for setup.
	std::uint64_t levels = 100;               ///< Distinct price levels in setup depth.
	std::uint64_t batch_size = 64;            ///< Ops per measured window (reduces timing overhead).
	std::uint64_t warmup_iters = 100;         ///< Number of unreported warmup iterations.
	std::uint64_t iters = 1000;               ///< Number of reported iterations.
	std::uint64_t trial_id = 1;               ///< User-controlled trial identifier.
	std::uint64_t seed = 42;                  ///< Reserved deterministic seed (future scenarios).
	std::string version_tag = "baseline";     ///< Human-readable version label.
	std::string commit_sha = "unknown";       ///< Source revision label for reproducibility.
	std::string out_csv;                      ///< Optional CSV output path (append mode).
};

/**
 * @brief Abstract benchmark scenario interface.
 *
 * Each concrete scenario must implement Name(), Setup(), RunOp(), and
 * Teardown(). The runner calls Setup+Teardown outside the timing window
 * and only measures the cost of RunOp() calls, so book construction and
 * pool allocation are never included in latency/PMC samples.
 */
class IBenchScenario {
 public:
	virtual ~IBenchScenario() = default;

	/** @return C-string scenario name used in stdout and CSV output. */
	virtual const char* Name() const = 0;

	/**
	 * @brief Build the measured state — called once per iteration, untimed.
	 * @param args     Parsed CLI arguments.
	 * @param iter_idx Monotonically increasing iteration index (global across
	 *                 warmup + measured phases). Use this to derive unique IDs.
	 */
	virtual void Setup(const Args& args, std::uint64_t iter_idx) = 0;

	/**
	 * @brief Run one measured operation — called @p batch_size times per
	 *        iteration inside the timing/PMC window.
	 * @param args      Parsed CLI arguments.
	 * @param iter_idx  Same iteration index passed to Setup() for this iteration.
	 * @param batch_idx  0 … batch_size-1 within the current iteration.
	 * @param ok        [in,out] Incremented when the operation completes with
	 *                  the expected status code.
	 * @return true on success, false on fatal error.
	 */
	virtual bool RunOp(const Args& args,
										 std::uint64_t iter_idx,
										 std::uint64_t batch_idx,
										 std::uint64_t& ok) = 0;

	/**
	 * @brief Tear down the state built by Setup() — called once per iteration,
	 *        untimed.
	 */
	virtual void Teardown() = 0;
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
int RunScenario(IBenchScenario& scenario, int argc, char** argv);

}  // namespace benchmark_runner
