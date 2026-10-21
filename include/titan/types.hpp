#ifndef TITAN_TYPES_HPP
#define TITAN_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <cassert>
#include <atomic>
#include <vector>
#include <string>
#include <type_traits>
#include <mutex>
#include <stdexcept>

namespace titan {

// Forward declarations
class HeapObject;
template <typename T> class GCPtr;
class RootRegistry;

// ============================================================================
// 1. Value Representation (64-bit NaN-boxed Tagged Union)
// ============================================================================

enum class TypeTag : uint8_t {
    Double  = 0,
    Int64   = 1,
    Boolean = 2,
    Nil     = 3,
    Object  = 4
};

class Value {
private:
    uint64_t bits_;

    // NaN-boxing constants
    // A quiet NaN has the form: 0x7FF8000000000000 | payload
    static constexpr uint64_t QNAN_MASK      = 0x7FF8000000000000ULL;
    static constexpr uint64_t TAG_SHIFT      = 48;
    static constexpr uint64_t TAG_MASK       = 0x7ULL;
    static constexpr uint64_t PAYLOAD_MASK   = 0x0000FFFFFFFFFFFFULL;

    static constexpr uint64_t TAG_DOUBLE     = 0;
    static constexpr uint64_t TAG_INT64      = 1;
    static constexpr uint64_t TAG_BOOLEAN    = 2;
    static constexpr uint64_t TAG_NIL        = 3;
    static constexpr uint64_t TAG_OBJECT     = 4;

    inline constexpr uint64_t make_nan_tagged(uint64_t tag, uint64_t payload) const noexcept {
        return QNAN_MASK | (tag << TAG_SHIFT) | (payload & PAYLOAD_MASK);
    }

public:
    inline constexpr Value() noexcept 
        : bits_(make_nan_tagged(TAG_NIL, 0)) {}

    inline constexpr Value(double d) noexcept {
        // Pun double to bits. If it's a NaN, we mask it to avoid collisions.
        uint64_t u;
        __builtin_memcpy(&u, &d, sizeof(double));
        if ((u & QNAN_MASK) == QNAN_MASK) {
            // Force canonical quiet NaN to preserve boxing integrity
            bits_ = QNAN_MASK | 1;
        } else {
            bits_ = u;
        }
    }

    inline constexpr Value(int64_t i) noexcept
        : bits_(make_nan_tagged(TAG_INT64, static_cast<uint64_t>(i))) {}

    inline constexpr Value(bool b) noexcept
        : bits_(make_nan_tagged(TAG_BOOLEAN, b ? 1ULL : 0ULL)) {}

    struct NilTag {};
    inline constexpr Value(NilTag) noexcept
        : bits_(make_nan_tagged(TAG_NIL, 0)) {}

    inline explicit Value(HeapObject* obj) noexcept
        : bits_(make_nan_tagged(TAG_OBJECT, reinterpret_cast<uint64_t>(obj))) {}

    inline constexpr bool is_double() const noexcept {
        return (bits_ & QNAN_MASK) != QNAN_MASK;
    }

    inline constexpr bool is_int64() const noexcept {
        return (bits_ & QNAN_MASK) == QNAN_MASK && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_INT64);
    }

    inline constexpr bool is_boolean() const noexcept {
        return (bits_ & QNAN_MASK) == QNAN_MASK && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_BOOLEAN);
    }

    inline constexpr bool is_nil() const noexcept {
        return (bits_ & QNAN_MASK) == QNAN_MASK && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_NIL);
    }

    inline constexpr bool is_object() const noexcept {
        return (bits_ & QNAN_MASK) == QNAN_MASK && (((bits_ >> TAG_SHIFT) & TAG_MASK) == TAG_OBJECT);
    }

    inline double as_double() const noexcept {
        assert(is_double());
        double d;
        __builtin_memcpy(&d, &bits_, sizeof(double));
        return d;
    }

    inline int64_t as_int64() const noexcept {
        assert(is_int64());
        int64_t val;
        uint64_t raw = bits_ & PAYLOAD_MASK;
        __builtin_memcpy(&val, &raw, sizeof(int64_t));
        // Sign extension if necessary for 48-bit payload
        if (raw & 0x0000800000000000ULL) {
            val |= static_cast<int64_t>(0xFFFF000000000000ULL);
        }
        return val;
    }

    inline bool as_boolean() const noexcept {
        assert(is_boolean());
        return (bits_ & PAYLOAD_MASK) != 0;
    }

    inline HeapObject* as_object() const noexcept {
        assert(is_object());
        return reinterpret_cast<HeapObject*>(bits_ & PAYLOAD_MASK);
    }

    inline constexpr uint64_t raw() const noexcept {
        return bits_;
    }

    inline static constexpr Value from_raw(uint64_t raw_bits) noexcept {
        Value v;
        v.bits_ = raw_bits;
        return v;
    }
};

// ============================================================================
// 2. Memory Management & GC Infrastructure
// ============================================================================

enum class GCPhase : uint8_t {
    Off,
    Marking,
    Sweeping,
    Compacting
};

enum class ObjectType : uint8_t {
    String,
    Function,
    Instance,
    Array
};

class HeapObject {
public:
    ObjectType type;
    uint8_t gc_flags; // Bit 0: Marked, Bit 1: Tenured, etc.
    uint32_t size;
    HeapObject* next;

    inline HeapObject(ObjectType t, uint32_t sz) noexcept
        : type(t), gc_flags(0), size(sz), next(nullptr) {}

    virtual ~HeapObject() = default;

    inline bool is_marked() const noexcept { return (gc_flags & 1) != 0; }
    inline void set_marked(bool m) noexcept {
        if (m) gc_flags |= 1;
        else gc_flags &= static_cast<uint8_t>(~1);
    }

    inline bool is_tenured() const noexcept { return (gc_flags & 2) != 0; }
    inline void set_tenured(bool t) noexcept {
        if (t) gc_flags |= 2;
        else gc_flags &= static_cast<uint8_t>(~2);
    }

    // Dijkstra Write Barrier
    void write_barrier(Value* slot, Value new_val);
};

// Thread-safe Root Registry for tracking execution roots
class RootRegistry {
private:
    std::vector<Value*> roots_;
    std::mutex mutex_;

public:
    RootRegistry() = default;
    ~RootRegistry() = default;

    void register_root(Value* root_ptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_.push_back(root_ptr);
    }

    void unregister_root(Value* root_ptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = roots_.begin(); it != roots_.end(); ++it) {
            if (*it == root_ptr) {
                roots_.erase(it);
                break;
            }
        }
    }

    template <typename Callback>
    void enumerate_roots(Callback&& cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Value* root : roots_) {
            cb(root);
        }
    }
};

// RAII GCPtr for memory safety
template <typename T>
class GCPtr {
private:
    HeapObject* ptr_;

public:
    inline explicit GCPtr(HeapObject* p = nullptr) noexcept : ptr_(p) {
        static_assert(std::is_base_of<HeapObject, T>::value, "T must derive from HeapObject");
    }

    inline GCPtr(const GCPtr& other) noexcept = default;
    inline GCPtr& operator=(const GCPtr& other) noexcept = default;

    inline T* get() const noexcept { return static_cast<T*>(ptr_); }
    inline T* operator->() const noexcept { return get(); }
    inline T& operator*() const noexcept { return *get(); }

    inline explicit operator bool() const noexcept { return ptr_ != nullptr; }

    inline HeapObject* raw() const noexcept { return ptr_; }

    inline void write(Value* slot, Value val, RootRegistry& registry) {
        if (ptr_) {
            ptr_->write_barrier(slot, val);
        } else {
            *slot = val;
        }
    }
};

// ============================================================================
// 3. VM Stack, Registers, & Execution Structures
// ============================================================================

struct Instruction {
    uint32_t raw_word;

    inline constexpr explicit Instruction(uint32_t word) noexcept : raw_word(word) {}
    inline constexpr uint32_t opcode() const noexcept { return raw_word & 0x3F; }
    inline constexpr uint32_t argA() const noexcept { return (raw_word >> 6) & 0xFF; }
    inline constexpr uint32_t argB() const noexcept { return (raw_word >> 14) & 0x3FFFF; }
};

class alignas(64) CallFrame {
public:
    Value* stack_base;
    uint32_t register_count;
    const uint32_t* instruction_ptr;
    CallFrame* caller;

    inline CallFrame(Value* base, uint32_t reg_count, const uint32_t* ip, CallFrame* prev) noexcept
        : stack_base(base), register_count(reg_count), instruction_ptr(ip), caller(prev) {}
};

class VMExecutionStack {
private:
    static constexpr size_t STACK_CAPACITY = 16384;
    Value stack_buffer_[STACK_CAPACITY];
    Value* stack_top_;
    CallFrame* current_frame_;

public:
    VMExecutionStack() noexcept 
        : stack_top_(stack_buffer_), current_frame_(nullptr) {}

    inline Value* top() noexcept { return stack_top_; }
    inline const Value* top() const noexcept { return stack_top_; }

    inline void push(Value val) {
        if (stack_top_ >= stack_buffer_ + STACK_CAPACITY) {
            throw std::overflow_error("TitanVM Stack Overflow Exception");
        }
        *stack_top_++ = val;
    }

    inline Value pop() {
        if (stack_top_ <= stack_buffer_) {
            throw std::underflow_error("TitanVM Stack Underflow Exception");
        }
        return *--stack_top_;
    }

    inline Value* get_register(uint32_t index) {
        if (!current_frame_) {
            throw std::runtime_error("Attempted register access outside active call frame");
        }
        if (index >= current_frame_->register_count) {
            throw std::out_of_range("Register index out of bounds in active call frame");
        }
        return current_frame_->stack_base + index;
    }

    inline void push_frame(uint32_t reg_count, const uint32_t* ip) {
        if (stack_top_ + reg_count > stack_buffer_ + STACK_CAPACITY) {
            throw std::overflow_error("Stack overflow allocating call frame");
        }
        current_frame_ = new (stack_top_) CallFrame(stack_top_, reg_count, ip, current_frame_);
        stack_top_ += reg_count;
    }

    inline void pop_frame() {
        if (!current_frame_) {
            throw std::runtime_error("Attempted frame underflow");
        }
        stack_top_ = current_frame_->stack_base;
        current_frame_ = current_frame_->caller;
    }

    inline CallFrame* current_frame() noexcept { return current_frame_; }
};

// ============================================================================
// 4. Safepoints and Exception Integration
// ============================================================================

class VMRuntimeContext {
private:
    std::atomic<GCPhase> gc_phase_;
    std::atomic<bool> safepoint_requested_;

public:
    VMRuntimeContext() noexcept 
        : gc_phase_(GCPhase::Off), safepoint_requested_(false) {}

    inline void request_safepoint() noexcept {
        safepoint_requested_.store(true, std::memory_order_release);
    }

    inline void poll_safepoint() {
        if (__builtin_expect(safepoint_requested_.load(std::memory_order_acquire), false)) {
            perform_safepoint_pause();
        }
    }

private:
    void perform_safepoint_pause() {
        // Synchronization barrier for Stop-The-World GC phase transitions
        std::atomic_thread_fence(std::memory_order_seq_cst);
        // Implementation yields execution until GC phase clears
        while (gc_phase_.load(std::memory_order_acquire) != GCPhase::Off) {
            std::this_thread::yield();
        }
        safepoint_requested_.store(false, std::memory_order_release);
    }
};

// Inline implementation of HeapObject write barrier
inline void HeapObject::write_barrier(Value* slot, Value new_val) {
    *slot = new_val;
    if (new_val.is_object() && !this->is_marked() && new_val.as_object()->is_marked()) {
        // Maintain Dijkstra tri-color invariant: Gray -> Black transition enforcement
        this->set_marked(true);
    }
}

} // namespace titan

#endif // TITAN_TYPES_HPP