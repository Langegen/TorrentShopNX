#pragma once

#include <borealis.hpp>
#include "../utils/file_ops.h"
#include <vector>
#include <string>
#include <unordered_set>
#include <memory>

namespace ui {

class FileManagerView;

class FileManagerCell : public brls::RecyclerCell {
public:
    FileManagerCell();
    ~FileManagerCell() override = default;
    static FileManagerCell* create();

    void setSelectedVisual(bool selected);
    void clearRegisteredActions() {
        while (!this->getActions().empty()) {
            this->unregisterAction(this->getActions().front()->getIdentifier());
        }
    }

    size_t rowIndex = 0;
    FileManagerView* parentView = nullptr;

    BRLS_BIND(brls::Box,   accentBar, "accentBar");
    BRLS_BIND(brls::Label, icon,      "icon");
    BRLS_BIND(brls::Label, name,      "name");
    BRLS_BIND(brls::Label, size,      "size");
    BRLS_BIND(brls::Label, date,      "date");
};

class FileManagerView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("file_manager_view.xml");

    FileManagerView(const std::string& initialPath = "", const std::string& focusChild = "", const std::string& rootDir = "");
    ~FileManagerView() override = default;

    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

    void navigateTo(const std::string& path, const std::string& focusChild = "");
    void navigateUp();
    void refresh(const std::string& focusChild = "");

    void setFocusedRow(int row) { currentFocusedRow_ = row; }
    void toggleSelection(size_t index);
    void toggleSelectionOnCell(size_t index, FileManagerCell* cell);
    void selectAll();
    void clearSelection();

    void showActionsMenu();
    void showArchiveDialog(const util::FileItem& item);
    void showInstallDialog(const util::FileItem& item);
    void promptDeleteSourceFile(const std::string& filePath, const std::string& fileName);
    void openTextViewer(const std::string& path, const std::string& name = "");
    void showDeleteConfirmDialog();
    void showNewFolderDialog();
    void showRenameDialog(const util::FileItem& item);
    void pasteClipboard();

    BRLS_BIND(brls::Label,         currentPath,  "currentPath");
    BRLS_BIND(brls::Label,         spaceInfo,    "spaceInfo");
    BRLS_BIND(brls::Box,           selectionBar, "selectionBar");
    BRLS_BIND(brls::Label,         selectionText,"selectionText");
    BRLS_BIND(brls::Label,         selectionHint,"selectionHint");
    BRLS_BIND(brls::RecyclerFrame, recycler,     "recycler");
    BRLS_BIND(brls::Label,         emptyLabel,   "emptyLabel");

private:
    std::string currentDir_;
    std::string rootDir_;
    std::string initialFocusChild_;
    std::vector<util::FileItem> items_;
    bool hasParentDir_ = false;
    int currentFocusedRow_ = -1;

    // Set of selected item paths
    std::unordered_set<std::string> selectedPaths_;

    void updateSelectionBar();
    void updateSpaceInfo();

    class FileManagerDataSource : public brls::RecyclerDataSource {
    public:
        FileManagerDataSource(FileManagerView* parent) : parent_(parent) {}
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 56.0f; }

    private:
        FileManagerView* parent_;
    };
};

} // namespace ui
