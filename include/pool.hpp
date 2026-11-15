#pragma once
#include <cstddef>
#include <cstdint>
#include <new>

struct Pool {
    explicit Pool(std::size_t obj_size, std::size_t capacity)
        : slot_(round_up(obj_size < sizeof(void*) ? sizeof(void*) : obj_size)),
          cap_(capacity) {
        buf_  = static_cast<std::uint8_t*>(::operator new(slot_ * cap_));
        head_ = nullptr;
        for (std::size_t i = cap_; i-- > 0;) free(buf_ + i * slot_);
    }
    ~Pool() { ::operator delete(buf_); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    void* alloc() {
        if (!head_) return nullptr;
        void* p = head_;
        head_ = *static_cast<void**>(head_);
        return p;
    }

    void free(void* p) {
        if (!p) return;
        *static_cast<void**>(p) = head_;
        head_ = p;
    }

private:
    static std::size_t round_up(std::size_t n) {
        constexpr std::size_t a = alignof(std::max_align_t);
        return (n + a - 1) / a * a;
    }
    std::size_t   slot_, cap_;
    std::uint8_t* buf_;
    void*         head_;
};
