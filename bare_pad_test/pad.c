// 膨胀实验用的纯填充数据（C 编译单元，支持 GNU 范围初始化器）
// 只占 flash 的 .rodata，不参与任何逻辑
#include <stdint.h>

#define PAD_KB 130

const uint8_t padData[PAD_KB * 1024] = { [0 ... (PAD_KB * 1024 - 1)] = 0xA5 };
