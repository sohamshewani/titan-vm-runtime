#include "engine.hpp"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <new>

namespace titan {

VMExecutionEngine::VMExecutionEngine()
    : ip_(0), running_(false) {
    stack_.fill(0);
}

VMExecutionEngine::~VMExecutionEngine() {
    shutdown();
}

void VMExecutionEngine::initialize() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    stack_pointer_ = stack_.data();
    frame_pointer_ = stack_.data();
    ip_ = 0;
    running_ = true;
    
    // Initialize root registry with global/stack roots if needed
    RootRegistry::instance().register_thread_stack(stack_.data(), stack_pointer_);
}

void VMExecutionEngine::shutdown() {
    std::lock_guard<std::mutex> lock(engine_mutex_);
    running_ = false;
    RootRegistry::instance().unregister_thread_stack(stack_.data());
}

void VMExecutionEngine::poll_safepoint() {
    // Check if GC has requested a stop-the-world safepoint
    if (GCController::instance().is_safepoint_requested()) {
        GCController::instance().reach_safepoint(stack_pointer_);
    }
}

void VMExecutionEngine::execute(const std::vector<uint32_t>& bytecode) {
    if (bytecode.empty()) {
        return;
    }

    running_ = true;
    const uint32_t* code_start = bytecode.data();
    size_t code_size = bytecode.size();

    while (running_) {
        poll_safepoint();

        if (ip_ >= code_size) {
            throw VMException("Instruction pointer out of bounds");
        }

        uint32_t raw_instr = code_start[ip_++];
        Opcode op = static_cast<Opcode>(raw_instr & 0xFF);

        switch (op) {
            case Opcode::NOP: {
                break;
            }
            case Opcode::LOAD_CONST: {
                uint8_t reg = (raw_instr >> 8) & 0xFF;
                uint16_t const_idx = (raw_instr >> 16) & 0xFFFF;
                // For demonstration, load a dummy integer NaN-boxed value
                stack_pointer_[reg] = Value::encode_int(static_cast<int64_t>(const_idx));
                break;
            }
            case Opcode::MOVE: {
                uint8_t dest = (raw_instr >> 8) & 0xFF;
                uint8_t src = (raw_instr >> 16) & 0xFF;
                stack_pointer_[dest] = stack_pointer_[src];
                break;
            }
            case Opcode::ADD: {
                uint8_t dest = (raw_instr >> 8) & 0xFF;
                uint8_t left = (raw_instr >> 16) & 0xFF;
                uint8_t right = (raw_instr >> 24) & 0xFF;

                uint64_t l_val = stack_pointer_[left];
                uint64_t r_val = stack_pointer_[right];

                if (Value::is_int(l_val) && Value::is_int(r_val)) {
                    int64_t res = Value::decode_int(l_val) + Value::decode_int(r_val);
                    stack_pointer_[dest] = Value::encode_int(res);
                } else if (Value::is_double(l_val) && Value::is_double(r_val)) {
                    double res = Value::decode_double(l_val) + Value::decode_double(r_val);
                    stack_pointer_[dest] = Value::encode_double(res);
                } else {
                    throw VMException("TypeError: invalid operands for ADD");
                }
                break;
            }
            case Opcode::SUB: {
                uint8_t dest = (raw_instr >> 8) & 0xFF;
                uint8_t left = (raw_instr >> 16) & 0xFF;
                uint8_t right = (raw_instr >> 24) & 0xFF;

                uint64_t l_val = stack_pointer_[left];
                uint64_t r_val = stack_pointer_[right];

                if (Value::is_int(l_val) && Value::is_int(r_val)) {
                    int64_t res = Value::decode_int(l_val) - Value::decode_int(r_val);
                    stack_pointer_[dest] = Value::encode_int(res);
                } else if (Value::is_double(l_val) && Value::is_double(r_val)) {
                    double res = Value::decode_double(l_val) - Value::decode_double(r_val);
                    stack_pointer_[dest] = Value::encode_double(res);
                } else {
                    throw VMException("TypeError: invalid operands for SUB");
                }
                break;
            }
            case Opcode::CALL: {
                // Function invocation logic
                break;
            }
            case Opcode::RET: {
                running_ = false;
                break;
            }
            case Opcode::HALT: {
                running_ = false;
                break;
            }
            default: {
                dump_state();
                throw VMException("Fault: Unknown or illegal opcode encountered");
            }
        }
    }
}

void VMExecutionEngine::dump_state() const {
    std::cerr << "=== TITAN-VM FATAL FAULT DUMP ===" << std::endl;
    std::cerr << "Instruction Pointer (IP): " << ip_ << std::endl;
    std::cerr << "Frame Pointer (FP): " << (void*)frame_pointer_ << std::endl;
    std::cerr << "Stack Pointer (SP): " << (void*)stack_pointer_ << std::endl;
    std::cerr << "Register State (First 16 registers):" << std::endl;
    for (int i = 0; i < 16; ++i) {
        std::fprintf(stderr, "  R[%02d]: 0x%016llX\n", i, (unsigned long long)stack_pointer_[i]);
    }
    std::cerr << "=================================" << std::endl;
}

// ArenaBlock Implementation
ArenaBlock::ArenaBlock(size_t capacity)
    : capacity_(capacity), offset_(0) {
    memory_ = static_cast<char*>(std::malloc(capacity_));
    if (!memory_) {
        throw std::bad_alloc();
    }
}

ArenaBlock::~ArenaBlock() {
    if (memory_) {
        std::free(memory_);
    }
}

void* ArenaBlock::allocate(size_t size, size_t alignment) {
    uintptr_t current_address = reinterpret_cast<uintptr_t>(memory_ + offset_);
    size_t adjustment = (alignment - (current_address % alignment)) % alignment;
    size_t needed = size + adjustment;

    if (offset_ + needed > capacity_) {
        return nullptr;
    }

    offset_ += adjustment;
    void* ptr = static_cast<void*>(memory_ + offset_);
    offset_ += size;
    return ptr;
}

void ArenaBlock::reset() {
    offset_ = 0;
}

// ArenaAllocator Implementation
ArenaAllocator::ArenaAllocator(size_t block_size)
    : block_size_(block_size) {
    blocks_.push_back(std::make_unique<ArenaBlock>(block_size_));
}

ArenaAllocator::~ArenaAllocator() = default;

void* ArenaAllocator::allocate(size_t size, size_t alignment) {
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    
    void* ptr = blocks_.back()->allocate(size, alignment);
    if (!ptr) {
        // Allocate a new block, double size if necessary or use default block_size
        size_t new_capacity = std::max(block_size_, size + alignment);
        blocks_.push_back(std::make_unique<ArenaBlock>(new_capacity));
        ptr = blocks_.back()->allocate(size, alignment);
        if (!ptr) {
            throw std::bad_alloc();
        }
    }
    return ptr;
}

void ArenaAllocator::reset() {
    std::lock_guard<std::mutex> lock(allocator_mutex_);
    for (auto& block : blocks_) {
        block->reset();
    }
    if (!blocks_.empty()) {
        // Keep the first block, clear the rest to save memory
        blocks_.erase(blocks_.begin() + 1, blocks_.end());
    }
}

// RootRegistry Implementation
RootRegistry& RootRegistry::instance() {
    static RootRegistry instance_ref;
    return instance_ref;
}

void RootRegistry::register_thread_stack(uint64_t* stack_base, uint64_t*& stack_ptr) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    thread_stacks_[std::this_thread::get_id()] = std::make_pair(stack_base, &stack_ptr);
}

void RootRegistry::unregister_thread_stack(uint64_t* stack_base) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    thread_stacks_.erase(std::this_thread::get_id());
}

void RootRegistry::register_global(HeapObject** global_ptr) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    globals_.push_back(global_ptr);
}

void RootRegistry::visit_roots(const std::function<void(HeapObject**)>& visitor) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    
    // Visit global roots
    for (auto* g : globals_) {
        visitor(g);
    }

    // Visit thread stack roots
    for (auto& pair : thread_stacks_) {
        uint64_t* base = pair.second.first;
        uint64_t* current = *pair.second.second;
        for (uint64_t* ptr = base; ptr <= current; ++ptr) {
            if (Value::is_object(*ptr)) {
                // Convert NaN-boxed representation pointer back to HeapObject*
                // In a strict implementation, we pass the address of the stack slot
                // if the GC updates values in place. Here we pass reinterpret_cast<HeapObject**>(ptr).
                visitor(reinterpret_cast<HeapObject**>(ptr));
            }
        }
    }
}

// GCHeap Implementation
GCHeap::GCHeap()
    : young_allocated_bytes_(0), tenured_allocated_bytes_(0) {}

GCHeap::~GCHeap() {
    std::lock_guard<std::mutex> lock(heap_mutex_);
    young_space_.clear();
    tenured_space_.clear();
}

void* GCHeap::allocate(size_t size, bool is_tenured) {
    std::lock_guard<std::mutex> lock(heap_mutex_);
    
    // Align size to 8 bytes
    size_t aligned_size = (size + 7) & ~static_cast<size_t>(7);
    
    void* memory = std::malloc(aligned_size);
    if (!memory) {
        throw std::bad_alloc();
    }

    HeapObject* obj = static_cast<HeapObject*>(memory);
    obj->gc_header = 0; // Initialize color (White)

    if (is_tenured) {
        tenured_space_.push_back(obj);
        tenured_allocated_bytes_ += aligned_size;
    } else {
        young_space_.push_back(obj);
        young_allocated_bytes_ += aligned_size;
    }

    return memory;
}

void GCHeap::write_barrier(HeapObject* source, HeapObject* slot) {
    // Dijkstra write barrier: if source is black and slot is white, make slot gray.
    // In our simplified tri-color abstraction:
    if (source && slot) {
        // If source is marked (black/gray) and slot is unmarked (white), push slot to GC worklist
        GCController::instance().mark_object(slot);
    }
}

void GCHeap::collect_garbage() {
    std::lock_guard<std::mutex> lock(heap_mutex_);

    // 1. Mark phase: trace roots
    RootRegistry::instance().visit_roots([this](HeapObject** root) {
        if (*root) {
            GCController::instance().mark_object(*root);
        }
    });

    // 2. Sweep phase for young space
    auto sweep_space = [](std::vector<HeapObject*>& space, size_t& allocated_bytes) {
        auto it = space.begin();
        while (it != space.end()) {
            HeapObject* obj = *it;
            if (!obj->is_marked()) {
                std::free(obj);
                it = space.erase(it);
            } else {
                obj->clear_mark(); // Reset for next cycle
                ++it;
            }
        }
    };

    sweep_space(young_space_, young_allocated_bytes_);
    sweep_space(tenured_space_, tenured_allocated_bytes_);
}

// GCController Implementation
GCController& GCController::instance() {
    static GCController instance_ref;
    return instance_ref;
}

GCController::GCController()
    : phase_(GCPhase::Idle), safepoint_requested_(false), worker_running_(false) {}

GCController::~GCController() {
    stop_worker();
}

void GCController::start_worker() {
    worker_running_ = true;
    gc_thread_ = std::thread(&GCController::gc_worker_loop, this);
}

void GCController::stop_worker() {
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_running_ = false;
    }
    cv_.notify_all();
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }
}

bool GCController::is_safepoint_requested() const {
    return safepoint_requested_.load(std::memory_order_acquire);
}

void GCController::reach_safepoint(uint64_t* stack_ptr) {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    safepoint_count_++;
    cv_.notify_all();
    // Wait until GC finishes the STW phase
    cv_.wait(lock, [this] { return !safepoint_requested_.load(std::memory_order_acquire); });
}

void GCController::mark_object(HeapObject* obj) {
    if (obj && !obj->is_marked()) {
        obj->set_marked();
        // In a full implementation, push to gray worklist
    }
}

void GCController::gc_worker_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return !worker_running_ || heap_.requires_collection();
            });

            if (!worker_running_ && !heap_.requires_collection()) {
                break;
            }
        }

        // Trigger STW
        phase_.store(GCPhase::Marking, std::memory_order_release);
        safepoint_requested_.store(true, std::memory_order_release);

        // Wait for all threads to reach safepoint
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            // In a multi-worker scenario, wait for thread count. Here we proceed.
        }

        // Perform collection
        phase_.store(GCPhase::Sweeping, std::memory_order_release);
        heap_.collect_garbage();

        // Release STW
        safepoint_requested_.store(false, std::memory_order_release);
        phase_.store(GCPhase::Idle, std::memory_order_release);
        cv_.notify_all();
    }
}

} // namespace titan