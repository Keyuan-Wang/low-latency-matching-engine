/**
 * @file phase1_benchmark.cpp
 * @brief Phase-1 benchmark harness with latency and in-process PMC modes.
 *
 * Provides two complementary measurement modes:
 *   - Latency mode: wall-clock timing via steady_clock, reporting average,
 *     percentiles (p50/p95/p99) and throughput (ops/s).
 *   - PMC mode: in-process Linux perf_event counters for cycles, instructions,
 *     branches, branch-misses, LLC load/store misses, and cache misses — all
 *     reported per-operation.
 *
 * Each run begins with a configurable warmup phase (@p warmup_iters) during
 * which the same operation is executed but neither timed nor measured by PMC,
 * ensuring steady-state CPU frequency and cache behaviour.
 *
 * Five synthetic scenarios exercise the OrderBook under different access
 * patterns: limit order resting, deep-cross, market sweep, cancel-miss, and
 * duplicate-ID rejection.  The book is pre-filled to a configurable size
 * before each operation so that latency and PMC metrics reflect realistic
 * working-set pressure.
 *
 * @par Usage examples
 * @code
 *   # Latency run, 5 000 iterations, lmt_rest scenario
 *   phase1_bench --mode latency --scenario lmt_rest --iters 5000
 *
 *   # PMC run with custom prefilled book depth
 *   phase1_bench --mode pmc --orders 50000 --levels 500 --iters 1000
 * @endcode
 */

#include "matching/order_book.hpp"

#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

/** @brief Benchmark execution mode: wall-clock latency or hardware PMC counters. */
enum class RunMode { Latency, Pmc };

/**
 * @brief Aggregated command-line arguments.
 *
 * All fields have safe defaults so the benchmark can run with zero arguments.
 * Every member corresponds to a `--key value` CLI flag.
 */
struct Args {
  RunMode mode = RunMode::Latency;       ///< Benchmark mode (latency | pmc)
  std::string scenario = "lmt_rest";     ///< Scenario name
  std::uint64_t orders = 10000;          ///< Number of orders to prefill the book with
  std::uint64_t levels = 100;            ///< Number of price levels in the prefilled book
  std::uint64_t warmup_iters = 200;      ///< Warmup iterations (not recorded)
  std::uint64_t iters = 2000;            ///< Number of iterations (operations to measure)
  std::uint64_t seed = 42;               ///< PRNG seed (reserved for future use)
  std::uint64_t trial_id = 1;            ///< Trial-identifier for multi-trial campaigns
  std::string out_csv;                   ///< Optional path to append CSV results (header auto-written if file is new/empty)
};

/**
 * @brief Per-operation PMC counter averages.
 *
 * Each field holds the mean value of one hardware counter across all
 * iterations, normalised to a single operation.  Derived fields (cpi,
 * branch_miss_rate, llc_miss_per_op) are computed after summation to
 * avoid divide-by-iteration bias.
 */
struct PmcPerOp {
  double cycles_per_op{0.0};           ///< CPU cycles per operation
  double instructions_per_op{0.0};     ///< Retired instructions per operation
  double branches_per_op{0.0};         ///< Branch instructions per operation
  double branch_misses_per_op{0.0};    ///< Mispredicted branches per operation
  double llc_load_misses_per_op{0.0};  ///< Last-level cache load misses per operation
  double llc_store_misses_per_op{0.0}; ///< Last-level cache store misses per operation
  double cache_misses_per_op{0.0};     ///< Total cache-miss events per operation
  double cpi{0.0};                     ///< Cycles per instruction (derived)
  double branch_miss_rate{0.0};        ///< Branch-miss / branch ratio (derived)
  double llc_miss_per_op{0.0};         ///< Aggregate LLC misses per operation (derived)
};

/**
 * @brief Thin wrapper around the perf_event_open syscall.
 * @param hw_event  Per-counter configuration.
 * @param pid       Target PID (0 = current thread).
 * @param cpu       CPU (-1 = any).
 * @param group_fd  Leader FD for event-groups (-1 = standalone).
 * @param flags     perf_event_open flags.
 * @return          File descriptor on success, -1 on error.
 */
static int perf_event_open(struct perf_event_attr* hw_event, pid_t pid, int cpu,
                           int group_fd, unsigned long flags) {
  return static_cast<int>(
      syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

/**
 * @brief Manages a group of Linux perf-event counters.
 *
 * Opens seven hardware counters as a single event-group so they can be
 * started, stopped and read atomically via the leader FD.  Counters run
 * in user-space only (exclude_kernel=1, exclude_hv=1) and measure the
 * current thread.
 *
 * Counter order (index 0-6):
 *   0: CPU cycles
 *   1: Instructions retired
 *   2: Branch instructions
 *   3: Branch misses
 *   4: LLC load misses
 *   5: LLC store misses
 *   6: Cache misses
 */
class PerfGroup {
 public:
  /**
   * @brief Open all seven counters as a single event-group.
   * @return true if every counter was opened successfully.
   */
  bool open() {
    close_all();
    if (!open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1)) return false;
    if (!open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, leader_fd_)) return false;
    if (!open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS, leader_fd_)) return false;
    if (!open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, leader_fd_)) return false;
    if (!open_counter(PERF_TYPE_HW_CACHE,
                      PERF_COUNT_HW_CACHE_LL |
                          (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                          (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
                      leader_fd_))
      return false;
    if (!open_counter(PERF_TYPE_HW_CACHE,
                      PERF_COUNT_HW_CACHE_LL |
                          (PERF_COUNT_HW_CACHE_OP_WRITE << 8) |
                          (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
                      leader_fd_))
      return false;
    if (!open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, leader_fd_)) return false;
    return true;
  }

  /**
   * @brief Reset and enable the entire counter group.
   * @return true on success.
   */
  bool reset_enable() const {
    if (leader_fd_ < 0) return false;
    if (ioctl(leader_fd_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == -1) return false;
    if (ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == -1) return false;
    return true;
  }

  /**
   * @brief Disable (freeze) the entire counter group.
   * @return true on success.
   */
  bool disable() const {
    if (leader_fd_ < 0) return false;
    return ioctl(leader_fd_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != -1;
  }

  /**
   * @brief Atomically read all counter values from the group.
   * @param[out] out  Array filled with the 7 counter values (index order
   *                  matches the open() sequence).
   * @return true on successful read.
   */
  bool read_values(std::array<std::uint64_t, 7>& out) const {
    if (leader_fd_ < 0) return false;
    struct ReadData {
      std::uint64_t nr;
      std::uint64_t values[7];
    } data{};

    const ssize_t n = read(leader_fd_, &data, sizeof(data));
    if (n != static_cast<ssize_t>(sizeof(data)) || data.nr != 7) {
      return false;
    }
    for (std::size_t i = 0; i < 7; ++i) {
      out[i] = data.values[i];
    }
    return true;
  }

  /** @brief Close all opened counter FDs. */
  ~PerfGroup() { close_all(); }

 private:
  /**
   * @brief Open a single counter and join it to the group.
   * @param type     PERF_TYPE_* (hardware, cache, etc.).
   * @param config   PERF_COUNT_HW_* or PERF_COUNT_HW_CACHE_* encoding.
   * @param group_fd Leader FD, or -1 to make this counter the leader.
   * @return true if the counter was opened and added.
   */
  bool open_counter(std::uint32_t type, std::uint64_t config, int group_fd) {
    struct perf_event_attr pe {};
    pe.type = type;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.read_format = PERF_FORMAT_GROUP;

    const int fd = perf_event_open(&pe, 0, -1, group_fd, 0);
    if (fd == -1) {
      std::cerr << "perf_event_open failed: " << std::strerror(errno) << "\n";
      close_all();
      return false;
    }

    if (group_fd == -1) {
      leader_fd_ = fd;
    }
    fds_.push_back(fd);
    return true;
  }

  /** @brief Close every opened counter FD and reset state. */
  void close_all() {
    for (const int fd : fds_) {
      if (fd >= 0) close(fd);
    }
    fds_.clear();
    leader_fd_ = -1;
  }

  int leader_fd_{-1};           ///< Group-leader FD, or -1 if not opened
  std::vector<int> fds_{};      ///< All opened counter FDs (leader included)
};

/**
 * @brief Parse CLI arguments into an Args struct.
 *
 * Supported flags: --mode, --scenario, --orders, --levels, --warmup-iters,
 * --iters, --seed, --trial-id, --out.  Unknown flags are silently ignored.
 *
 * @param argc  Argument count from main().
 * @param argv  Argument vector from main().
 * @return      Populated Args (exits on fatal errors).
 */
static Args parse_args(int argc, char** argv) {
  Args a{};
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&](std::uint64_t& v) { v = std::stoull(argv[++i]); };
    if (s == "--mode") {
      const std::string mode = argv[++i];
      if (mode == "latency") a.mode = RunMode::Latency;
      else if (mode == "pmc") a.mode = RunMode::Pmc;
      else {
        std::cerr << "unknown mode: " << mode << "\n";
        std::exit(2);
      }
    } else if (s == "--scenario") a.scenario = argv[++i];
    else if (s == "--orders") next(a.orders);
    else if (s == "--levels") next(a.levels);
    else if (s == "--warmup-iters") next(a.warmup_iters);
    else if (s == "--iters") next(a.iters);
    else if (s == "--seed") next(a.seed);
    else if (s == "--trial-id") next(a.trial_id);
    else if (s == "--out") a.out_csv = argv[++i];
  }
  return a;
}

/**
 * @brief Compute the p-th percentile of a sorted latency vector.
 *
 * Uses linear interpolation between adjacent values (type R-7, same as
 * numpy's default).  The input vector is sorted in-place.
 *
 * @param v  Raw latency samples (ns).
 * @param p  Desired percentile in [0.0, 1.0].
 * @return   Interpolated percentile value.
 */
static double percentile_ns(std::vector<std::uint64_t> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double pos = p * (v.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(pos);
  const std::size_t hi = std::min(lo + 1, v.size() - 1);
  const double frac = pos - lo;
  return v[lo] * (1.0 - frac) + v[hi] * frac;
}

/**
 * @brief Prefill an OrderBook with resting sell orders at increasing prices.
 *
 * Distributes @p orders across @p levels price points (1 tick apart starting
 * at 1000) so the book has a controlled depth before the measured operation.
 *
 * @param book     Target OrderBook.
 * @param orders   Total number of orders to insert.
 * @param levels   Number of distinct price levels.
 * @param id_base  Starting order-ID offset.
 */
static void prefill_book(matching::OrderBook& book, std::uint64_t orders,
                         std::uint64_t levels, std::uint64_t id_base) {
  const std::uint64_t per_level =
      std::max<std::uint64_t>(1, orders / std::max<std::uint64_t>(1, levels));
  std::uint64_t id = id_base;
  for (std::uint64_t lvl = 0; lvl < levels; ++lvl) {
    const std::int64_t ask_price = 1000 + static_cast<std::int64_t>(lvl);
    for (std::uint64_t j = 0; j < per_level; ++j) {
      (void)book.add_limit_order(id, matching::Side::Sell, ask_price, 1, id);
      ++id;
    }
  }
}

/**
 * @brief Precondition the OrderBook for a given scenario.
 *
 * Scenarios that need a deep book (lmt_cross_deep, mkt_sweep_deep, cxl_miss,
 * dup_reject) get pre-filled via prefill_book().  dup_reject additionally
 * inserts a specific order that the measured operation will attempt to
 * duplicate.
 *
 * @param book  Target OrderBook.
 * @param a     Benchmark arguments (scenario, orders, levels).
 * @param id0   Base order-ID offset for this iteration.
 */
static void setup_scenario(matching::OrderBook& book, const Args& a, std::uint64_t id0) {
  if (a.scenario == "lmt_cross_deep" || a.scenario == "mkt_sweep_deep" ||
      a.scenario == "cxl_miss" || a.scenario == "dup_reject") {
    prefill_book(book, a.orders, a.levels, id0);
  }
  if (a.scenario == "dup_reject") {
    (void)book.add_limit_order(7, matching::Side::Buy, 900, 10, id0 + 1);
  }
}

/**
 * @brief Execute one benchmark operation according to the active scenario.
 *
 * Dispatches to the corresponding OrderBook method and increments @p ok when
 * the operation completes with the expected status code (e.g. Success for
 * lmt_rest, UnknownOrderId for cxl_miss).
 *
 * @param a         Benchmark arguments (selects the scenario).
 * @param book      OrderBook to operate on.
 * @param id0       Base order-ID offset.
 * @param iter_idx  Current iteration index (used for deterministic ID
 *                  generation in the cxl_miss scenario).
 * @param ok        [in,out] Counter of successful (expected-outcome) ops.
 * @return true if the scenario is known and was dispatched.
 */
static bool run_one_operation(const Args& a, matching::OrderBook& book, std::uint64_t id0,
                              std::uint64_t iter_idx, std::uint64_t& ok) {
  matching::AddResult r{};
  matching::ErrorCode c = matching::ErrorCode::Success;

  if (a.scenario == "lmt_rest") {
    r = book.add_limit_order(id0 + 2, matching::Side::Buy, 900, 10, id0 + 2);
    if (r.code == matching::ErrorCode::Success) ++ok;
    return true;
  }
  if (a.scenario == "lmt_cross_deep") {
    r = book.add_limit_order(id0 + 3, matching::Side::Buy, 5000,
                             static_cast<std::uint32_t>(a.levels), id0 + 3);
    if (r.code == matching::ErrorCode::Success) ++ok;
    return true;
  }
  if (a.scenario == "mkt_sweep_deep") {
    r = book.add_market_order(id0 + 4, matching::Side::Buy,
                              static_cast<std::uint32_t>(a.levels * 2), id0 + 4);
    if (r.code == matching::ErrorCode::Success ||
        r.code == matching::ErrorCode::MarketRemainderCancelled) {
      ++ok;
    }
    return true;
  }
  if (a.scenario == "cxl_miss") {
    c = book.cancel_order(9'999'999'999ULL - iter_idx);
    if (c == matching::ErrorCode::UnknownOrderId) ++ok;
    return true;
  }
  if (a.scenario == "dup_reject") {
    r = book.add_limit_order(7, matching::Side::Sell, 2000, 10, id0 + 5);
    if (r.code == matching::ErrorCode::DuplicateOrderId) ++ok;
    return true;
  }
  return false;
}

/**
 * @brief Ensure a CSV file has a header row, writing one if the file is empty or
 *        doesn't exist.
 * @param path   CSV file path.
 * @param header CSV header line (without trailing newline).
 */
static void ensure_csv_header(const std::string& path, const std::string& header) {
  if (path.empty()) return;
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) return;
  std::ofstream f(path, std::ios::app);
  f << header << "\n";
}

/**
 * @brief Run the latency benchmark.
 *
 * A warmup phase executes @p a.warmup_iters iterations without timing to
 * stabilise CPU frequency and cache state.  A subsequent measurement phase
 * times each operation via steady_clock.  Results are printed to stdout and,
 * when @p a.out_csv is set, appended to a CSV file (header written
 * automatically if the file is new or empty).
 *
 * @param a  Benchmark arguments.
 * @return   0 on success, 2 on unknown scenario.
 */
static int run_latency(const Args& a) {
  std::vector<std::uint64_t> lat_ns;
  lat_ns.reserve(a.iters);
  std::uint64_t ok = 0;

  // Warmup phase: execute the same scenario shape without recording latency.
  std::uint64_t warmup_ok = 0;
  for (std::uint64_t i = 0; i < a.warmup_iters; ++i) {
    matching::OrderBook book;
    const std::uint64_t id0 = 100'000ULL + i * 10'000ULL;
    setup_scenario(book, a, id0);
    if (!run_one_operation(a, book, id0, i, warmup_ok)) {
      std::cerr << "unknown scenario: " << a.scenario << "\n";
      return 2;
    }
  }

  for (std::uint64_t i = 0; i < a.iters; ++i) {
    matching::OrderBook book;
    const std::uint64_t id0 = 1'000'000ULL + i * 10'000ULL;
    setup_scenario(book, a, id0);

    const auto t0 = Clock::now();
    if (!run_one_operation(a, book, id0, i, ok)) {
      std::cerr << "unknown scenario: " << a.scenario << "\n";
      return 2;
    }
    const auto t1 = Clock::now();
    lat_ns.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }

  const double sum_ns = std::accumulate(lat_ns.begin(), lat_ns.end(), 0.0);
  const double avg = sum_ns / static_cast<double>(a.iters);
  const double p50 = percentile_ns(lat_ns, 0.50);
  const double p95 = percentile_ns(lat_ns, 0.95);
  const double p99 = percentile_ns(lat_ns, 0.99);
  const double ops_s = (avg > 0.0) ? (1e9 / avg) : 0.0;

  std::cout << "mode=latency"
            << " trial_id=" << a.trial_id
            << " scenario=" << a.scenario
            << " orders=" << a.orders
            << " levels=" << a.levels
            << " warmup_iters=" << a.warmup_iters
            << " iters=" << a.iters
            << " avg_ns=" << avg
            << " p50_ns=" << p50
            << " p95_ns=" << p95
            << " p99_ns=" << p99
            << " ops_s=" << ops_s
            << " ok=" << ok << "\n";

  if (!a.out_csv.empty()) {
    ensure_csv_header(a.out_csv,
                      "mode,trial_id,scenario,orders,levels,warmup_iters,iters,seed,"
                      "avg_ns,p50_ns,p95_ns,p99_ns,ops_s,ok");
    std::ofstream f(a.out_csv, std::ios::app);
    f << "latency,"
      << a.trial_id << ","
      << a.scenario << ","
      << a.orders << ","
      << a.levels << ","
      << a.warmup_iters << ","
      << a.iters << ","
      << a.seed << ","
      << avg << ","
      << p50 << ","
      << p95 << ","
      << p99 << ","
      << ops_s << ","
      << ok << "\n";
  }

  return 0;
}

/**
 * @brief Run the PMC (performance-counter) benchmark.
 *
 * A warmup phase executes @p a.warmup_iters iterations without PMC
 * measurement.  Then each measured iteration wraps with perf_event group
 * enable/disable so that hardware counters only accumulate during the
 * benchmarked operation.  Counter totals are summed and divided to produce
 * per-op averages.  Prints results to stdout and, when @p a.out_csv is set,
 * appends to a CSV file (header written automatically if the file is new or
 * empty).
 *
 * Requires Linux perf_event access (CAP_PERFMON or
 * /proc/sys/kernel/perf_event_paranoid <= 1).
 *
 * @param a  Benchmark arguments.
 * @return   0 on success, 2 on unknown scenario, 3 on PMC init/read error.
 */
static int run_pmc(const Args& a) {
  PerfGroup perf{};
  if (!perf.open()) {
    std::cerr << "failed to initialize perf counters (need Linux perf support/permissions)\n";
    return 3;
  }

  std::array<std::uint64_t, 7> totals{};
  std::uint64_t ok = 0;

  // Warmup phase: execute scenario without reading/enabling PMCs.
  std::uint64_t warmup_ok = 0;
  for (std::uint64_t i = 0; i < a.warmup_iters; ++i) {
    matching::OrderBook book;
    const std::uint64_t id0 = 100'000ULL + i * 10'000ULL;
    setup_scenario(book, a, id0);
    if (!run_one_operation(a, book, id0, i, warmup_ok)) {
      std::cerr << "unknown scenario: " << a.scenario << "\n";
      return 2;
    }
  }

  for (std::uint64_t i = 0; i < a.iters; ++i) {
    matching::OrderBook book;
    const std::uint64_t id0 = 1'000'000ULL + i * 10'000ULL;
    setup_scenario(book, a, id0);

    if (!perf.reset_enable()) {
      std::cerr << "perf reset/enable failed\n";
      return 3;
    }

    if (!run_one_operation(a, book, id0, i, ok)) {
      std::cerr << "unknown scenario: " << a.scenario << "\n";
      return 2;
    }

    if (!perf.disable()) {
      std::cerr << "perf disable failed\n";
      return 3;
    }

    std::array<std::uint64_t, 7> values{};
    if (!perf.read_values(values)) {
      std::cerr << "perf read failed\n";
      return 3;
    }
    for (std::size_t j = 0; j < totals.size(); ++j) {
      totals[j] += values[j];
    }
  }

  const double denom = static_cast<double>(a.iters);
  PmcPerOp out{};
  out.cycles_per_op = totals[0] / denom;
  out.instructions_per_op = totals[1] / denom;
  out.branches_per_op = totals[2] / denom;
  out.branch_misses_per_op = totals[3] / denom;
  out.llc_load_misses_per_op = totals[4] / denom;
  out.llc_store_misses_per_op = totals[5] / denom;
  out.cache_misses_per_op = totals[6] / denom;
  out.cpi = (out.instructions_per_op > 0.0)
                ? (out.cycles_per_op / out.instructions_per_op)
                : 0.0;
  out.branch_miss_rate = (out.branches_per_op > 0.0)
                             ? (out.branch_misses_per_op / out.branches_per_op)
                             : 0.0;
  out.llc_miss_per_op = out.llc_load_misses_per_op + out.llc_store_misses_per_op;

  std::cout << "mode=pmc"
            << " trial_id=" << a.trial_id
            << " scenario=" << a.scenario
            << " orders=" << a.orders
            << " levels=" << a.levels
            << " warmup_iters=" << a.warmup_iters
            << " iters=" << a.iters
            << " cycles_per_op=" << out.cycles_per_op
            << " instructions_per_op=" << out.instructions_per_op
            << " cpi=" << out.cpi
            << " branch_miss_rate=" << out.branch_miss_rate
            << " llc_miss_per_op=" << out.llc_miss_per_op
            << " ok=" << ok << "\n";

  if (!a.out_csv.empty()) {
    ensure_csv_header(a.out_csv,
                      "mode,trial_id,scenario,orders,levels,warmup_iters,iters,seed,"
                      "cycles_per_op,instructions_per_op,branches_per_op,branch_misses_per_op,"
                      "llc_load_misses_per_op,llc_store_misses_per_op,cache_misses_per_op,"
                      "cpi,branch_miss_rate,llc_miss_per_op,ok");
    std::ofstream f(a.out_csv, std::ios::app);
    f << "pmc,"
      << a.trial_id << ","
      << a.scenario << ","
      << a.orders << ","
      << a.levels << ","
      << a.warmup_iters << ","
      << a.iters << ","
      << a.seed << ","
      << out.cycles_per_op << ","
      << out.instructions_per_op << ","
      << out.branches_per_op << ","
      << out.branch_misses_per_op << ","
      << out.llc_load_misses_per_op << ","
      << out.llc_store_misses_per_op << ","
      << out.cache_misses_per_op << ","
      << out.cpi << ","
      << out.branch_miss_rate << ","
      << out.llc_miss_per_op << ","
      << ok << "\n";
  }

  return 0;
}

/**
 * @brief Program entry point.
 *
 * Parses arguments, then dispatches to run_latency() or run_pmc() based on
 * the selected mode.  Return codes are propagated as the process exit code.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return      0 on success, 2 on bad arguments / unknown scenario, 3 on PMC error.
 */
int main(int argc, char** argv) {
  const Args a = parse_args(argc, argv);
  if (a.mode == RunMode::Latency) return run_latency(a);
  return run_pmc(a);
}
