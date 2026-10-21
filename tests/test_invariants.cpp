#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <random>
#include <exception>
#include <mutex>
#include <memory>
#include <sstream>

// ============================================================================
// SIMULATED 'titan-vm-runtime' IMPLEMENTATION
// ============================================================================
namespace TitanVM {

enum class Opcode : uint8_t {
    NOP = 0,
    PUSH,
    POP,
    ADD,
    SUB,
    MUL,
    DIV,
    LOAD,
    STORE,
    HALT
};

struct Instruction {
    Opcode op;
    int64_t arg;
};

enum class VMStatus {
    READY,
    RUNNING,
    HALTED,
    ERROR_STACK_OVERFLOW,
    ERROR_STACK_UNDERFLOW,
    ERROR_DIVISION_BY_ZERO,
    ERROR_INVALID_OPCODE,
    ERROR_OUT_OF_BOUNDS
};

class VMSnapshot {
public:
    std::vector<int64_t> stack;
    std::vector<int64_t> memory;
    size_t pc;
    VMStatus status;
};

class VM {
private:
    std::vector<int64_t> stack_;
    std::vector<int64_t> memory_;
    std::vector<Instruction> code_;
    size_t pc_;
    VMStatus status_;
    size_t max_stack_size_;
    std::recursive_mutex vm_mutex_;

    void set_status(VMStatus s) {
        status_ = s;
    }

public:
    explicit VM(size_t memory_size = 1024, size_t max_stack = 256) 
        : pc_(0), status_(VMStatus::READY), max_stack_size_(max_stack) {
        memory_.resize(memory_size, 0);
        stack_.reserve(max_stack);
    }

    void load_program(const std::vector<Instruction>& program) {
        std::lock_guard<std::recursive_mutex> lock(vm_mutex_);
        code_ = program;
        pc_ = 0;
        status_ = VMStatus::READY;
        stack_.clear();
    }

    VMStatus step() {
        std::lock_guard<std::recursive_mutex> lock(vm_mutex_);
        if (status_ != VMStatus::RUNNING && status_ != VMStatus::READY) {
            return status_;
        }
        
        status_ = VMStatus::RUNNING;

        if (pc_ >= code_.size()) {
            status_ = VMStatus::HALTED;
            return status_;
        }

        const auto& instr = code_[pc_++];

        switch (instr.op) {
            case Opcode::NOP:
                break;
            case Opcode::PUSH:
                if (stack_.size() >= max_stack_size_) {
                    status_ = VMStatus::ERROR_STACK_OVERFLOW;
                    return status_;
                }
                stack_.push_back(instr.arg);
                break;
            case Opcode::POP:
                if (stack_.empty()) {
                    status_ = VMStatus::ERROR_STACK_UNDERFLOW;
                    return status_;
                }
                stack_.pop_back();
                break;
            case Opcode::ADD: {
                if (stack_.size() < 2) {
                    status_ = VMStatus::ERROR_STACK_UNDERFLOW;
                    return status_;
                }
                int64_t b = stack_.back(); stack_.pop_back();
                int64_t a = stack_.back(); stack_.pop_back();
                stack_.push_back(a + b);
                break;
            }
            case Opcode::SUB: {
                if (stack_.size() < 2) {
                    status_ = VMStatus::ERROR_STACK_UNDERFLOW;
                    return status_;
                }
                int64_t b = stack_.back(); stack_.pop_back();
                int64_t a = stack_.back(); stack_.pop_back();
                stack_.push_back(a - b);
                break;
            }
            case Opcode::MUL: {
                if (stack_.size() < 2) {
                    status_ = VMStatus::ERROR_STACK_UNDERFLOW;
                    return status_;
                }
                int64_t b = stack_.back(); stack_.pop_back();
                int64_t a = stack_.back(); stack_.pop_back();
                stack_.push_back(a * b);
                break;
            }
            case Opcode::DIV: {
                if (stack_.size() < 2) {
                    status_ = VMStatus::ERROR_STACK_UNDERFLOW;
                    return status_;
                }
                int64_t b = stack_.back(); stack_.pop_back();
                int64_t a = stack_.back(); stack_.pop_back();
                if (b == 0) {
                    status_ = VMStatus::ERROR_DIVISION_BY_ZERO;
                    return status_;
                }
                stack_.push_back(a / b);
                break;
            }
            case Opcode::LOAD: {
                if (instr.arg < 0 || static_cast<size_t>(instr.arg) >= memory_.size()) {
                    status_ = VMStatus::ERROR_OUT_OF_BOUNDS;
                    return status_;
                }
                if (stack_.size() >= max_stack_size_) {
                    status_ = VMStatus::ERROR_STACK_OVERFLOW;
                    return status_;
                }
                stack_.push_back(memory_[instr.arg]);
                break;
            }
            case Opcode::STORE: {
                if (instr.arg < 0 || static_cast<size_t>(instr.arg) >= memory_.size()) {
                    status_ = VMStatus::ERROR_OUT_OF_BOUNDS;
                    return status_;
                }
                if (stack_.empty()) {
                    status_ = VMStatus::ERROR_STACK_UNDERFLOW;
                    return status_;
                }
                memory_[instr.arg] = stack_.back();
                stack_.pop_back();
                break;
            }
            case Opcode::HALT:
                status_ = VMStatus::HALTED;
                break;
            default:
                status_ = VMStatus::ERROR_INVALID_OPCODE;
                break;
        }

        return status_;
    }

    VMStatus run() {
        VMStatus s = status_;
        while (s == VMStatus::READY || s == VMStatus::RUNNING) {
            s = step();
        }
        return s;
    }

    VMSnapshot create_snapshot() {
        std::lock_guard<std::recursive_mutex> lock(vm_mutex_);
        VMSnapshot snap;
        snap.stack = stack_;
        snap.memory = memory_;
        snap.pc = pc_;
        snap.status = status_;
        return snap;
    }

    void restore_snapshot(const VMSnapshot& snap) {
        std::lock_guard<std::recursive_mutex> lock(vm_mutex_);
        stack_ = snap.stack;
        memory_ = snap.memory;
        pc_ = snap.pc;
        status_ = snap.status;
    }

    VMStatus get_status() const {
        // Note: const cast or mutable mutex preferred in prod, using mutable in real code.
        return status_;
    }
};

} // namespace TitanVM

// ============================================================================
// TEST SUITE FRAMEWORK & IMPLEMENTATION
// ============================================================================

class TestSuite {
private:
    int passed_ = 0;
    int failed_ = 0;
    std::mutex mtx_;

    void log_result(const std::string& name, bool success, const std::string& msg = "") {
        std::lock_guard<std::mutex> lock(mtx_);
        if (success) {
            std::cout << "[PASS] " << name << "\n";
            passed_++;
        } else {
            std::cerr << "[FAIL] " << name << " -> " << msg << "\n";
            failed_++;
        }
    }

public:
    void run_all_tests() {
        std::cout << "====================================================\n";
        std::cout << "Starting 'titan-vm-runtime' Exhaustive Test Suite\n";
        std::cout << "====================================================\n";

        // 1. Unit & Boundary Tests
        test_stack_overflow();
        test_stack_underflow();
        test_division_by_zero();
        test_memory_bounds();
        test_arithmetic_edge_cases();

        // 2. Invariant & Snapshot Tests
        test_snapshot_recovery();
        test_deterministic_execution();

        // 3. Concurrent Stress & Race Condition Tests
        test_concurrent_execution_stress();
        test_concurrent_snapshot_stress();

        std::cout << "====================================================\n";
        std::cout << "Test Complete. Passed: " << passed_ << ", Failed: " << failed_ << "\n";
        std::cout << "====================================================\n";
    }

    int get_failure_count() const {
        return failed_;
    }

private:
    void test_stack_overflow() {
        using namespace TitanVM;
        VM vm(128, 4); // Max stack size = 4
        std::vector<Instruction> program = {
            {Opcode::PUSH, 1},
            {Opcode::PUSH, 2},
            {Opcode::PUSH, 3},
            {Opcode::PUSH, 4},
            {Opcode::PUSH, 5} // Should trigger overflow
        };
        vm.load_program(program);
        VMStatus status = vm.run();
        bool success = (status == VMStatus::ERROR_STACK_OVERFLOW);
        log_result("Unit: Stack Overflow Boundary", success, "Expected STACK_OVERFLOW status");
    }

    void test_stack_underflow() {
        using namespace TitanVM;
        VM vm;
        std::vector<Instruction> program = {
            {Opcode::POP, 0} // Underflow on empty stack
        };
        vm.load_program(program);
        VMStatus status = vm.run();
        bool success = (status == VMStatus::ERROR_STACK_UNDERFLOW);
        log_result("Unit: Stack Underflow Boundary", success, "Expected STACK_UNDERFLOW status");
    }

    void test_division_by_zero() {
        using namespace TitanVM;
        VM vm;
        std::vector<Instruction> program = {
            {Opcode::PUSH, 10},
            {Opcode::PUSH, 0},
            {Opcode::DIV, 0}
        };
        vm.load_program(program);
        VMStatus status = vm.run();
        bool success = (status == VMStatus::ERROR_DIVISION_BY_ZERO);
        log_result("Unit: Division By Zero", success, "Expected ERROR_DIVISION_BY_ZERO status");
    }

    void test_memory_bounds() {
        using namespace TitanVM;
        VM vm(10, 16); // Memory size 10
        std::vector<Instruction> program = {
            {Opcode::PUSH, 42},
            {Opcode::STORE, 15} // Out of bounds write
        };
        vm.load_program(program);
        VMStatus status = vm.run();
        bool success = (status == VMStatus::ERROR_OUT_OF_BOUNDS);
        log_result("Unit: Memory Out-Of-Bounds Write", success, "Expected ERROR_OUT_OF_BOUNDS status");

        VM vm2(10, 16);
        std::vector<Instruction> program2 = {
            {Opcode::LOAD, -1} // Out of bounds read (negative)
        };
        vm2.load_program(program2);
        status = vm2.run();
        success = (status == VMStatus::ERROR_OUT_OF_BOUNDS);
        log_result("Unit: Memory Out-Of-Bounds Negative Read", success, "Expected ERROR_OUT_OF_BOUNDS status");
    }

    void test_arithmetic_edge_cases() {
        using namespace TitanVM;
        VM vm;
        // Test max int arithmetic or standard combinations
        std::vector<Instruction> program = {
            {Opcode::PUSH, 100},
            {Op