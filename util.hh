#pragma once

#include <source_location>
#include <stdexcept>

#include <fmt/format.h>

template <typename ...T>
[[noreturn]]
static void _die(const std::source_location & s, fmt::format_string<T...> fmt, T && ... args){
  auto str = fmt::format(fmt, args...);
  auto location = fmt::format("{}:{}:{}", s.file_name(), s.line(), s.column());
  if(errno != 0){
    auto system = fmt::format("Last system error is {} (error {})", std::strerror(errno), errno);
    fmt::print(stderr, "{} ERROR: {}. {}.\n", location, str, system);
  } else {
    fmt::print(stderr, "{} ERROR: {}. (No system error)\n", location, str);
  }
  throw std::runtime_error(str);
}

#define die(fmt, ...) (_die(std::source_location::current(), fmt __VA_OPT__(,) __VA_ARGS__ ))

static void display_buffer_hex(unsigned char *buffer, unsigned size, unsigned limit=128)
{
  unsigned i, j, k;
  
  for (i=0; i<size; i+=16) {
    fmt::print("\n  {:08x}  ", i);
    for(j=0,k=0; k<16; j++,k++) {
      if (i+j < size) {
        fmt::print("{:02x}", buffer[i+j]);
      } else {
        fmt::print("  ");
      }
      fmt::print(" ");
    }
    fmt::print(" ");
    for(j=0,k=0; k<16; j++,k++) {
      if (i+j < size) {
        if ((buffer[i+j] < 32) || (buffer[i+j] > 126)) {
          fmt::print(".");
        } else {
          fmt::print("{:c}", buffer[i+j]);
        }
      }
    }
    if(i >= limit){
      fmt::print("\n  ... skipped {} bytes ...", size-i);
      break;
    }
  }
  fmt::print("\n");
}
