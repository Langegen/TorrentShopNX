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
    void setCompactMode(bool compact);
    void clearRegisteredActions() {
        while (!this->getActions().empty()) {
            this->unregisterAction(this->getActions().front()->getIdentifier());
        }
    }

    size_t rowIndex = 0;
    int panelIndex = 0;
    FileManagerView* parentView = nullptr;

    BRLS_BIND(brls::Box,   cellRoot,  "cellRoot");
    BRLS_BIND(brls::Box,   accentBar, "accentBar");
    BRLS_BIND(brls::Label, icon,      "icon");
    BRLS_BIND(brls::Label, name,      "name");
    BRLS_BIND(brls::Label, size,      "size");
    BRLS_BIND(brls::Label, date,      "date");

private:
    bool isCompact_ = false;
    bool compactConfigured_ = false;
};

struct PanelState {
    int index = 0;
    std::string currentDir;
    std::string rootDir;
    std::vector<util::FileItem> items;
    std::unordered_set<std::string> selectedPaths;
    int currentFocusedRow = -1;
    bool hasParentDir = false;

    // UI elements bound or assigned
    brls::Box* container = nullptr;
    brls::Label* panelIcon = nullptr;
    brls::Label* currentPath = nullptr;
    brls::Label* spaceInfo = nullptr;
    brls::Box* selectionBar = nullptr;
    brls::Label* selectionText = nullptr;
    brls::Label* selectionHint = nullptr;
    brls::Box* colHeaders = nullptr;
    brls::Label* colSize = nullptr;
    brls::Label* colDate = nullptr;
    brls::RecyclerFrame* recycler = nullptr;
    brls::Label* emptyLabel = nullptr;
};

class FileManagerView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("file_manager_view.xml");

    FileManagerView(const std::string& initialPath = "", const std::string& focusChild = "", const std::string& rootDir = "");
    ~FileManagerView() override = default;

    void onContentAvailable() override;
    void willAppear(bool resetState = false) override;

    void navigateTo(int panelIdx, const std::string& path, const std::string& focusChild = "");
    void navigateTo(const std::string& path, const std::string& focusChild = "") {
        navigateTo(activePanel_, path, focusChild);
    }
    void navigateUp(int panelIdx = -1);
    void refresh(int panelIdx = -1, const std::string& focusChild = "");

    void setFocusedRow(int panelIdx, int row);
    void setFocusedRow(int row) { setFocusedRow(activePanel_, row); }
    int getActivePanel() const { return activePanel_; }
    void setActivePanel(int panelIdx);
    void switchActivePanel(int panelIdx);
    void toggleSplitMode();

    void toggleSelection(int panelIdx, size_t index);
    void toggleSelection(size_t index) { toggleSelection(activePanel_, index); }
    void toggleSelectionOnCell(int panelIdx, size_t index, FileManagerCell* cell);
    void toggleSelectionOnCell(size_t index, FileManagerCell* cell) { toggleSelectionOnCell(activePanel_, index, cell); }
    void selectAll(int panelIdx = -1);
    void clearSelection(int panelIdx = -1);

    void showActionsMenu();
    void showArchiveDialog(const util::FileItem& item);
    void showCreateArchiveDialog(const std::vector<std::string>& targets);
    void showInstallDialog(const util::FileItem& item);
    void promptDeleteSourceFile(const std::string& filePath, const std::string& fileName);
    void openTextViewer(const std::string& path, const std::string& name = "");
    void showDeleteConfirmDialog();
    void showNewFolderDialog();
    void showRenameDialog(const util::FileItem& item);
    void pasteClipboard();
    void copyToOppositePanel();
    void moveToOppositePanel();

    // Bound Views (Left Panel / Legacy IDs)
    BRLS_BIND(brls::Label,         currentPath,        "currentPath");
    BRLS_BIND(brls::Label,         spaceInfo,          "spaceInfo");
    BRLS_BIND(brls::Box,           selectionBar,       "selectionBar");
    BRLS_BIND(brls::Label,         selectionText,      "selectionText");
    BRLS_BIND(brls::Label,         selectionHint,      "selectionHint");
    BRLS_BIND(brls::RecyclerFrame, recycler,           "recycler");
    BRLS_BIND(brls::Label,         emptyLabel,         "emptyLabel");
    BRLS_BIND(brls::Box,           leftPanelBox,       "leftPanelBox");
    BRLS_BIND(brls::Label,         leftPanelIcon,      "leftPanelIcon");
    BRLS_BIND(brls::Box,           leftColHeaders,     "leftColHeaders");
    BRLS_BIND(brls::Label,         leftColSize,        "leftColSize");
    BRLS_BIND(brls::Label,         leftColDate,        "leftColDate");

    // Bound Views (Right Panel)
    BRLS_BIND(brls::Box,           rightPanelBox,      "rightPanelBox");
    BRLS_BIND(brls::Label,         rightPanelIcon,     "rightPanelIcon");
    BRLS_BIND(brls::Label,         currentPathRight,   "currentPathRight");
    BRLS_BIND(brls::Label,         spaceInfoRight,     "spaceInfoRight");
    BRLS_BIND(brls::Box,           selectionBarRight,  "selectionBarRight");
    BRLS_BIND(brls::Label,         selectionTextRight, "selectionTextRight");
    BRLS_BIND(brls::Label,         selectionHintRight, "selectionHintRight");
    BRLS_BIND(brls::Box,           rightColHeaders,    "rightColHeaders");
    BRLS_BIND(brls::Label,         rightColSize,       "rightColSize");
    BRLS_BIND(brls::Label,         rightColDate,       "rightColDate");
    BRLS_BIND(brls::RecyclerFrame, recyclerRight,      "recyclerRight");
    BRLS_BIND(brls::Label,         emptyLabelRight,    "emptyLabelRight");

    // Bound Views (General)
    BRLS_BIND(brls::Box,           panelDivider,       "panelDivider");
    BRLS_BIND(brls::Label,         splitNavHint,       "splitNavHint");
    BRLS_BIND(brls::Label,         splitToggleHint,    "splitToggleHint");

private:
    PanelState panels_[2];
    int activePanel_ = 0;
    bool isSplitMode_ = false;
    std::string initialFocusChild_;

    void initPanelBindings();
    void updateSelectionBar(int panelIdx);
    void updateSpaceInfo(int panelIdx);
    void updateActivePanelVisuals();
    void updateCompactMode();
    void updateSplitHints();

    class FileManagerDataSource : public brls::RecyclerDataSource {
    public:
        FileManagerDataSource(FileManagerView* parent, int panelIndex) : parent_(parent), panelIndex_(panelIndex) {}
        int numberOfSections(brls::RecyclerFrame* recycler) override { return 1; }
        int numberOfRows(brls::RecyclerFrame* recycler, int section) override;
        brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override;
        float heightForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override { return 56.0f; }

    private:
        FileManagerView* parent_;
        int panelIndex_ = 0;
    };
};

} // namespace ui
