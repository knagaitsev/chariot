#include <dev/device.h>
#include <dev/driver.h>
#include <module.h>

#define COMPAT "sifive,fu740-pcie"
#define LOG(...) PFXLOG(BLU COMPAT, __VA_ARGS__)

class FU740PciDriver : public dev::Driver {
 public:
  virtual ~FU740PciDriver(void) {}

  dev::ProbeResult probe(ck::ref<dev::Device> dev) override {
    if (auto mmio = dev->cast<dev::MMIODevice>()) {
      if (mmio->is_compat("sifive,fu740-pcie")) {
        LOG("Found device @%08llx\n", mmio->address());
      }
    }
    return dev::ProbeResult::Ignore;
  };
};


driver_init(COMPAT, FU740PciDriver);