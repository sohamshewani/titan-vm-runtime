#ifndef TITAN_VM_HPP
#define TITAN_VM_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <new>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <string>
#include <stdexcept>
#include <type_traits>

namespace titan {

// Forward declarations
class HeapObject;
class VM;
class GCHeap;
template <typename T> class GCPtr;

// ============================================================================
// 1. DATA STRUCTURES & VALUE REPRESENTATION (NaN-Boxing)
// ============================================================================

enum class ValueTag : uint64_t {
    TAG_QUIET_NAN = 0x7FF8000000000000ULL,
    TAG_MASK      = 0xFFFF800000000000ULL,
    
    // Sub-tags within NaN space (upper 16 bits active, bits 48-51)
    TAG_NIL       = 0x7FF8000000000001ULL,
    TAG_FALSE     = 0x7FF8000000000002ULL,
    TAG_TRUE      = 0x7FF8000000000003ULL,
    TAG_INT64     = 0x7FF8000100000000ULL,
    TAG_OBJECT    = 0x7FF0000000000000ULL
};

class Value {
private:
    uint64_t bits_;

    constexpr explicit Value(uint64_t b) : bits_(b) {}

public:
    constexpr Value() : bits_(static_cast<uint64_t>(ValueTag::TAG_NIL)) {}

    static constexpr Value nil() { return Value(static_cast<uint64_t>(ValueTag::TAG_NIL)); }
    static constexpr Value boolean(bool b) {
        return Value(b ? static_cast<uint64_t>(ValueTag::TAG_TRUE) : static_cast<uint64_t>(ValueTag::TAG_FALSE));
    }
    static constexpr Value integer(int64_t i) {
        return Value(static_cast<uint64_t>(ValueTag::TAG_INT64) | (static_cast<uint64_t>(i) & 0xFFFFFFFFULL));
    }
    static constexpr Value double_val(double d) {
        uint64_t u;
        std::memcpy(&u, &d, sizeof(u));
        return Value(u);
    }
    static constexpr Value object(HeapObject* ptr) {
        return Value(static_cast<uint64_t>(ValueTag::TAG_OBJECT) | (reinterpret_cast<uint64_t>(ptr) & 0x0000FFFFFFFFFFFFULL));
    }

    constexpr bool is_nil() const { return bits_ == static_cast<uint64_t>(ValueTag::TAG_NIL); }
    constexpr bool is_boolean() const { 
        return bits_ == static_cast<uint64_t>(ValueTag::TAG_TRUE) || bits_ == static_cast<uint64_t>(ValueTag::TAG_FALSE); 
    }
    constexpr bool is_int64() const { return (bits_ & 0xFFFF000000000000ULL) == 0x7FF8000100000000ULL; }
    constexpr bool is_double() const { return (bits_ & static_cast<uint64_t>(ValueTag::TAG_QUIET_NAN)) != static_cast<uint64_t>(ValueTag::TAG_QUIET_NAN); }
    constexpr bool is_object() const { return (bits_ & 0xFFFF000000000000ULL) == static_cast<uint64_t>(ValueTag::TAG_OBJECT); }

    constexpr bool as_boolean() const { return bits_ == static_cast<uint64_t>(ValueTag::TAG_TRUE); }
    constexpr int64_t as_int64() const { return static_cast<int64_t>(bits_ & 0xFFFFFFFFULL); }
    constexpr double as_double() const {
        double d;
        std::memcpy(&d, &bits_, sizeof(d));
        return d;
    }
    constexpr HeapObject* as_object() const {
        return reinterpret_cast<HeapObject*>(bits_ & 0x0000FFFFFFFFFFFFULL);
    }

    constexpr uint64_t raw() const { return bits_; }
};

// ============================================================================
// 2. ARENA ALLOCATOR & AST MEMORY LAYOUT
// ============================================================================

struct alignas(64) ArenaBlock {
    size_t capacity;
    size_t used;
    ArenaBlock* next;
    char data[];

    static ArenaBlock* create(size_t cap) {
        void* mem = std::aligned_alloc(64, sizeof(ArenaBlock) + cap);
        if (!mem) throw std::bad_alloc();
        ArenaBlock* block = new (mem) ArenaBlock{cap, 0, nullptr};
        return block;
    }

    void destroy() {
        this->~ArenaBlock();
        std::free(this);
    }
};

class ArenaAllocator {
private:
    ArenaBlock* head_;
    size_t block_size_;
    std::mutex mutex_;

public:
    explicit ArenaAllocator(size_t block_size = 65536) 
        : block_size_(block_size) {
        head_ = ArenaBlock::create(block_size_);
    }

    ~ArenaAllocator() {
        ArenaBlock* curr = head_;
        while (curr) {
            ArenaBlock* next = curr->next;
            curr->destroy();
            curr = next;
        }
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* allocate(size_t size, size_t alignment = 8) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Align current used pointer
        uintptr_t current_address = reinterpret_cast<uintptr_t>(head_->data) + head_->used;
        size_t padding = (alignment - (current_address % alignment)) % alignment;
        size_t total_needed = size + padding;

        if (head_->used + total_needed > head_->capacity) {
            size_t new_cap = std::max(block_size_, total_needed + 64);
            ArenaBlock* new_block = ArenaBlock::create(new_cap);
            new_block->next = head_;
            head_ = new_block;
            current_address = reinterpret_cast<uintptr_t>(head_->data);
            padding = (alignment - (current_address % alignment)) % alignment;
            total_needed = size + padding;
        }

        void* ptr = reinterpret_cast<void*>(current_address + padding);
        head_->used += total_needed;
        return ptr;
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }
};

// ============================================================================
// 3. CONCURRENCY, GC PHASES & SAFEPOINTS
// ============================================================================

enum class GCPhase : uint8_t {
    Off = 0,
    ConcurrentMark,
    Remark,
    ConcurrentSweep
};

struct VMThreadContext {
    std::atomic<bool> safepoint_requested{false};
    std::atomic<bool> thread_suspended{false};

    void poll_safepoint() {
        if (safepoint_requested.load(std::memory_order_relaxed)) [[unlikely]] {
            handle_safepoint();
        }
    }

    void handle_safepoint();
};

// ============================================================================
// 4. GARBAGE COLLECTION & MEMORY MANAGEMENT
// ============================================================================

enum class Color : uint8_t {
    White = 0,
    Gray  = 1,
    Black = 2
};

class alignas(64) HeapObject {
private:
    std::atomic<Color> color_{Color::White};
    std::atomic<bool> tenured_{false};
    HeapObject* next_{nullptr};

public:
    virtual ~HeapObject() = default;

    Color get_color() const { return color_.load(std::memory_order_relaxed); }
    void set_color(Color c) { color_.store(c, std::memory_order_relaxed); }

    bool is_tenured() const { return tenured_.load(std::memory_order_relaxed); }
    void set_tenured(bool t) { tenured_.store(t, std::memory_order_relaxed); }

    HeapObject* next() const { return next_; }
    void set_next(HeapObject* n) { next_ = n; }

    virtual void trace_references(GCHeap* heap) = 0;
};

class GCHeap {
private:
    std::mutex heap_mutex_;
    HeapObject* young_head_{nullptr};
    HeapObject* tenured_head_{nullptr};
    std::atomic<size_t> allocated_bytes_{0};
    std::atomic<size_t> gc_threshold_{1024 * 1024};
    std::atomic<GCPhase> gc_phase_{GCPhase::Off};

public:
    GCHeap() = default;
    ~GCHeap() {
        purge();
    }

    template <typename T, typename... Args>
    GCPtr<T> allocate(Args&&... args) {
        static_assert(std::is_base_of_v<HeapObject, T>, "T must inherit from HeapObject");
        
        // Allocate raw memory
        void* mem = std::malloc(sizeof(T));
        if (!mem) throw std::bad_alloc();
        T* obj = new (mem) T(std::forward<Args>(args...));

        {
            std::lock_guard<std::mutex> lock(heap_mutex_);
            obj->set_next(young_head_);
            young_head_ = obj;
        }

        allocated_bytes_.fetch_add(sizeof(T), std::memory_order_relaxed);
        check_and_trigger_gc();

        return GCPtr<T>(obj);
    }

    void write_barrier(HeapObject* source, HeapObject* target) {
        if (!source || !target) return;
        // Baker-style Dijkstra write barrier: if source is Black and target is White, make target Gray
        if (source->get_color() == Color::Black && target->get_color() == Color::White) {
            target->set_color(Color::Gray);
            push_to_gray_stack(target);
        }
    }

    void push_to_gray_stack(HeapObject* obj) {
        // Internal marking worklist integration point
        (void)obj;
    }

    void check_and_trigger_gc() {
        if (allocated_bytes_.load(std::memory_order_relaxed) > gc_threshold_.load(std::memory_order_relaxed)) {
            collect();
        }
    }

    void collect() {
        gc_phase_.store(GCPhase::ConcurrentMark, std::memory_order_seq_cst);
        // Execute STW & Mark-Sweep cycle
        sweep();
        gc_phase_.store(GCPhase::Off, std::memory_order_seq_cst);
    }

    void sweep() {
        std::lock_guard<std::mutex> lock(heap_mutex_);
        
        // Sweep Young Generation
        HeapObject* prev = nullptr;
        HeapObject* curr = young_head_;
        size_t freed = 0;
        
        while (curr) {
            if (curr->get_color() == Color::White) {
                HeapObject* dead = curr;
                curr = curr->next();
                if (prev) {
                    prev->set_next(curr);
                } else {
                    young_head_ = curr;
                }
                freed += sizeof(*dead);
                dead->~HeapObject();
                std::free(dead);
            } else {
                curr->set_color(Color::White); // Reset for next cycle
                prev = curr;
                curr = curr->next();
            }
        }
        allocated_bytes_.fetch_sub(freed, std::memory_order_relaxed);
    }

    void purge() {
        std::lock_guard<std::mutex> lock(heap_mutex_);
        HeapObject* curr = young_head_;
        while (curr) {
            HeapObject* next = curr->next();
            curr->~HeapObject();
            std::free(curr);
            curr = next;
        }
        young_head_ = nullptr;

        curr = tenured_head_;
        while (curr) {
            HeapObject* next = curr->next();
            curr->~HeapObject();
            std::free(curr);
            curr = next;
        }
        tenured_head_ = nullptr;
        allocated_bytes_.store(0, std::memory_order_relaxed);
    }
};

template <typename T>
class GCPtr {
private:
    T* ptr_;

public:
    explicit GCPtr(T* p = nullptr) : ptr_(p) {}

    T* get() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }

    operator bool() const { return ptr_ != nullptr; }

    void write(GCHeap* heap, T* new_val, HeapObject* parent_obj = nullptr) {
        if (parent_obj && new_val) {
            heap->write_barrier(parent_obj, static_cast<HeapObject*>(new_val));
        }
        ptr_ = new_val;
    }
};

class RootRegistry {
private:
    std::mutex mutex_;
    std::vector<Value*> roots_;

public:
    void register_root(Value* root) {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_.push_back(root);
    }

    void unregister_root(Value* root) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = roots_.begin(); it != roots_.end(); ++it) {
            if (*it == root) {
                roots_.erase(it);
                break;
            }
        }
    }
};

// ============================================================================
// 5. VM STACK, REGISTERS & EXECUTION ENGINE
// ============================================================================

struct alignas(64) CallFrame {
    uint32_t return_ip;
    uint32_t register_base;
    uint32_t local_count;
};

class alignas(64) VMStack {
private:
    static constexpr size_t STACK_CAPACITY = 1024 * 64; // 64K slots
    Value slots_[STACK_CAPACITY];
    size_t sp_{0};
    CallFrame frames_[1024];
    size_t fp_{0};

public:
    VMStack() = default;

    void push(Value val) {
        if (sp_ >= STACK_CAPACITY) [[unlikely]] {
            throw std::overflow_error("TitanVM Stack Overflow");
        }
        slots_[sp_++] = val;
    }

    Value pop() {
        if (sp_ == 0) [[unlikely]] {
            throw std::underflow_error("TitanVM Stack Underflow");
        }
        return slots_[--sp_];
    }

    Value& peek(size_t distance) {
        if (distance >= sp_) [[unlikely]] {
            throw std::out_of_range("TitanVM Stack Peek Out of Bounds");
        }
        return slots_[sp_ - 1 - distance];
    }

    void push_frame(CallFrame frame) {
        if (fp_ >= 1024) [[unlikely]] {
            throw std::overflow_error("TitanVM Call Frame Stack Overflow");
        }
        frames_[fp_++] = frame;
    }

    CallFrame pop_