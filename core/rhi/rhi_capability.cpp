#include "rhi/rhi_capability.h"

namespace rd {

const char* DeviceCaps::name(Capability c) {
  static const char* kNames[] = {
#define RD_CAPABILITY(name) #name,
#include "rhi/rhi_constants.inc.h"
  };
  return kNames[static_cast<uint32_t>(c)];
}

} // namespace rd
