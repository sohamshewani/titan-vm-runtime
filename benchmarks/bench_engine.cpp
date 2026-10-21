#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <iomanip>
#include <cstdint>

// Forward declarations / mock structures representing 'titan-vm-runtime' components.
// In a real integration, these would be included from <titan/vm/runtime.hpp>
namespace titan {
    namespace vm {
        struct RuntimeContext {
            uint64_t reg[8] = {0};
            uint64_t pc = 0;
            bool halted = false;
        };

        class Runtime {
        public:
            Runtime() = default;
            
            // Simulates a lightweight VM instruction dispatch or execution step
            inline void execute_step(RuntimeContext& ctx) {
                // Dummy workload representing VM bytecode execution overhead
                ctx.reg[0] += 1;
                ctx.reg[1] ^= ctx.reg[0];
                ctx.pc += 4;
                if (ctx.pc >= 4096) {
                    ctx.pc = 0;
                }
            }
        };
    }
}

int main() {
    // Configuration
    const size_t WARMUP_ITERATIONS = 100'000;
    const size_t BENCHMARK_ITERATIONS = 2'000'000;

    titan::vm::Runtime vm;
    titan::vm::RuntimeContext ctx;

    // Allocate storage for latency measurements (nanoseconds)
    std::vector<uint64_t> latencies;
    latencies.reserve(BENCHMARK_ITERATIONS);

    // --- Warmup Phase ---
    for (size_t i = 0; i < WARMUP_ITERATIONS; ++i) {
        vm.execute_step(ctx);
    }

    // --- Benchmark Phase ---
    auto global_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < BENCHMARK_ITERATIONS; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        
        vm.execute_step(ctx);
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        latencies.push_back(duration);
    }

    auto global_end = std::chrono::high_resolution_clock::now();
    auto total_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(global_end - global_start).count();

    // --- Metrics Calculation ---
    // Sort latencies to compute percentiles
    std::sort(latencies.begin(), latencies.end());

    uint64_t p50 = latencies[BENCHMARK_ITERATIONS * 50 / 100];
    uint64_t p90 = latencies[BENCHMARK_ITERATIONS * 90 / 100];
    uint64_t p99 = latencies[BENCHMARK_ITERATIONS * 99 / 100];
    uint64_t p999 = latencies[static_cast<size_t>(BENCHMARK_ITERATIONS * 99.9 / 100)];
    
    double total_seconds = static_cast<double>(total_duration_ns) / 1e9;
    double ops_per_sec = static_cast<double>(BENCHMARK_ITERATIONS) / total_seconds;
    double avg_latency_ns = static_cast<double>(total_duration_ns) / static_cast<double>(BENCHMARK_ITERATIONS);

    // --- Formatted Tabular Output ---
    std::cout << "\n";
    std::cout << "===================================================================\n";
    std::cout << "             TITAN-VM-RUNTIME MICROBENCHMARK RESULTS               \n";
    std::cout << "===================================================================\n";
    std::cout << "| Metric                  | Value                  | Unit         |\n";
    std::cout << "+-------------------------+------------------------+--------------+\n";
    std::cout << "| Total Iterations        | " << std::setw(22) << BENCHMARK_ITERATIONS << " | ops          |\n";
    std::cout << "| Total Execution Time    | " << std::setw(22) << std::fixed << std::setprecision(4) << total_seconds << " | s            |\n";
    std::cout << "| Throughput (Ops/Sec)    | " << std::setw(22) << std::fixed << std::setprecision(2) << ops_per_sec << " | ops/sec      |\n";
    std::cout << "| Average Latency         | " << std::setw(22) << std::fixed << std::setprecision(2) << avg_latency_ns << " | ns           |\n";
    std::cout << "| p50 Latency             | " << std::setw(22) << p50 << " | ns           |\n";
    std::cout << "| p90 Latency             | " << std::setw(22) << p90 << " | ns           |\n";
    std::cout << "| p99 Latency             | " << std::setw(22) << p99 << " | ns           |\n";
    std::cout << "| p99.9 Latency           | " << std::setw(22) << p999 << " | ns           |\n";
    std::cout << "===================================================================\n";
    std::cout << "\n";

    return 0;
}