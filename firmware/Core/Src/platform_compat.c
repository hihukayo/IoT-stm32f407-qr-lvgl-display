/**
 * @file    platform_compat.c
 * @brief   工具链兼容层：为 ARM Compiler 6 + Keil 链接环境补齐缺失的编译器运行时符号。
 * @details
 *          LVGL v9.6 的内置内存分配器 `lv_tlsf.c` 用 `__builtin_clz()` 计算 log2，
 *          而 ARM Compiler 6 在部分优化级别下会把它下降为对运行时符号 `__clz` 的调用。
 *          Keil 使用 armlink + ARM C 库（不是 clang 驱动自带的 compiler-rt），
 *          因此该符号不存在，链接会报：
 *
 *          @verbatim
 *          Error: L6218E: Undefined symbol __clz (referred from lv_tlsf.o)
 *          @endverbatim
 *
 *          本文件提供等价实现。语义与 ARM 内建 `__clz` 一致：
 *          返回 32 位值最高有效位之前的零位数；输入 0 时返回 32。
 *
 *          @note 该函数只在 TLSF 计算内存块 log2 时调用，调用频率低，
 *                用移位循环实现不影响运行性能，也无需内联汇编。
 */

#include <stdint.h>

/**
 * @brief  统计 32 位无符号整数前导零的个数（Count Leading Zeros）。
 * @param  value 待统计的值
 * @return 前导零个数，范围 0..32；`value == 0` 时返回 32
 */
int __clz(unsigned int value)
{
    int count = 0;
    uint32_t v = (uint32_t)value;

    if (v == 0U) {
        return 32;
    }

    while ((v & 0x80000000U) == 0U) {
        v <<= 1;
        count++;
    }

    return count;
}
