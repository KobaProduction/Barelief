#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace dsp_kernels::detail {
// Aligned owning storage with explicit failure: exception-free firmware must
// let prepare() report OOM instead of terminating in a vector allocator.
class FftStorage {
  public:
    FftStorage() noexcept = default;
    explicit FftStorage(std::size_t n,float value=0) noexcept {
        if(resize(n))std::fill_n(data_,n,value);
    }
    ~FftStorage(){release(data_);}
    FftStorage(const FftStorage& other) : FftStorage(other.size_) {
        if(size_!=other.size_) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc();
#else
            std::abort(); // Same copy failure policy as std::vector without exceptions.
#endif
        }
        if(size_)std::copy_n(other.data_,size_,data_);
    }
    FftStorage& operator=(const FftStorage& other) {
        if(this!=&other){FftStorage copy(other);swap(copy);}return *this;
    }
    FftStorage(FftStorage&& other) noexcept {swap(other);}
    FftStorage& operator=(FftStorage&& other) noexcept {
        if(this!=&other){FftStorage old;old.swap(*this);swap(other);}return *this;
    }
    bool resize(std::size_t n) noexcept {
        if(n>capacity_) {
            if(n>std::numeric_limits<std::size_t>::max()/sizeof(float))return false;
            auto* fresh=allocate(n*sizeof(float));
            if(!fresh)return false;
            if(size_)std::copy_n(data_,size_,fresh);
            release(data_);
            data_=fresh;capacity_=n;
        }
        if(n>size_)std::fill(data_+size_,data_+n,0.f);
        size_=n;return true;
    }
    void clear() noexcept {size_=0;}
    void swap(FftStorage& other) noexcept {
        std::swap(data_,other.data_);std::swap(size_,other.size_);std::swap(capacity_,other.capacity_);
    }
    float* data() noexcept{return data_;}
    const float* data()const noexcept{return data_;}
    std::size_t size()const noexcept{return size_;}
    std::size_t capacity()const noexcept{return capacity_;}
    bool empty()const noexcept{return !size_;}
    float* begin()noexcept{return data_;}
    const float* begin()const noexcept{return data_;}
    float* end()noexcept{return size_?data_+size_:data_;}
    const float* end()const noexcept{return size_?data_+size_:data_;}
    float& operator[](std::size_t i)noexcept{return data_[i];}
    const float& operator[](std::size_t i)const noexcept{return data_[i];}
  private:
    static float* allocate(std::size_t bytes) noexcept {
#if defined(DSP_FFT_USE_C_ALIGNED_STORAGE)
        // Some exception-free C++ runtimes implement aligned nothrow new by
        // calling throwing new. Use a portable C allocation in this profile.
        constexpr auto extra=15+sizeof(void*);
        if(bytes>std::numeric_limits<std::size_t>::max()-extra)return nullptr;
        void* base=std::malloc(bytes+extra);
        if(!base)return nullptr;
        const auto address=(reinterpret_cast<std::uintptr_t>(base)+extra)&~std::uintptr_t{15};
        std::memcpy(reinterpret_cast<void*>(address-sizeof(void*)),&base,sizeof(base));
        return reinterpret_cast<float*>(address);
#else
        return static_cast<float*>(::operator new(bytes,std::align_val_t{16},std::nothrow));
#endif
    }
    static void release(float* pointer) noexcept {
#if defined(DSP_FFT_USE_C_ALIGNED_STORAGE)
        if(!pointer)return;
        void* base;
        std::memcpy(&base,reinterpret_cast<unsigned char*>(pointer)-sizeof(void*),sizeof(base));
        std::free(base);
#else
        ::operator delete(pointer,std::align_val_t{16});
#endif
    }
    float* data_=nullptr;
    std::size_t size_=0,capacity_=0;
};
}
