#include "file_installer.h"

#include <dirent.h>
#include <sys/stat.h>

namespace installer {

std::vector<std::string> FileInstaller::scanDownloads(const std::string& path) {
    std::vector<std::string> files;
    DIR* dir = opendir(path.c_str());
    if (!dir) return files;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = path + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back(full);
        }
    }
    closedir(dir);
    return files;
}

bool FileInstaller::installFile(const std::string& path) {
    // Placeholder for local file install logic.
    (void)path;
    return true;
}

} // namespace installer
