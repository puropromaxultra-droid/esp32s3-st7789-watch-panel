#pragma once
#include <stdint.h>

// 注意：结构体必须放在头文件里 include 进来。
// .ino 的自动原型插在第一个函数定义之前，若 struct 定义在 .ino 里会出现在原型之后 -> 编译报错。
struct BenchCase {
  const char *name;
  const uint8_t *start;
  const uint8_t *end;
  int sr;   // 标称采样率
  int br;   // 标称比特率 kbps
  int ch;   // 标称声道数
};
