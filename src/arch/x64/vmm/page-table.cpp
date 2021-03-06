#include "page-table.hpp"
#include "tlb.hpp"
#include <anarch/api/panic>
#include <anarch/critical>
#include <anarch/easy-map>
#include <ansa/cstring>

namespace anarch {

namespace x64 {

int PageTable::CalcDepth(size_t size) {
  switch (size) {
    case 0x1000:
      return 3;
    case 0x200000:
      return 2;
    case 0x40000000:
      return 1;
    default:
      break;
  }
  Panic("PageTable::CalcDepth() - unknown page size");
  return -1;
}

uint64_t PageTable::CalcMask(size_t pageSize, bool kernel,
                             const MemoryMap::Attributes & attributes) {
  return 1 | (pageSize == 0x1000 ? 0 : 0x80) | (kernel ? 0x100 : 0x4)
    | (attributes.executable ? 0 : (uint64_t)1 << 63)
    | (attributes.writable ? 2 : 0);
}

PageTable::PageTable(Allocator & a, PhysAddr p) : allocator(&a), pml4(p) {
}

PageTable::PageTable(Allocator & a) : allocator(&a) {
}

void PageTable::SetPml4(PhysAddr _pml4) {
  pml4 = _pml4;
}

PhysAddr PageTable::GetPml4() {
  return pml4;
}

bool PageTable::IsSet() {
  PhysAddr cr3;
  __asm__("mov %%cr3, %0" : "=r" (cr3));
  return cr3 == GetPml4();
}

void PageTable::SetAllocator(Allocator & a) {
  allocator = &a;
}

Allocator & PageTable::GetAllocator() {
  return *allocator;
}

int PageTable::Walk(VirtAddr addr, uint64_t & entry, size_t * size) {
  AssertNoncritical();
  assert(IsSet());
  
  int depth;
  for (depth = 0; depth < 4; ++depth) {
    entry = GetTableEntry(addr, depth);
    if (entry & 0x80 || !(entry & 1)) {
      break;
    }
  }
  
  if (depth == 4 || (entry & 0x80)) {
    if (depth == 4) depth = 3;
    if (size) *size = 0x1000UL << (27 - 9 * depth);
    return depth;
  }
  
  if (size) *size = 0x1000UL << (27 - 9 * depth);
  return -1;
}

bool PageTable::Set(VirtAddr addr, uint64_t entry, uint64_t parentMask,
                    int theDepth, bool * overwrote) {
  AssertNoncritical();
  assert(IsSet());
  assert(theDepth >= 0 && theDepth < 4);
  assert(!(addr % (0x1000L << (27 - 9 * theDepth))));
  
  for (int depth = 0; depth < theDepth; ++depth) {
    uint64_t & nextPage = GetTableEntry(addr, depth);
    if (!(nextPage & 1)) {
      // allocate a new page
      PhysAddr fresh;
      if (!GetAllocator().Alloc(fresh, 0x1000, 0x1000)) {
        Panic("PageTable::Set() - allocation failed");
      }
      nextPage = (uint64_t)fresh | parentMask;
      ansa::Bzero((void *)GetTableStart(addr, depth + 1), 0x1000);
    } else if (nextPage & 0x80) {
      // fail if we hit a huge TLB entry
      return false;
    } else {
      nextPage |= parentMask;
    }
  }
  
  uint64_t & entryRef = GetTableEntry(addr, theDepth);
  if (overwrote) *overwrote = (entryRef != 0);
  entryRef = entry;
  return true;
}

bool PageTable::Unset(VirtAddr addr) {
  AssertNoncritical();
  assert(IsSet());
  assert(!(addr & 0xfff));
  
  int depth;
  for (depth = 0; depth < 3; ++depth) {
    uint64_t & entry = GetTableEntry(addr, depth);
    if (entry & 0x80) break;
    if (!(entry & 1)) {
      Panic("PageTable::Unset() - no mapping found");
    }
  }
  
  GetTableEntry(addr, depth) = 0;
  
  for (; depth > 0; --depth) {
    // check if the whole table is empty
    uint64_t * tablePtr = GetTableStart(addr, depth);
    for (int i = 0; i < 0x200; ++i) {
      // if there's an entry, just return
      if (tablePtr[i]) return true;
    }
    
    // unmap this table, invalidate the fractal map, and free it
    uint64_t & parentEntry = GetTableEntry(addr, depth - 1);
    PhysAddr memory = parentEntry & 0x7FFFFFFFFFFFF000UL;
    parentEntry = 0;
    Tlb::GetGlobal().DistributeInvlpg((VirtAddr)tablePtr, 0x1000);
    GetAllocator().Free(memory);
  }
  
  return true;
}

void PageTable::SetList(VirtAddr virt, uint64_t phys, MemoryMap::Size size,
                        uint64_t parentMask, bool * overwrote) {
  AssertNoncritical();
  assert(IsSet());
  int depth = CalcDepth(size.pageSize);
  VirtAddr curVirt = virt;
  PhysAddr curPhys = phys;
  
  for (size_t i = 0; i < size.pageCount; i++) {
    if (!Set(curVirt, curPhys, parentMask, depth, overwrote)) {
      Panic("PageTable::SetList() - Set() failed");
    }
    
    // if we overwrote something, set `overwrote` to NULL so that we won't set
    // it back to false in subsequent iterations of this loop
    if (overwrote) {
      if (*overwrote) overwrote = NULL;
    }
    
    curPhys += size.pageSize;
    curVirt += size.pageSize;
  }
}

bool PageTable::Read(PhysAddr * physOut, MemoryMap::Attributes * attrOut,
                     size_t * sizeOut, VirtAddr addr) {
  AssertNoncritical();
  assert(IsSet());
  
  uint64_t entry;
  int depth = Walk(addr, entry, sizeOut);
  if (depth < 0) return false;
  assert(entry != 0);  
  assert(depth >= 2);
  
  if (physOut) {
    if (depth == 3) {
      *physOut = entry & 0x7ffffffffffff000UL;
    } else {
      *physOut = entry & 0x7fffffffffe00000UL;
    }
  }
  
  if (attrOut) {
    attrOut->executable = (entry & (1UL << 63)) == 0;
    attrOut->writable = (entry & 2) != 0;
  
    // TODO: set cachable flag properly here
    attrOut->cachable = true;
  }
  
  return true;
}

void PageTable::FreeTable(int start) {
  AssertNoncritical();
  assert(!IsSet());
  FreeTableRecursive(pml4, 0, start, 0x1ff);
}

// PRIVATE //

uint64_t & PageTable::GetTableEntry(VirtAddr address, int depth) {
  assert(depth >= 0 && depth < 4);
  // remove canonical bits on address
  address &= 0xFFFFFFFFFFFFUL;
  
  if (depth == 3) {
    VirtAddr inner = (address / 0x200) & (~(uint64_t)7);
    return *(uint64_t *)(0xFFFFFF8000000000UL + inner);
  } else if (depth == 2) {
    VirtAddr inner = (address / 0x40000) & (~(uint64_t)7);
    return *(uint64_t *)(0xFFFFFFFFC0000000 + inner);
  } else if (depth == 1) {
    VirtAddr inner = (address / 0x8000000) & (~(uint64_t)7);
    return *(uint64_t *)(0xFFFFFFFFFFE00000 + inner);
  } else {
    VirtAddr inner = (address / 0x1000000000UL) & (~(uint64_t)7);
    return *(uint64_t *)(0xFFFFFFFFFFFFF000 + inner);
  }
}

uint64_t * PageTable::GetTableStart(VirtAddr address, int depth) {
  VirtAddr entryPtr = (VirtAddr)&GetTableEntry(address, depth);
  entryPtr &= ~(VirtAddr)0xfff;
  return (uint64_t *)entryPtr;
}

void PageTable::FreeTableRecursive(PhysAddr addr, int depth, int start,
                                   int end) {
  // it doesn't matter if addresses are canonical because our fractal helpers
  // uncanonicalize addresses anyway
  AssertNoncritical();
  if (depth != 3) {
    // free sub-tables
    EasyMap map(addr, 0x1000);
    uint64_t * entries = (uint64_t *)map.GetStart();
    for (int i = start; i < end; ++i) {
      uint64_t entry = entries[i];
      if (!entry || entry & 0x80) continue;
      PhysAddr phys = entry & 0x7FFFFFFFFFFFF000UL;
      FreeTableRecursive(phys, depth + 1);
    }
  }
  GetAllocator().Free(addr);
}

}

}
