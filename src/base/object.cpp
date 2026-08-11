#include "base/object.h"

#include <cxxabi.h>

#include <cstdlib>
#include <memory>

namespace latch {

std::string demangle(const char* name) {
  int status = -4;
  std::unique_ptr<char, void (*)(void*)> res{abi::__cxa_demangle(name, NULL, NULL, &status), std::free};

  return (status == 0) ? res.get() : name;
}

}
