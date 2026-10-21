#ifndef TITAN_GC_HPP
#define TITAN_GC_HPP

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <thread>
#include <condition_variable>
#include <system_error>
#include <new>

namespace titan {

// Forward declarations
class HeapObject;
class VMThreadContext;
class GarbageCollector;

// ============================================================================
// 1. Value Representation & NaN-Boxing Types
// ============================================================================

enum class Tag : uint16_t {
    Double  = 0x0000,
    Int64   = 0x0001,
    Boolean = 0x0002,
    Nil     = 0x0003,
    Object  = 0x0004
};

struct Value {
    uint64_t raw;

    constexpr Value() : raw(0) {}
    constexpr explicit Value(uint64_t r) : raw(r) {}

    static constexpr uint64_t QNAN_MASK = 0x7FF8000000000000ULL;
    static constexpr uint64_t TAG_MASK  = 0xFFFF000000000000ULL;

    inline bool is_double() const {
        return (raw & QNAN_MASK) != QNAN_MASK;
    }

    inline bool is_object() const {
        return (raw & TAG_MASK) == (QNAN_MASK | (static_cast<uint64_t>(Tag::Object) << 48));
    }

    inline HeapObject* as_object() const {
        return reinterpret_cast<HeapObject*>(raw & 0x0000FFFFFFFFFFFFULL);
    }
};

// ============================================================================
// 2. Garbage Collection Phases & States
// ============================================================================

enum class GCPhase : uint8_t {
    Idle = 0,
    ConcurrentMark,
    Remark,
    Sweep,
    Compaction
};

enum class Generation : uint8_t {
    Young = 0,
    Tenured = 1
};

// ============================================================================
// 3. HeapObject Base Class
// ============================================================================

class HeapObject {
public:
    enum class Color : uint8_t {
        White = 0,
        Grey  = 1,
        Black = 2
    };

    uint32_t gc_header; // Bits: [0-1] Color, [2] Generation, [3-31] Metadata
    HeapObject* next;

    constexpr HeapObject() : gc_header(0), next(nullptr) {}
    virtual ~HeapObject() = default;

    inline Color get_color() const {
        return static_cast<Color>(gc_header & 0x03);
    }

    inline void set_color(Color c) {
        gc_header = (gc_header & ~0x03) | static_cast<uint32_t>(c);
    }

    inline Generation get_generation() const {
        return static_cast<Generation>((gc_header >> 2) & 0x01);
    }

    inline void set_generation(Generation g) {
        gc_header = (gc_header & ~(1 << 2)) | (static_cast<uint32_t>(g) << 2);
    }

    virtual void visit_pointers(class RootRegistry& registry) = 0;
};

// ============================================================================
// 4. GCPtr<T> - RAII Safe Pointer Handle
// ============================================================================

template <typename T>
class GCPtr {
private:
    T* ptr;

public:
    constexpr GCPtr() noexcept : ptr(nullptr) {}
    constexpr explicit GCPtr(T* p) noexcept : ptr(p) {
        if (ptr) {
            // Write barrier hook on creation assignment if needed
        }
    }

    ~GCPtr() = default;

    GCPtr(const GCPtr& other) noexcept : ptr(other.ptr) {}
    GCPtr& operator=(const GCPtr& other) noexcept {
        ptr = other.ptr;
        return *this;
    }

    GCPtr(GCPtr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    GCPtr& operator=(GCPtr&& other) noexcept {
        if (this != &other) {
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    T* get() const noexcept { return ptr; }
    T& operator*() const { return *ptr; }
    T* operator->() const noexcept { return ptr; }

    explicit operator bool() const noexcept { return ptr != nullptr; }

    void write(T* new_val, GarbageCollector* gc);
};

// ============================================================================
// 5. Root Registry
// ============================================================================

class RootRegistry {
private:
    std::mutex mtx_;
    std::unordered_set<HeapObject**> raw_roots_;
    std::vector<HeapObject*> direct_roots_;

public:
    RootRegistry() = default;
    ~RootRegistry() = default;

    void register_root(HeapObject** root_addr) {
        std::lock_guard<std::mutex> lock(mtx_);
        raw_roots_.insert(root_addr);
    }

    void unregister_root(HeapObject** root_addr) {
        std::lock_guard<std::mutex> lock(mtx_);
        raw_roots_.erase(root_addr);
    }

    void register_direct(HeapObject* obj) {
        if (!obj) return;
        std::lock_guard<std::mutex> lock(mtx_);
        direct_roots_.push_back(obj);
    }

    template <typename Callback>
    void iterate_roots(Callback&& cb) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto* addr : raw_roots_) {
            if (addr && *addr) {
                cb(*addr);
            }
        }
        for (auto* obj : direct_roots_) {
            if (obj) {
                cb(obj);
            }
        }
    }

    void clear_direct() {
        std::lock_guard<std::mutex> lock(mtx_);
        direct_roots_.clear();
    }
};

// ============================================================================
// 6. Garbage Collector Engine
// ============================================================================

class GarbageCollector {
private:
    std::atomic<GCPhase> phase_;
    std::atomic<bool> safepoint_flag_;
    std::atomic<bool> shutdown_requested_;

    std::mutex heap_mutex_;
    HeapObject* young_head_;
    HeapObject* tenured_head_;

    RootRegistry root_registry_;
    std::thread gc_thread_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    size_t bytes_allocated_;
    size_t next_gc_threshold_;

    void gc_worker_loop();
    void execute_collection_cycle();
    void mark_phase();
    void sweep_phase();
    void stop_the_world();
    void resume_world();

public:
    explicit GarbageCollector(size_t initial_threshold = 10485760);
    ~GarbageCollector();

    void initialize();
    void shutdown();

    HeapObject* allocate(size_t size, Generation gen);
    
    inline void write_barrier(HeapObject* source, HeapObject* target) {
        if (!source || !target) return;
        // Dijkstra style barrier: if source is black and target is white, make target grey
        if (source->get_color() == HeapObject::Color::Black && 
            target->get_color() == HeapObject::Color::White) {
            target->set_color(HeapObject::Color::Grey);
            std::lock_guard<std::mutex> lock(heap_mutex_);
            // Push to grey worklist or handle locally
        }
    }

    inline void poll_safepoint() {
        if (safepoint_flag_.load(std::memory_order_relaxed)) {
            cooperative_safepoint_suspend();
        }
    }

    void cooperative_safepoint_suspend();

    RootRegistry& get_root_registry() { return root_registry_; }
    GCPhase get_phase() const { return phase_.load(std::memory_order_acquire); }
};

// ============================================================================
// 7. Template Method Definitions
// ============================================================================

template <typename T>
void GCPtr<T>::write(T* new_val, GarbageCollector* gc) {
    if (gc && ptr != new_val) {
        // Execute barrier logic using underlying HeapObject pointers
        HeapObject* src = reinterpret_cast<HeapObject*>(ptr);
        HeapObject* tgt = reinterpret_cast<HeapObject*>(new_val);
        gc->write_barrier(src, tgt);
    }
    ptr = new_val;
}

} // namespace titan

#endif // TITAN_GC_HPP