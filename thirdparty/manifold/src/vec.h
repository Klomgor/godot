// Copyright 2021 The Manifold Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#if TRACY_ENABLE && TRACY_MEMORY_USAGE
#include "tracy/Tracy.hpp"
#else
#define TracyAllocS(ptr, size, n) (void)0
#define TracyFreeS(ptr, n) (void)0
#endif
#include <vector>

#include "manifold/vec_view.h"
#include "parallel.h"

namespace manifold {

#if (MANIFOLD_PAR == 1)
extern tbb::task_arena gc_arena;
#endif

template <typename T>
class Vec;

/*
 * Specialized vector implementation with multithreaded fill and uninitialized
 * memory optimizations.
 * Note that the constructor and resize function will not perform initialization
 * if the parameter val is not set. Also, this implementation is a toy
 * implementation that did not consider things like non-trivial
 * constructor/destructor, please keep T trivial.
 */
template <typename T>
class Vec : public VecView<T> {
 public:
  Vec() {}

  // Note that the vector constructed with this constructor will contain
  // uninitialized memory. Please specify `val` if you need to make sure that
  // the data is initialized.
  Vec(size_t size) : VecView<T>() {
    reserve(size);
    this->size_ = size;
  }

  Vec(size_t size, T val) : VecView<T>() { resize(size, val); }

  Vec(const Vec<T>& vec) : VecView<T>() { *this = Vec(vec.view()); }

  Vec(const VecView<const T>& vec) : VecView<T>() {
    this->size_ = vec.size();
    this->capacity_ = this->size_;
    auto policy = autoPolicy(this->size_);
    if (this->size_ != 0) {
      this->ptr_ = reinterpret_cast<T*>(malloc(this->size_ * sizeof(T)));
      ASSERT(this->ptr_ != nullptr, std::bad_alloc());
      TracyAllocS(this->ptr_, this->size_ * sizeof(T), 3);
      copy(policy, vec.begin(), vec.end(), this->ptr_);
    }
  }

  Vec(const std::vector<T>& vec) : VecView<T>() {
    this->size_ = vec.size();
    this->capacity_ = this->size_;
    auto policy = autoPolicy(this->size_);
    if (this->size_ != 0) {
      this->ptr_ = reinterpret_cast<T*>(malloc(this->size_ * sizeof(T)));
      ASSERT(this->ptr_ != nullptr, std::bad_alloc());
      TracyAllocS(this->ptr_, this->size_ * sizeof(T), 3);
      copy(policy, vec.begin(), vec.end(), this->ptr_);
    }
  }

  Vec(Vec<T>&& vec) : VecView<T>() {
    this->ptr_ = vec.ptr_;
    this->size_ = vec.size_;
    capacity_ = vec.capacity_;
    vec.ptr_ = nullptr;
    vec.size_ = 0;
    vec.capacity_ = 0;
  }

  operator VecView<T>() { return {this->ptr_, this->size_}; }
  operator VecView<const T>() const { return {this->ptr_, this->size_}; }

  ~Vec() {
    if (this->ptr_ != nullptr) {
      free_async(this->ptr_, capacity_);
    }
    this->ptr_ = nullptr;
    this->size_ = 0;
    capacity_ = 0;
  }

  Vec<T>& operator=(const Vec<T>& other) {
    if (&other == this) return *this;
    if (this->ptr_ != nullptr) {
      free_async(this->ptr_, capacity_);
    }
    this->size_ = other.size_;
    capacity_ = other.size_;
    if (this->size_ != 0) {
      this->ptr_ = reinterpret_cast<T*>(malloc(this->size_ * sizeof(T)));
      ASSERT(this->ptr_ != nullptr, std::bad_alloc());
      TracyAllocS(this->ptr_, this->size_ * sizeof(T), 3);
      manifold::copy(other.begin(), other.end(), this->ptr_);
    }
    return *this;
  }

  Vec<T>& operator=(Vec<T>&& other) {
    if (&other == this) return *this;
    if (this->ptr_ != nullptr) {
      free_async(this->ptr_, capacity_);
    }
    this->size_ = other.size_;
    capacity_ = other.capacity_;
    this->ptr_ = other.ptr_;
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    return *this;
  }

  operator VecView<T>() const { return {this->ptr_, this->size_}; }

  void swap(Vec<T>& other) {
    std::swap(this->ptr_, other.ptr_);
    std::swap(this->size_, other.size_);
    std::swap(capacity_, other.capacity_);
  }

  inline void push_back(const T& val) {
    if (this->size_ >= capacity_) {
      // avoid dangling pointer in case val is a reference of our array
      T val_copy = val;
      reserve(capacity_ == 0 ? 128 : capacity_ * 2);
      this->ptr_[this->size_++] = val_copy;
      return;
    }
    this->ptr_[this->size_++] = val;
  }

  inline void extend(size_t n) {
    if (this->size_ + n >= capacity_)
      reserve(capacity_ == 0 ? 128 : std::max(capacity_ * 2, this->size_ + n));
    this->size_ += n;
  }

  void reserve(size_t n) {
    if (n > capacity_) {
      T* newBuffer = reinterpret_cast<T*>(malloc(n * sizeof(T)));
      ASSERT(newBuffer != nullptr, std::bad_alloc());
      TracyAllocS(newBuffer, n * sizeof(T), 3);
      if (this->size_ > 0)
        manifold::copy(autoPolicy(this->size_), this->ptr_,
                       this->ptr_ + this->size_, newBuffer);
      if (this->ptr_ != nullptr) {
        free_async(this->ptr_, capacity_);
      }
      this->ptr_ = newBuffer;
      capacity_ = n;
    }
  }

  void resize(size_t newSize, T val = T()) {
    bool shrink = this->size_ > 2 * newSize && this->size_ > 16;
    reserve(newSize);
    if (this->size_ < newSize) {
      fill(autoPolicy(newSize - this->size_), this->ptr_ + this->size_,
           this->ptr_ + newSize, val);
    }
    this->size_ = newSize;
    if (shrink) shrink_to_fit();
  }

  void resize_nofill(size_t newSize) {
    bool shrink = this->size_ > 2 * newSize && this->size_ > 16;
    reserve(newSize);
    this->size_ = newSize;
    if (shrink) shrink_to_fit();
  }

  void pop_back() { resize_nofill(this->size_ - 1); }

  void clear(bool shrink = true) {
    this->size_ = 0;
    if (shrink) shrink_to_fit();
  }

  void shrink_to_fit() {
    T* newBuffer = nullptr;
    if (this->size_ > 0) {
      newBuffer = reinterpret_cast<T*>(malloc(this->size_ * sizeof(T)));
      ASSERT(newBuffer != nullptr, std::bad_alloc());
      TracyAllocS(newBuffer, this->size_ * sizeof(T), 3);
      manifold::copy(this->ptr_, this->ptr_ + this->size_, newBuffer);
    }
    if (this->ptr_ != nullptr) {
      free_async(this->ptr_, capacity_);
    }
    this->ptr_ = newBuffer;
    capacity_ = this->size_;
  }

  size_t capacity() const { return capacity_; }

 private:
  size_t capacity_ = 0;

  static_assert(std::is_trivially_destructible<T>::value);

  static void free_async(T* ptr, size_t size) {
    // Only do async free if the size is large, because otherwise we may be able
    // to reuse the allocation, and the deallocation probably won't trigger
    // munmap.
    // Currently it is set to 64 pages (4kB page).
    constexpr size_t ASYNC_FREE_THRESHOLD = 1 << 18;
    TracyFreeS(ptr, 3);
#if (MANIFOLD_PAR == 1)
    if (size * sizeof(T) > ASYNC_FREE_THRESHOLD)
      gc_arena.enqueue([ptr]() { free(ptr); });
    else
#endif
      free(ptr);
  }
};
}  // namespace manifold
