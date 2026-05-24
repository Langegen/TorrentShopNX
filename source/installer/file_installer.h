#pragma once

#include <string>
#include <vector>

namespace installer {

class FileInstaller {
public:
    std::vector<std::string> scanDownloads(const std::string& path);
    bool installFile(const std::string& path);
};

} // namespace installer
