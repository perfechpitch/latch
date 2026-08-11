#ifndef _LATCH_OBJECT_
#define _LATCH_OBJECT_

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "base/runtime.h"
#include "base/time_stamp.h"

namespace latch {

std::string demangle(const char* name);
#define OBJ_NAME demangle(typeid(*this).name())

class Object {
 public:
  Object() : obj_id(0){};
  virtual ~Object() = default;

  virtual uint64_t RegisterName(const std::string objectNmae) {
    obj_id = RT::GetModulePool().CreateID(objectNmae);
    return obj_id;
  }

  virtual uint64_t RegisterPid(uint64_t pid) {
    LOGCHECK(obj_id != 0, "Must CreateID first!");
    RT::GetModulePool().SetPid(obj_id, pid);
    return obj_id;
  }

  virtual uint64_t RegisterId(std::string objectNmae, uint64_t pid) {
    obj_id = RT::GetModulePool().CreateID(pid, objectNmae);
    return obj_id;
  }

  std::string Name() { return RT::GetModulePool().GetName(obj_id); }
  uint64_t Id() { return obj_id; }

  virtual void Dump() {}

 protected:
  uint64_t obj_id;
};

}

#endif
