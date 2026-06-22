/**
	* @file bench_common.hpp
	* @brief Shared utilities for benchmark scenarios and the measurement runner.
	*
	* Header-only library providing:
	*   - PerfGroup: RAII wrapper around Linux perf_event_open for in-process
	*     hardware performance counter measurement.
	*   - EnsureCsvHeader(): idempotent CSV header writer.
	*   - Percentile(): linear-interpolation percentile (R-7, matches numpy default).
	*   - PrefillSellBook(): populate an OrderBook with resting sell orders at
	*     evenly spaced price levels.
	*/

#pragma once

#include "matching_core/order_book.hpp"

#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace benchmark_runner {

struct PerfEventSpec {
	std::string name;
	std::uint64_t type = 0;
	std::uint64_t config = 0;
};

/**
	* @brief Minimal splitmix64 PRNG — ~2 ns/call, deterministic, seedable.
	*
	* Faster than std::mt19937 and friendlier to inlining.  Used inside
	* RunOp() to generate random order IDs/prices so that compilers and
	* branch predictors cannot lock onto a fixed constant across trials.
	*/
struct SplitMix64 {
	std::uint64_t state;

	explicit SplitMix64(std::uint64_t seed) : state(seed) {}

	std::uint64_t next() {
	state += 0x9e3779b97f4a7c15;
	std::uint64_t z = state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
	z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
	return z ^ (z >> 31);
	}
};

/**
	* @brief Thin wrapper around the perf_event_open syscall.
	* @param hw_event  Per-counter configuration.
	* @param pid       Target PID (0 = current thread).
	* @param cpu       CPU to monitor (-1 = any).
	* @param group_fd  Leader FD for event groups (-1 = standalone).
	* @param flags     perf_event_open flags (0 for default behaviour).
	* @return File descriptor on success, -1 on error (errno set).
	*/
inline int perf_event_open(struct perf_event_attr* hw_event, pid_t pid, int cpu,
													 int group_fd, unsigned long flags) {
	return static_cast<int>(
			syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

/**
	* @brief Manages a group of Linux perf_event hardware counters.
	*
	* Opens five counters as a single event group so they can be started,
	* stopped and read atomically via the leader FD. Counters run in
	* user-space only (exclude_kernel=1, exclude_hv=1) and measure the
	* current thread.
	*
	* Counter order (indices 0–4):
	*   0: CPU cycles
	*   1: Instructions retired
	*   2: Branch instructions
	*   3: Branch misses
	*   4: Cache misses
	*/
class PerfGroup {
	public:
	/**
	 * @brief Open all five counters as a single event group.
	 * @return true if every counter was opened successfully.
	 */
	bool Open() {
		return Open({
				{"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
				{"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
				{"branches", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
				{"branch_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
				{"cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
		});
	}

	/**
	 * @brief Open a caller-specified counter group.
	 *
	 * The first event becomes the group leader. All following events join the
	 * same group so ResetEnable(), Disable(), and ReadValues() operate on a
	 * single synchronized measurement window.
	 */
	bool Open(const std::vector<PerfEventSpec>& specs) {
		CloseAll();
		event_names_.clear();
		if (specs.empty()) return false;

		for (const auto& spec : specs) {
			const int group_fd = (leader_fd_ < 0) ? -1 : leader_fd_;
			if (!OpenCounter(spec.type, spec.config, group_fd)) {
				return false;
			}
			event_names_.push_back(spec.name);
		}
		return true;
	}

	/**
	 * @brief Reset all counters to zero and start counting.
	 * @return true on success.
	 */
	bool ResetEnable() const {
		if (leader_fd_ < 0) return false;
		// Reset before each measured batch to avoid accumulation across iterations.
		if (ioctl(leader_fd_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == -1) {
			return false;
		}
		if (ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == -1) {
			return false;
		}
		return true;
	}

	/**
	 * @brief Freeze (disable) the entire counter group.
	 * @return true on success.
	 */
	bool Disable() const {
		if (leader_fd_ < 0) return false;
		return ioctl(leader_fd_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != -1;
	}

	/**
	 * @brief Atomically read all counter values from the group.
	 * @param[out] out  Array filled with the 5 counter values (index order matches Open()).
	 * @return true on successful read.
	 */
	bool ReadValues(std::array<std::uint64_t, 5>& out) const {
		if (leader_fd_ < 0) return false;
		if (fds_.size() != out.size()) return false;
		struct ReadData {
			std::uint64_t nr;
			std::uint64_t values[5];
		} data{};
		const ssize_t n = read(leader_fd_, &data, sizeof(data));
		if (n != static_cast<ssize_t>(sizeof(data)) || data.nr != 5) {
			return false;
		}
		for (std::size_t i = 0; i < 5; ++i) out[i] = data.values[i];
		return true;
	}

	bool ReadValues(std::vector<std::uint64_t>& out) const {
		if (leader_fd_ < 0 || fds_.empty()) return false;

		std::vector<std::uint64_t> data(1 + fds_.size(), 0);
		const ssize_t expected =
				static_cast<ssize_t>(data.size() * sizeof(std::uint64_t));
		const ssize_t n = read(leader_fd_, data.data(), data.size() * sizeof(std::uint64_t));
		if (n != expected || data[0] != fds_.size()) {
			return false;
		}

		out.assign(data.begin() + 1, data.end());
		return true;
	}

	[[nodiscard]] const std::vector<std::string>& EventNames() const noexcept {
		return event_names_;
	}

	/** @brief Close all opened counter FDs. */
	~PerfGroup() { CloseAll(); }

	private:
	/**
	 * @brief Open a single counter and join it to the group.
	 * @param type     PERF_TYPE_* constant (hardware, cache, etc.).
	 * @param config   PERF_COUNT_HW_* or PERF_COUNT_HW_CACHE_* encoding.
	 * @param group_fd Leader FD, or -1 to make this counter the group leader.
	 * @return true if the counter was opened and added to the group.
	 */
	bool OpenCounter(std::uint64_t type, std::uint64_t config, int group_fd) {
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
			CloseAll();
			return false;
		}
		if (group_fd == -1) leader_fd_ = fd;
		fds_.push_back(fd);
		return true;
	}

	/** @brief Close every opened counter FD and reset internal state. */
	void CloseAll() {
		for (const int fd : fds_) {
			if (fd >= 0) close(fd);
		}
		fds_.clear();
		event_names_.clear();
		leader_fd_ = -1;
	}

	int leader_fd_{-1};           ///< Group-leader FD, or -1 if not opened
	std::vector<int> fds_{};      ///< All opened counter FDs (leader included)
	std::vector<std::string> event_names_{};
};

/**
	* @brief Ensure a CSV file has a header row.
	*
	* Writes @p header to @p path only if the file does not exist or is empty.
	* Subsequent calls are no-ops — no duplicate headers are ever written.
	*
	* @param path   CSV file path.
	* @param header CSV header line (without trailing newline).
	*/
inline void EnsureCsvHeader(const std::string& path, const std::string& header) {
	if (path.empty()) return;
	std::error_code ec;
	if (std::filesystem::exists(path, ec) &&
			std::filesystem::file_size(path, ec) > 0) {
		return;
	}
	std::ofstream f(path, std::ios::app);
	f << header << "\n";
}

/**
	* @brief Compute the p-th percentile of a numeric vector.
	*
	* Uses linear interpolation between adjacent values (type R-7, same as
	* numpy's default).
	*
	* @param values  Sample vector (taken by value; will be sorted in-place).
	* @param p       Desired percentile in [0.0, 1.0].
	* @return Interpolated percentile value, or 0.0 if the input is empty.
	*/
inline double Percentile(std::vector<double> values, double p) {
	if (values.empty()) return 0.0;
	std::sort(values.begin(), values.end());
	const double pos = p * (values.size() - 1);
	const std::size_t lo = static_cast<std::size_t>(pos);
	const std::size_t hi = std::min(lo + 1, values.size() - 1);
	const double frac = pos - lo;
	return values[lo] * (1.0 - frac) + values[hi] * frac;
}

/**
	* @brief Prefill an OrderBook with resting sell orders at increasing prices.
	*
	* Distributes @p orders across @p levels price points (1 tick apart, starting
	* at 1000) so the book has a controlled depth before the measured operation.
	*
	* @param book     Target OrderBook (modified in-place).
	* @param orders   Total number of orders to insert.
	* @param levels   Number of distinct price levels.
	* @param id_base  Starting order-ID offset.
	*/
inline void PrefillSellBook(llmes::matching_core::OrderBook& book, std::uint64_t orders,
														std::uint64_t levels, std::uint64_t id_base,
														std::vector<llmes::matching_core::OrderHandle>* handles = nullptr) {
	const std::uint64_t per_level =
			std::max<std::uint64_t>(1, orders / std::max<std::uint64_t>(1, levels));
	std::uint64_t id = id_base;
	for (std::uint64_t lvl = 0; lvl < levels; ++lvl) {
		// Starts at 1000 and increments by one tick per level.
		const std::int64_t ask_price = 1000 + static_cast<std::int64_t>(lvl);
		for (std::uint64_t j = 0; j < per_level; ++j) {
			const auto res =
					book.add_limit_order(id, llmes::matching_core::Side::Sell, ask_price, 1, id);
			if (handles != nullptr) handles->push_back(res.handle);
			++id;
		}
	}
}

/**
	* @brief Prefill an OrderBook with HFT-realistic depth decay.
	*
	* Distributes @p orders across @p levels with an exponential-like decay from
	* the best price, matching empirical limit-order-book depth profiles:
	*   best price (tick 0):   20% of orders
	*   tick  1:  18%      tick  2:  15%      tick  3:  12%
	*   tick  4:  10%      tick  5:   8%      ticks 6-10: 2.4% each
	*   ticks 11+: balance of remaining 5%
	*
	* For fewer than 12 levels the same shape is proportionally compressed.
	*
	* @param book      Target OrderBook (modified in-place).
	* @param orders    Total number of orders to insert.
	* @param levels    Number of distinct price levels.
	* @param base_price Best (tightest) price in ticks (default 1000 = $10.00).
	* @param id_base   Starting order-ID offset.
	* @param seed      Deterministic RNG seed for ID generation.
	* @param side      Side to place orders (Sell for asks, Buy for bids).
	*/
inline void PrefillHftBook(llmes::matching_core::OrderBook& book,
														std::uint64_t orders,
														std::uint64_t levels,
														std::int64_t base_price = 1000,
														std::uint64_t id_base = 100'000'000ULL,
														std::uint64_t seed = 42,
														llmes::matching_core::Side side = llmes::matching_core::Side::Sell,
														std::vector<llmes::matching_core::OrderHandle>* handles = nullptr) {
	SplitMix64 rng(seed);
	std::uint64_t id = id_base;
	std::uint64_t remaining = orders;

	// Weights for ticks 0..10 (exponential-like decay)
	static constexpr double kW[11] = {20.0, 18.0, 15.0, 12.0, 10.0,
																		8.0, 2.4, 2.4, 2.4, 2.4, 2.4};

	if (levels >= 12) {
		// Standard distribution: ticks 0-10 get their exact weight / 100,
		// ticks 11+ share the remaining 5% equally.
		for (std::uint64_t tick = 0; tick < levels && remaining > 0; ++tick) {
			const double pct = (tick < 11)
				? kW[tick] / 100.0
				: 0.05 / static_cast<double>(levels - 11);
			std::uint64_t count = static_cast<std::uint64_t>(
					pct * static_cast<double>(orders));
			if (count == 0 && remaining > 0) count = 1;
			if (count > remaining) count = remaining;

			const std::int64_t price = base_price + static_cast<std::int64_t>(tick);
			for (std::uint64_t j = 0; j < count; ++j) {
				const auto res = book.add_limit_order(id, side, price, 1, id);
				if (handles != nullptr) handles->push_back(res.handle);
				++id;
			}
			remaining -= count;
		}
	} else {
		// Fewer than 12 levels: normalize weights proportionally.
		double total = 0.0;
		for (std::uint64_t i = 0; i < levels; ++i) total += kW[i];
		for (std::uint64_t tick = 0; tick < levels && remaining > 0; ++tick) {
			const double pct = kW[tick] / total;
			std::uint64_t count = static_cast<std::uint64_t>(
					pct * static_cast<double>(orders));
			if (count == 0 && remaining > 0) count = 1;
			if (count > remaining) count = remaining;

			const std::int64_t price = base_price + static_cast<std::int64_t>(tick);
			for (std::uint64_t j = 0; j < count; ++j) {
				const auto res = book.add_limit_order(id, side, price, 1, id);
				if (handles != nullptr) handles->push_back(res.handle);
				++id;
			}
			remaining -= count;
		}
	}

	// Any leftover orders due to rounding go to the best price.
	if (remaining > 0) {
		const std::int64_t price = base_price;
		for (std::uint64_t j = 0; j < remaining; ++j) {
			const auto res = book.add_limit_order(id, side, price, 1, id);
			if (handles != nullptr) handles->push_back(res.handle);
			++id;
		}
	}
}

}  // namespace benchmark_runner
