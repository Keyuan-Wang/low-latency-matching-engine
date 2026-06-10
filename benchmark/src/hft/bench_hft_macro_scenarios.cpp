/**
 * @file bench_hft_macro_scenarios.cpp
 * @brief Diagnostic per-scenario latency collector for hft_macro.
 *
 * This tool keeps the real hft_macro state evolution, but only records cycle
 * samples for the three single-operation buckets we want to reason about:
 * add_rest, cancel_order, and modify_order. Matching-heavy operations are
 * still replayed so book state stays realistic, but they are reported only as
 * unmeasured workload composition.
 */

#include "bench_common.hpp"
#include "benchmark_runner.hpp"
#include "hft_macro_workload.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#else
#error "bench_hft_macro_scenarios requires x86_64 TSC support"
#endif

namespace {

using benchmark_runner::hft::MacroScenario;
using Clock = std::chrono::steady_clock;

struct ScenarioArgs {
	benchmark_runner::Args base{};
	std::string focus = "all";
};

struct MeasurementOverhead {
	std::uint64_t cycles = 0;
	std::uint64_t elapsed_ns = 0;
};

struct ScenarioSample {
	std::uint64_t measurement_iter = 0;
	std::uint64_t replay_iter_idx = 0;
	std::uint64_t op_index = 0;
	std::uint64_t scenario_call_index = 0;
	std::uint64_t raw_cycles = 0;
	std::uint64_t cycles = 0;
	std::uint64_t raw_elapsed_ns = 0;
	std::uint64_t elapsed_ns = 0;
};

struct RowStats {
	std::uint64_t count = 0;
	double share = 0.0;
	bool measured = false;
	double avg_cycles = 0.0;
	double p50_cycles = 0.0;
	double p95_cycles = 0.0;
	double p99_cycles = 0.0;
	double p999_cycles = 0.0;
	double min_cycles = 0.0;
	double max_cycles = 0.0;
};

struct CampaignStats {
	std::array<std::uint64_t, benchmark_runner::hft::kMacroScenarioCount> counts{};
	std::array<std::vector<ScenarioSample>, benchmark_runner::hft::kMacroScenarioCount>
			samples{};
	std::uint64_t ok = 0;
};

[[nodiscard]] std::uint64_t ReadCycleStart() noexcept {
	_mm_lfence();
	return __rdtsc();
}

[[nodiscard]] std::uint64_t ReadCycleStop() noexcept {
	unsigned aux = 0;
	const std::uint64_t t = __rdtscp(&aux);
	_mm_lfence();
	return t;
}

[[nodiscard]] MeasurementOverhead MeasureTimingOverhead() {
	constexpr int kSamples = 10000;
	std::vector<std::uint64_t> cycle_samples;
	std::vector<std::uint64_t> elapsed_samples;
	cycle_samples.reserve(kSamples);
	elapsed_samples.reserve(kSamples);
	for (int i = 0; i < kSamples; ++i) {
		const auto ns0 = Clock::now();
		const std::uint64_t t0 = ReadCycleStart();
		const std::uint64_t t1 = ReadCycleStop();
		const auto ns1 = Clock::now();
		cycle_samples.push_back(t1 - t0);
		elapsed_samples.push_back(
				static_cast<std::uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(ns1 - ns0)
								.count()));
	}
	return MeasurementOverhead{
			*std::min_element(cycle_samples.begin(), cycle_samples.end()),
			*std::min_element(elapsed_samples.begin(), elapsed_samples.end())};
}

void ParseArgs(int argc, char** argv, ScenarioArgs& args) {
	args.base.orders = 100000;
	args.base.levels = 100;
	args.base.batch_size = 100000;
	args.base.warmup_iters = 1;
	args.base.iters = 1;

	for (int i = 1; i < argc; ++i) {
		std::string s = argv[i];
		auto next_u64 = [&](std::uint64_t& v) {
			if (i + 1 >= argc) {
				std::cerr << "missing value for " << s << "\n";
				std::exit(2);
			}
			v = std::stoull(argv[++i]);
		};
		auto next_str = [&]() -> std::string {
			if (i + 1 >= argc) {
				std::cerr << "missing value for " << s << "\n";
				std::exit(2);
			}
			return argv[++i];
		};

		if (s == "--metric") {
			(void)next_str();  // accepted for script compatibility, ignored here
		} else if (s == "--orders") {
			next_u64(args.base.orders);
		} else if (s == "--levels") {
			next_u64(args.base.levels);
		} else if (s == "--batch-size") {
			next_u64(args.base.batch_size);
		} else if (s == "--warmup-iters") {
			next_u64(args.base.warmup_iters);
		} else if (s == "--iters") {
			next_u64(args.base.iters);
		} else if (s == "--trial-id") {
			next_u64(args.base.trial_id);
		} else if (s == "--seed") {
			next_u64(args.base.seed);
		} else if (s == "--version-tag") {
			args.base.version_tag = next_str();
		} else if (s == "--commit-sha") {
			args.base.commit_sha = next_str();
		} else if (s == "--out") {
			args.base.out_csv = next_str();
		} else if (s == "--focus") {
			args.focus = next_str();
		}
	}
}

[[nodiscard]] MacroScenario ParseFocusOne(const std::string& focus) {
	if (focus == "add_rest" || focus == "add") return MacroScenario::AddRest;
	if (focus == "cancel_order" || focus == "cancel") {
		return MacroScenario::CancelOrder;
	}
	if (focus == "modify_order" || focus == "modify") {
		return MacroScenario::ModifyOrder;
	}
	std::cerr << "unknown --focus: " << focus << "\n";
	std::exit(2);
}

[[nodiscard]] std::vector<MacroScenario> FocusList(const std::string& focus) {
	if (focus == "all") {
		return {
				MacroScenario::AddRest,
				MacroScenario::CancelOrder,
				MacroScenario::ModifyOrder,
		};
	}
	return {ParseFocusOne(focus)};
}

void RunUntimedWarmup(const benchmark_runner::Args& args,
											std::uint64_t iter_idx) {
	benchmark_runner::hft::HftMacroWorkload workload;
	workload.Setup(args, iter_idx);
	std::uint64_t ok = 0;
	for (std::size_t i = 0; i < workload.size(); ++i) {
		(void)workload.Execute(i, ok);
	}
	workload.Teardown();
}

void RunMeasuredPass(const benchmark_runner::Args& args,
										 std::uint64_t measurement_iter,
										 std::uint64_t replay_iter_idx,
										 MacroScenario focus,
										 bool record_composition,
										 MeasurementOverhead timing_overhead,
										 CampaignStats& stats) {
	benchmark_runner::hft::HftMacroWorkload workload;
	workload.Setup(args, replay_iter_idx);

	std::uint64_t ok = 0;
	for (std::size_t i = 0; i < workload.size(); ++i) {
		const MacroScenario scenario = workload.scenario(i);
		if (record_composition) {
			++stats.counts[benchmark_runner::hft::ScenarioIndex(scenario)];
		}

		if (scenario == focus) {
			const auto ns0 = Clock::now();
			const std::uint64_t t0 = ReadCycleStart();
			(void)workload.Execute(i, ok);
			const std::uint64_t t1 = ReadCycleStop();
			const auto ns1 = Clock::now();
			const std::uint64_t raw = t1 - t0;
			const std::uint64_t adjusted =
					(raw > timing_overhead.cycles) ? (raw - timing_overhead.cycles) : 0;
			const std::uint64_t raw_elapsed =
					static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(ns1 - ns0)
									.count());
			const std::uint64_t adjusted_elapsed =
					(raw_elapsed > timing_overhead.elapsed_ns)
							? (raw_elapsed - timing_overhead.elapsed_ns)
							: 0;
			auto& samples =
					stats.samples[benchmark_runner::hft::ScenarioIndex(scenario)];
			samples.push_back(ScenarioSample{
					measurement_iter,
					replay_iter_idx,
					static_cast<std::uint64_t>(i),
					static_cast<std::uint64_t>(samples.size()),
					raw,
					adjusted,
					raw_elapsed,
					adjusted_elapsed});
		} else {
			(void)workload.Execute(i, ok);
		}
	}

	if (record_composition) stats.ok += ok;
	workload.Teardown();
}

[[nodiscard]] RowStats BuildRowStats(
		MacroScenario scenario,
		const CampaignStats& stats,
		std::uint64_t total_ops) {
	const auto idx = benchmark_runner::hft::ScenarioIndex(scenario);
	RowStats row;
	row.count = stats.counts[idx];
	row.share = total_ops > 0
									? static_cast<double>(row.count) /
												static_cast<double>(total_ops)
									: 0.0;
	const auto& samples = stats.samples[idx];
	row.measured = !samples.empty();
	if (!row.measured) return row;

	std::vector<double> cycle_values;
	cycle_values.reserve(samples.size());
	for (const auto& sample : samples) {
		cycle_values.push_back(static_cast<double>(sample.cycles));
	}

	row.avg_cycles =
			std::accumulate(cycle_values.begin(), cycle_values.end(), 0.0) /
			static_cast<double>(cycle_values.size());
	row.p50_cycles = benchmark_runner::Percentile(cycle_values, 0.50);
	row.p95_cycles = benchmark_runner::Percentile(cycle_values, 0.95);
	row.p99_cycles = benchmark_runner::Percentile(cycle_values, 0.99);
	row.p999_cycles = benchmark_runner::Percentile(cycle_values, 0.999);
	auto [min_it, max_it] =
			std::minmax_element(cycle_values.begin(), cycle_values.end());
	row.min_cycles = *min_it;
	row.max_cycles = *max_it;
	return row;
}

void PrintRow(const ScenarioArgs& args,
							MacroScenario scenario,
							const RowStats& row,
							MeasurementOverhead timing_overhead,
							std::uint64_t ok) {
	std::cout << "mode=scenario_cycles_summary"
						<< " scenario=hft_macro"
						<< " op_type="
						<< benchmark_runner::hft::ScenarioName(scenario)
						<< " version_tag=" << args.base.version_tag
						<< " commit_sha=" << args.base.commit_sha
						<< " trial_id=" << args.base.trial_id
						<< " orders=" << args.base.orders
						<< " levels=" << args.base.levels
						<< " batch_size=" << args.base.batch_size
						<< " warmup_iters=" << args.base.warmup_iters
						<< " iters=" << args.base.iters
						<< " seed=" << args.base.seed
						<< " count=" << row.count
						<< " share=" << row.share
						<< " measured=" << (row.measured ? 1 : 0)
						<< " avg_cycles=" << row.avg_cycles
						<< " p50_cycles=" << row.p50_cycles
						<< " p95_cycles=" << row.p95_cycles
						<< " p99_cycles=" << row.p99_cycles
						<< " p999_cycles=" << row.p999_cycles
						<< " min_cycles=" << row.min_cycles
						<< " max_cycles=" << row.max_cycles
						<< " timing_overhead_cycles=" << timing_overhead.cycles
						<< " elapsed_overhead_ns=" << timing_overhead.elapsed_ns
						<< " ok=" << ok << "\n";
}

void WriteCsvSamples(const ScenarioArgs& args,
										 const CampaignStats& stats,
										 MeasurementOverhead timing_overhead) {
	if (args.base.out_csv.empty()) return;

	benchmark_runner::EnsureCsvHeader(
			args.base.out_csv,
			"mode,scenario,op_type,version_tag,commit_sha,trial_id,orders,levels,"
			"batch_size,warmup_iters,iters,seed,measurement_iter,replay_iter_idx,"
			"op_index,scenario_call_index,raw_cycles,cycles,timing_overhead_cycles,"
			"raw_elapsed_ns,elapsed_ns,elapsed_overhead_ns");

	std::ofstream f(args.base.out_csv, std::ios::app);
	const std::array<MacroScenario, 3> measured_rows = {
			MacroScenario::AddRest,
			MacroScenario::CancelOrder,
			MacroScenario::ModifyOrder,
	};

	struct CsvSampleRef {
		MacroScenario scenario = MacroScenario::Unmeasured;
		const ScenarioSample* sample = nullptr;
	};
	std::vector<CsvSampleRef> rows;
	for (MacroScenario scenario : measured_rows) {
		const auto& samples =
				stats.samples[benchmark_runner::hft::ScenarioIndex(scenario)];
		for (const auto& sample : samples) {
			rows.push_back(CsvSampleRef{scenario, &sample});
		}
	}

	std::sort(rows.begin(), rows.end(),
						[](const CsvSampleRef& lhs, const CsvSampleRef& rhs) {
							if (lhs.sample->measurement_iter != rhs.sample->measurement_iter) {
								return lhs.sample->measurement_iter <
											 rhs.sample->measurement_iter;
							}
							if (lhs.sample->op_index != rhs.sample->op_index) {
								return lhs.sample->op_index < rhs.sample->op_index;
							}
							return benchmark_runner::hft::ScenarioIndex(lhs.scenario) <
										 benchmark_runner::hft::ScenarioIndex(rhs.scenario);
						});

	for (const auto& row : rows) {
		const auto& sample = *row.sample;
			f << "scenario_call,"
				<< "hft_macro,"
				<< benchmark_runner::hft::ScenarioName(row.scenario)
				<< ","
				<< args.base.version_tag
				<< ","
				<< args.base.commit_sha
				<< ","
				<< args.base.trial_id
				<< ","
				<< args.base.orders
				<< ","
				<< args.base.levels
				<< ","
				<< args.base.batch_size
				<< ","
				<< args.base.warmup_iters
				<< ","
				<< args.base.iters
				<< ","
				<< args.base.seed
				<< ","
				<< sample.measurement_iter
				<< ","
				<< sample.replay_iter_idx
				<< ","
				<< sample.op_index
				<< ","
				<< sample.scenario_call_index
				<< ","
				<< sample.raw_cycles
				<< ","
				<< sample.cycles
				<< ","
				<< timing_overhead.cycles
				<< ","
				<< sample.raw_elapsed_ns
				<< ","
				<< sample.elapsed_ns
				<< ","
				<< timing_overhead.elapsed_ns
				<< "\n";
	}
}

}  // namespace

int main(int argc, char** argv) {
	ScenarioArgs args;
	ParseArgs(argc, argv, args);

	const auto foci = FocusList(args.focus);
	const MeasurementOverhead timing_overhead = MeasureTimingOverhead();

	std::uint64_t iter_counter = 0;
	for (std::uint64_t i = 0; i < args.base.warmup_iters; ++i) {
		RunUntimedWarmup(args.base, iter_counter);
		++iter_counter;
	}

	CampaignStats stats;
	for (std::uint64_t i = 0; i < args.base.iters; ++i) {
		for (std::size_t f = 0; f < foci.size(); ++f) {
			RunMeasuredPass(args.base, i, iter_counter, foci[f], f == 0,
											timing_overhead, stats);
		}
		++iter_counter;
	}

	const std::uint64_t total_ops =
			std::accumulate(stats.counts.begin(), stats.counts.end(),
											std::uint64_t{0});
	const std::array<MacroScenario, benchmark_runner::hft::kMacroScenarioCount>
			rows = {
					MacroScenario::AddRest,
					MacroScenario::CancelOrder,
					MacroScenario::ModifyOrder,
					MacroScenario::Unmeasured,
			};

	for (MacroScenario scenario : rows) {
		const RowStats row = BuildRowStats(scenario, stats, total_ops);
		PrintRow(args, scenario, row, timing_overhead, stats.ok);
	}
	WriteCsvSamples(args, stats, timing_overhead);

	return 0;
}
