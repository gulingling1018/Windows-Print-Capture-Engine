#pragma once

#include <cassert>
#include <type_traits>
#include <utility>

namespace wpce::common
{

/// Minimal RAII COM smart pointer.
///
/// Wraps a raw COM interface pointer and calls Release() on destruction.
/// Supports copy (AddRef), move, and the standard COM output patterns
/// (operator&, **, AddressOf).
template <typename T>
class com_ptr
{
 public:
  com_ptr() noexcept = default;

  explicit com_ptr(T* ptr) noexcept : ptr_(ptr) {}

  ~com_ptr()
  {
    if (ptr_ != nullptr)
    {
      ptr_->Release();
    }
  }

  // ── Copy (AddRef) ─────────────────────────────────────────────────────

  com_ptr(const com_ptr& other) noexcept : ptr_(other.ptr_)
  {
    if (ptr_ != nullptr)
    {
      ptr_->AddRef();
    }
  }

  com_ptr& operator=(const com_ptr& other) noexcept
  {
    if (this != &other)
    {
      reset();
      ptr_ = other.ptr_;
      if (ptr_ != nullptr)
      {
        ptr_->AddRef();
      }
    }
    return *this;
  }

  // ── Move ──────────────────────────────────────────────────────────────

  com_ptr(com_ptr&& other) noexcept : ptr_(other.ptr_)
  {
    other.ptr_ = nullptr;
  }

  com_ptr& operator=(com_ptr&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  // ── Operators ─────────────────────────────────────────────────────────

  T* get() const noexcept
  {
    return ptr_;
  }

  T* operator->() const noexcept
  {
    assert(ptr_ != nullptr);
    return ptr_;
  }

  T& operator*() const noexcept
  {
    assert(ptr_ != nullptr);
    return *ptr_;
  }

  explicit operator bool() const noexcept
  {
    return ptr_ != nullptr;
  }

  // ── COM output helpers ────────────────────────────────────────────────

  /// Returns the address of the internal pointer for COM output parameters.
  /// Asserts the pointer is currently null (will leak otherwise).
  T** operator&() noexcept
  {
    assert(ptr_ == nullptr);
    return &ptr_;
  }

  /// Returns the address of the internal pointer WITHOUT asserting null.
  /// Use for cases where the pointer may already be set.
  T** AddressOf() noexcept
  {
    reset();
    return &ptr_;
  }

  // ── Lifetime ──────────────────────────────────────────────────────────

  void reset() noexcept
  {
    if (ptr_ != nullptr)
    {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

  T* detach() noexcept
  {
    T* result = ptr_;
    ptr_ = nullptr;
    return result;
  }

  void attach(T* ptr) noexcept
  {
    reset();
    ptr_ = ptr;
  }

 private:
  T* ptr_{nullptr};
};

/// Helper: QI a COM pointer and return the result as com_ptr<U>.
template <typename U, typename T>
com_ptr<U> query_interface(T* source) noexcept
{
  if (source == nullptr)
  {
    return com_ptr<U>{};
  }
  U* result = nullptr;
  if (FAILED(source->QueryInterface(__uuidof(U), reinterpret_cast<void**>(&result))))
  {
    return com_ptr<U>{};
  }
  return com_ptr<U>{result};
}

}  // namespace wpce::common
