#include "protocol/FileName.h"

namespace p2p {

bool sanitizeFileName(const std::string& input, std::string* safeName, std::string* error) {
    if (!safeName) return false;
    std::string name = input;
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") {
        if (error) *error = "文件名无效";
        return false;
    }
    if (name.find(':') != std::string::npos) {
        if (error) *error = "文件名不能包含盘符";
        return false;
    }
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*' || ch == '/' ||
            ch == '\\') {
            if (error) *error = "文件名包含非法字符";
            return false;
        }
    }
    if (name.size() > 180) {
        if (error) *error = "文件名过长";
        return false;
    }
    *safeName = name;
    return true;
}

}  // namespace p2p
