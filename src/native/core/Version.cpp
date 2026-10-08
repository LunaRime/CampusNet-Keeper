#include "core/Version.h"

// 本翻译单元存在的唯一目的：让 cnk_core 不是纯头文件库。
// CMake 无法为一个只有头文件的 STATIC 库确定链接语言，即便补上
// LINKER_LANGUAGE 也不会产出 .lib，依赖目标会以 LNK1181 失败。
//
// VersionString() 刻意保持 inline 定义在头文件里，不放在本文件 ——
// 这样它不依赖本文件是否被列进源列表，将来重组构建也不会出现未定义符号。
