#pragma once

#include <string>

namespace p2p {

// 只保留文件名本身，拒绝目录穿越、盘符和非法字符。
bool sanitizeFileName(const std::string& input, std::string* safeName, std::string* error);

}  // namespace p2p
