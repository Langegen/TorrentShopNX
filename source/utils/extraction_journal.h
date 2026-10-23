#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace util {

// Remembers which files and directories an extraction created, so a failed or
// cancelled extraction can remove its leftovers. Only paths that did not exist
// before are recorded: files we overwrote and pre-existing folders (sdmc:/switch)
// are never deleted. Not thread-safe; one journal per extraction.
class ExtractionJournal {
public:
    // Creates `dir` and any missing parents, recording the ones that were new.
    void createDirectories(const std::string& dir);

    // Call right before opening `path` for writing. Records it if it is new.
    void noteFile(const std::string& path);

    // Deletes recorded files, then recorded directories if they are empty.
    // Clears the journal. Stops early if the app is exiting.
    void rollback();

    size_t fileCount() const { return files_.size(); }
    size_t dirCount() const { return dirs_.size(); }

private:
    std::vector<std::string> files_;
    std::unordered_set<std::string> fileSet_;
    std::vector<std::string> dirs_;            // in creation order (parents first)
    std::unordered_set<std::string> knownDirs_; // existing or created
};

} // namespace util
