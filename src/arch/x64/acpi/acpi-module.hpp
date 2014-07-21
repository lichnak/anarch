#ifndef __ANARCH_X64_ACPI_MODULE_HPP__
#define __ANARCH_X64_ACPI_MODULE_HPP__

#include <ansa/module>
#include "acpi-root.hpp"

namespace anarch {

namespace x64 {

class AcpiModule : public ansa::Module {
public:
  static void InitGlobal();
  static AcpiModule & GetGlobal();
  
  AcpiRoot & GetRoot();
  
protected:
  virtual ansa::DepList GetDependencies();
  virtual void Initialize();
  
private:
  AcpiRoot * root;
};

}

}

#endif