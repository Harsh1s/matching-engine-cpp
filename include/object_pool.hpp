#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace me {

// Fixed-block free-list arena. All allocations from a single instance are the
// same size (the std::list node type), which holds for every order-book level.
// Frees are pushed onto an intrusive free list so resting/cancel churn reuses
// memory instead of round-tripping the global allocator, keeping nodes packed.
class PoolResource {
public:
    PoolResource() = default;
    PoolResource(const PoolResource&) = delete;
    PoolResource& operator=(const PoolResource&) = delete;
    ~PoolResource() {
        for (void* chunk : chunks_) {
            ::operator delete(chunk, std::align_val_t(align_));
        }
    }

    void* allocate(std::size_t bytes, std::size_t align) {
        std::size_t need = block_size(bytes, align);
        if (block_ == 0) {
            block_ = need;
            align_ = align;
        } else if (need != block_ || align != align_) {
            return ::operator new(bytes, std::align_val_t(align));  // off-size fallback
        }
        if (free_ == nullptr) grow();
        void* p = free_;
        free_ = *reinterpret_cast<void**>(free_);
        return p;
    }

    void deallocate(void* p, std::size_t bytes, std::size_t align) noexcept {
        if (block_ == 0 || block_size(bytes, align) != block_ || align != align_) {
            ::operator delete(p, std::align_val_t(align));
            return;
        }
        *reinterpret_cast<void**>(p) = free_;
        free_ = p;
    }

private:
    static std::size_t block_size(std::size_t bytes, std::size_t align) {
        std::size_t b = bytes < sizeof(void*) ? sizeof(void*) : bytes;
        return (b + align - 1) & ~(align - 1);
    }

    void grow() {
        constexpr std::size_t kBlocksPerChunk = 1024;
        char* chunk = static_cast<char*>(::operator new(block_ * kBlocksPerChunk, std::align_val_t(align_)));
        chunks_.push_back(chunk);
        for (std::size_t i = 0; i < kBlocksPerChunk; ++i) {
            void* slot = chunk + i * block_;
            *reinterpret_cast<void**>(slot) = free_;
            free_ = slot;
        }
    }

    std::size_t block_ = 0;
    std::size_t align_ = alignof(std::max_align_t);
    void* free_ = nullptr;
    std::vector<void*> chunks_;
};

// STL allocator that draws from a PoolResource shared (by shared_ptr) across all
// rebound copies. Each MatchingEngine owns one resource and injects it into every
// price level, so order nodes are pooled per engine and the pool always outlives
// its nodes even when allocated and freed on different threads (no thread_local
// lifetime hazard). A default-constructed allocator owns a private resource.
template <typename T>
class PoolAllocator {
public:
    using value_type = T;
    template <typename U>
    friend class PoolAllocator;

    PoolAllocator() : res_(std::make_shared<PoolResource>()) {}
    explicit PoolAllocator(std::shared_ptr<PoolResource> res) : res_(std::move(res)) {}
    template <typename U>
    PoolAllocator(const PoolAllocator<U>& other) noexcept : res_(other.res_) {}

    T* allocate(std::size_t n) {
        if (n != 1) return static_cast<T*>(::operator new(n * sizeof(T), std::align_val_t(alignof(T))));
        return static_cast<T*>(res_->allocate(sizeof(T), alignof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (n != 1) {
            ::operator delete(p, std::align_val_t(alignof(T)));
            return;
        }
        res_->deallocate(p, sizeof(T), alignof(T));
    }

    template <typename U>
    bool operator==(const PoolAllocator<U>& o) const noexcept { return res_ == o.res_; }
    template <typename U>
    bool operator!=(const PoolAllocator<U>& o) const noexcept { return res_ != o.res_; }

private:
    std::shared_ptr<PoolResource> res_;
};

}  // namespace me
