#ifndef __ANARCH_X64_COMMON_HPP__
#define __ANARCH_X64_COMMON_HPP__

#include <anarch/stdint>

namespace anarch {

namespace x64 {

/**
 * @critical
 */
void OutB(uint16_t port, uint8_t byte);

/**
 * @critical
 */
void CpuID(uint32_t eax, uint32_t * ebx, uint32_t * edx, uint32_t * ecx);

/**
 * @critical
 */
uint64_t ReadMsr(uint32_t cell);

/**
 * @critical
 */
void WriteMsr(uint32_t cell, uint64_t value);

}

}

#endif
