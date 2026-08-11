#pragma once

namespace binary_log {
template <typename T>
struct constant {
  using type = T;
  const T value;
  constexpr constant(T v) : value(v) {}
};

}  // namespace binary_log

#include <type_traits>

namespace binary_log {
template <class T, template <class...> class Template>
struct is_specialization : std::false_type {};

template <template <class...> class Template, class... Args>
struct is_specialization<Template<Args...>, Template> : std::true_type {};

}  // namespace binary_log

#include <cstdint>

namespace binary_log {
enum class fmt_arg_type {
  type_bool,
  type_char,
  type_uint8,
  type_uint16,
  type_uint32,
  type_uint64,
  type_int8,
  type_int16,
  type_int32,
  type_int64,
  type_float,
  type_double,
  type_string,
};

static inline std::size_t sizeof_arg_type(fmt_arg_type type) {
  switch (type) {
    case fmt_arg_type::type_bool:
      return sizeof(bool);
    case fmt_arg_type::type_char:
      return sizeof(char);
    case fmt_arg_type::type_uint8:
      return sizeof(uint8_t);
    case fmt_arg_type::type_uint16:
      return sizeof(uint16_t);
    case fmt_arg_type::type_uint32:
      return sizeof(uint32_t);
    case fmt_arg_type::type_uint64:
      return sizeof(uint64_t);
    case fmt_arg_type::type_int8:
      return sizeof(int8_t);
    case fmt_arg_type::type_int16:
      return sizeof(int16_t);
    case fmt_arg_type::type_int32:
      return sizeof(int32_t);
    case fmt_arg_type::type_int64:
      return sizeof(int64_t);
    case fmt_arg_type::type_float:
      return sizeof(float);
    case fmt_arg_type::type_double:
      return sizeof(double);
    case fmt_arg_type::type_string:
      return sizeof(char);
    default:
      return 0;
  }
}

template <typename T>
constexpr static inline fmt_arg_type get_arg_type() = delete;

template <>
constexpr inline fmt_arg_type get_arg_type<bool>() {
  return fmt_arg_type::type_bool;
}

template <>
constexpr inline fmt_arg_type get_arg_type<char>() {
  return fmt_arg_type::type_char;
}

template <>
constexpr inline fmt_arg_type get_arg_type<uint8_t>() {
  return fmt_arg_type::type_uint8;
}

template <>
constexpr inline fmt_arg_type get_arg_type<uint16_t>() {
  return fmt_arg_type::type_uint16;
}

template <>
constexpr inline fmt_arg_type get_arg_type<uint32_t>() {
  return fmt_arg_type::type_uint32;
}

template <>
constexpr inline fmt_arg_type get_arg_type<uint64_t>() {
  return fmt_arg_type::type_uint64;
}

template <>
constexpr inline fmt_arg_type get_arg_type<int8_t>() {
  return fmt_arg_type::type_int8;
}

template <>
constexpr inline fmt_arg_type get_arg_type<int16_t>() {
  return fmt_arg_type::type_int16;
}

template <>
constexpr inline fmt_arg_type get_arg_type<int32_t>() {
  return fmt_arg_type::type_int32;
}

template <>
constexpr inline fmt_arg_type get_arg_type<int64_t>() {
  return fmt_arg_type::type_int64;
}

template <>
constexpr inline fmt_arg_type get_arg_type<float>() {
  return fmt_arg_type::type_float;
}

template <>
constexpr inline fmt_arg_type get_arg_type<double>() {
  return fmt_arg_type::type_double;
}

template <>
constexpr inline fmt_arg_type get_arg_type<const char*>() {
  return fmt_arg_type::type_string;
}

template <class T, class... Ts>
constexpr static inline bool all_args_are_constants() {
  if constexpr (is_specialization<T, constant>{}) {
    constexpr auto num_args = sizeof...(Ts);
    if constexpr (num_args == 0) {
      return true;
    } else {
      return all_args_are_constants<Ts...>();
    }
  } else {
    return false;
  }
}

}  // namespace binary_log

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace binary_log {
class packer {
  std::FILE* m_log_file;

  // This buffer is buffering fwrite calls
  // to the log file.
  //
  // fwrite already has an internal buffer
  // but this buffer is used to avoid
  // multiple fwrite calls.
  constexpr static inline std::size_t buffer_size = 1 * 1024 * 1024;
  std::array<uint8_t, buffer_size> m_buffer;
  std::size_t m_buffer_index = 0;

  template <typename T, std::size_t size>
  void buffer_or_write(T* input) {
    if (m_buffer_index + size >= buffer_size) {
      fwrite(m_buffer.data(), sizeof(uint8_t), m_buffer_index, m_log_file);
      m_buffer_index = 0;
    }

    const auto* byte_array = reinterpret_cast<const uint8_t*>(input);
    std::copy_n(byte_array, size, &m_buffer[m_buffer_index]);
    m_buffer_index += size;
  }

  template <typename T>
  void buffer_or_write(T* input, std::size_t size) {
    if (m_buffer_index + size >= buffer_size) {
      fwrite(m_buffer.data(), sizeof(uint8_t), m_buffer_index, m_log_file);
      m_buffer_index = 0;
    }

    const auto* byte_array = reinterpret_cast<const uint8_t*>(input);
    std::copy_n(byte_array, size, &m_buffer[m_buffer_index]);
    m_buffer_index += size;
  }

 public:
  packer(const char* path) {
    // Create the log file
    // All the log contents go here
    m_log_file = fopen(path, "wb");
    if (m_log_file == nullptr) {
      throw std::invalid_argument("fopen failed");
    }

    // No fwrite buffering
    setvbuf(m_log_file, nullptr, _IONBF, 0);
  }

  ~packer() {
    flush();
    fclose(m_log_file);
  }

  void flush_log_file() {
    fwrite(m_buffer.data(), sizeof(uint8_t), m_buffer_index, m_log_file);
    m_buffer_index = 0;
    fflush(m_log_file);
  }

  void flush() { flush_log_file(); }

  template <typename T>
  inline void write_arg_value_to_log_file(T&& input) = delete;

  inline void write_arg_value_to_log_file(const char* input) {
    uint16_t size = static_cast<uint16_t>(std::strlen(input));
    buffer_or_write<uint16_t, sizeof(uint16_t)>(&size);
    buffer_or_write(input, size);
  }

  inline void write_arg_value_to_log_file(char input) {
    buffer_or_write<char, sizeof(char)>(&input);
  }

  inline void write_arg_value_to_log_file(bool input) {
    buffer_or_write<bool, sizeof(bool)>(&input);
  }

  inline void write_arg_value_to_log_file(uint8_t input) {
    buffer_or_write<uint8_t, sizeof(uint8_t)>(&input);
  }

  inline void write_arg_value_to_log_file(uint16_t input) {
    buffer_or_write<uint16_t, sizeof(uint16_t)>(&input);
  }

  inline void write_arg_value_to_log_file(uint32_t input) {
    buffer_or_write<uint32_t, sizeof(uint32_t)>(&input);
  }

  inline void write_arg_value_to_log_file(uint64_t input) {
    buffer_or_write<uint64_t, sizeof(uint64_t)>(&input);
  }

  inline void write_arg_value_to_log_file(int8_t input) {
    buffer_or_write<int8_t, sizeof(int8_t)>(&input);
  }

  inline void write_arg_value_to_log_file(int16_t input) {
    buffer_or_write<int16_t, sizeof(int16_t)>(&input);
  }

  inline void write_arg_value_to_log_file(int32_t input) {
    buffer_or_write<int32_t, sizeof(int32_t)>(&input);
  }

  inline void write_arg_value_to_log_file(int64_t input) {
    buffer_or_write<int64_t, sizeof(int64_t)>(&input);
  }

  inline void write_arg_value_to_log_file(float input) {
    buffer_or_write<float, sizeof(float)>(&input);
  }

  inline void write_arg_value_to_log_file(double input) {
    buffer_or_write<double, sizeof(double)>(&input);
  }

  template <typename T>
  constexpr inline void pack_arg(T&& input) {
    if constexpr (!is_specialization<T, constant>{}) {
      write_arg_value_to_log_file(std::forward<T>(input));
    }
  }

  template <class... Args>
  constexpr inline void update_log_file(Args&&... args) {
    ((void)pack_arg(std::forward<Args>(args)), ...);
  }
};

}  // namespace binary_log

#include <iostream>
#include <string>

namespace binary_log {
class binary_log {
  packer m_packer;

 public:
  binary_log(const char* path) : m_packer(path) {}

  ~binary_log() { m_packer.flush(); }

  void flush() { m_packer.flush(); }

  template <class... Args>
  constexpr inline void log_data(Args&&... args) {
    m_packer.update_log_file(std::forward<Args>(args)...);
  }
};

}  // namespace binary_log