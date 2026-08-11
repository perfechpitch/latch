#ifndef _CIRCULAR_BUFFER_
#define _CIRCULAR_BUFFER_

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace latch {

template <typename T>
class CircularBuffer {
 private:
  T* buffer;
  size_t capacity;
  size_t maxCapacity;
  size_t size;
  size_t head;
  size_t tail;

  void resize(size_t new_capacity) {
    if (new_capacity <= capacity) return;
    if (maxCapacity > 0 && new_capacity > maxCapacity) {
      new_capacity = maxCapacity;
    }
    T* new_buffer = new T[new_capacity];
    if (size > 0) {
      if (head <= tail) {
        std::memcpy(new_buffer, buffer + head, size * sizeof(T));
      } else {
        size_t first_part = capacity - head;
        std::memcpy(new_buffer, buffer + head, first_part * sizeof(T));
        std::memcpy(new_buffer + first_part, buffer, tail * sizeof(T));
      }
    }
    delete[] buffer;
    buffer = new_buffer;
    capacity = new_capacity;
    head = 0;
    tail = size;
  }

 public:
  CircularBuffer(size_t initial_capacity = 1024, size_t max_capacity = 0)
      : capacity(initial_capacity), maxCapacity(max_capacity), size(0), head(0), tail(0) {
    if (initial_capacity == 0) {
      throw std::invalid_argument("初始容量不能为0");
    }
    if (max_capacity > 0 && initial_capacity > max_capacity) {
      throw std::invalid_argument("初始容量不能大于最大容量");
    }
    buffer = new T[capacity];
  }

  ~CircularBuffer() { delete[] buffer; }

  CircularBuffer(const CircularBuffer&) = delete;
  CircularBuffer& operator=(const CircularBuffer&) = delete;

  CircularBuffer(CircularBuffer&& other) noexcept
      : buffer(other.buffer),
        capacity(other.capacity),
        maxCapacity(other.maxCapacity),
        size(other.size),
        head(other.head),
        tail(other.tail) {
    other.buffer = nullptr;
    other.capacity = 0;
    other.maxCapacity = 0;
    other.size = 0;
    other.head = 0;
    other.tail = 0;
  }

  CircularBuffer& operator=(CircularBuffer&& other) noexcept {
    if (this != &other) {
      delete[] buffer;

      buffer = other.buffer;
      capacity = other.capacity;
      maxCapacity = other.maxCapacity;
      size = other.size;
      head = other.head;
      tail = other.tail;

      other.buffer = nullptr;
      other.capacity = 0;
      other.maxCapacity = 0;
      other.size = 0;
      other.head = 0;
      other.tail = 0;
    }
    return *this;
  }

  void TryResize(size_t append_count) {
    if (append_count > (capacity - size)) {
      size_t need = size + append_count;
      size_t target = maxCapacity > 0 ? std::min(maxCapacity, need) : need;
      if (target > capacity) resize(target);
    }
  }

  size_t GetContinueBuffer(T** data) {
    *data = buffer + tail;
    if (tail == head && size == 0) return capacity - tail;
    if (tail > head) {
      return capacity - tail;
    } else {
      return head - tail;
    }
  }

  void PushWithoutData(size_t count) {
    LOGCHECK(count <= (capacity - size), "Buffer overflow");
    size_t first_part = std::min(count, capacity - tail);
    if (count > first_part) {
      size_t second_part = count - first_part;
    }
    tail = (tail + count) % capacity;
    size += count;
  }

  bool Push(const T* data, size_t count) {
    if (data == nullptr || count == 0) return false;

    if (count > (capacity - size)) {
      size_t needed_total = size + count;
      if (maxCapacity > 0 && needed_total > maxCapacity) {
        spdlog::info("CircularBuffer 超过最大容量限制，无法插入");
        return false;
      }
      resize(needed_total);
    }

    size_t first_part = std::min(count, capacity - tail);
    std::memcpy(buffer + tail, data, first_part * sizeof(T));
    if (count > first_part) {
      size_t second_part = count - first_part;
      std::memcpy(buffer, data + first_part, second_part * sizeof(T));
    }
    tail = (tail + count) % capacity;
    size += count;
    return true;
  }

  size_t Pop(T* data, size_t count) {
    if (data == nullptr || count == 0 || size == 0) return 0;

    size_t read_count = std::min(count, size);

    size_t first_part = std::min(read_count, capacity - head);
    std::memcpy(data, buffer + head, first_part * sizeof(T));

    if (read_count > first_part) {
      size_t second_part = read_count - first_part;
      std::memcpy(data + first_part, buffer, second_part * sizeof(T));
    }

    head = (head + read_count) % capacity;
    size -= read_count;

    return read_count;
  }

  size_t Peek(T* data, size_t count) const {
    if (data == nullptr || count == 0 || size == 0) return 0;

    size_t read_count = std::min(count, size);

    size_t first_part = std::min(read_count, capacity - head);
    std::memcpy(data, buffer + head, first_part * sizeof(T));

    if (read_count > first_part) {
      size_t second_part = read_count - first_part;
      std::memcpy(data + first_part, buffer, second_part * sizeof(T));
    }

    return read_count;
  }

  void Clear() {
    size = 0;
    head = 0;
    tail = 0;
  }

  size_t Size() const { return size; }

  size_t Capacity() const { return capacity; }

  size_t Remain() const { return capacity - size; }

  size_t MaxCapacity() const { return maxCapacity; }

  bool Empty() const { return size == 0; }

  bool Full() const { return size == capacity; }
};

template <typename T>
class CircularBufferThreadSafe {
 private:
  T* buffer;
  uint64_t capacity;

  std::atomic<uint64_t> head;
  std::atomic<uint64_t> tail;

  uint64_t size(uint64_t t, uint64_t h) const { return t >= h ? (t - h - 1) : capacity - (h - t + 1); }
  uint64_t remain(uint64_t t, uint64_t h) const { return t >= h ? (capacity - t + h - 1) : (h - t - 1); }

 public:
  CircularBufferThreadSafe(uint64_t initial_capacity = 1024) : capacity(initial_capacity + 2), head(0), tail(1) {
    if (initial_capacity == 0) {
      throw std::invalid_argument("初始容量不能为0");
    }
    buffer = new T[capacity];
    head.store(0, std::memory_order_relaxed);
    tail.store(1, std::memory_order_relaxed);
  }

  ~CircularBufferThreadSafe() { delete[] buffer; }

  CircularBufferThreadSafe(const CircularBufferThreadSafe&) = delete;
  CircularBufferThreadSafe& operator=(const CircularBufferThreadSafe&) = delete;

  uint64_t GetContinueBuffer(T** data) {
    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);

    *data = buffer + t;
    if (h == 0) {
      return capacity - t - 1;
    } else {
      if (t >= h)
        return capacity - t;
      else
        return h - t - 1;
    }
  }

  void PushWithoutData(uint64_t count) {
    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);
    LOGCHECK(count <= remain(t, h), "Buffer overflow");
    tail.store((t + count) % capacity, std::memory_order_release);
  }

  bool Push(const T* data, uint64_t count) {
    if (data == nullptr || count == 0) return false;

    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);

    if (remain(t, h) < count) {
      spdlog::info("CircularBuffer space not enough!");
      return false;
    }

    uint64_t first_part = std::min(count, capacity - t);
    std::memcpy(buffer + t, data, first_part * sizeof(T));
    if (count > first_part) {
      uint64_t second_part = count - first_part;
      std::memcpy(buffer, data + first_part, second_part * sizeof(T));
    }
    tail.store((t + count) % capacity, std::memory_order_release);

    return true;
  }

  uint64_t Pop(T* data, uint64_t count) {
    if (data == nullptr || count == 0) return 0;

    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);

    uint64_t read_count = std::min(count, size(t, h));
    uint64_t first_part = std::min(read_count, capacity - h - 1);
    std::memcpy(data, buffer + h + 1, first_part * sizeof(T));
    if (read_count > first_part) {
      uint64_t second_part = read_count - first_part;
      std::memcpy(data + first_part, buffer, second_part * sizeof(T));
    }
    head.store((h + read_count) % capacity, std::memory_order_release);
    return read_count;
  }

  uint64_t Peek(T* data, uint64_t count) {
    if (data == nullptr || count == 0) return 0;

    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);

    uint64_t read_count = std::min(count, size(t, h));
    uint64_t first_part = std::min(read_count, capacity - h - 1);
    std::memcpy(data, buffer + h + 1, first_part * sizeof(T));
    if (read_count > first_part) {
      uint64_t second_part = read_count - first_part;
      std::memcpy(data + first_part, buffer, second_part * sizeof(T));
    }
    return read_count;
  }

  uint64_t Size() const {
    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);
    return size(t, h);
  }
  uint64_t Remain() const {
    uint64_t h = head.load(std::memory_order_acquire);
    uint64_t t = tail.load(std::memory_order_acquire);
    return remain(t, h);
  }
  bool Empty() const { return Size() == 0; }
  bool Full() const { return Remain() == 0; }

  void Clear() {
    head.store(0, std::memory_order_relaxed);
    tail.store(1, std::memory_order_relaxed);
  }
};
}

#endif
