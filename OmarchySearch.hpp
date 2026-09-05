#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace OmarchySearch {

enum class EItemKind : uint8_t {
    App,
    Action,      // Omarchy menu action / toggle / command
    Menu,        // Omarchy submenu (drills down into route)
    Back,        // Back to parent menu
    Agent,       // AI Coding Agent chat launcher (/grok, /agy, etc.)
    Calculator,  // Instant arithmetic calculation
    File,        // File search result
    Provider,    // D-Bus search provider
};

struct SSearchItem {
    EItemKind   kind = EItemKind::Action;
    std::string id;
    std::string name;
    std::string description;
    std::string icon;
    std::string category;     // "Apps", "Omarchy Menu", "AI Agent", "Calculator", "Files", "Settings"
    std::string actionCommand;// shell command or executable
    std::string filePath;     // absolute path for files
    uint64_t    fileSizeBytes = 0;
    std::string fileSizeStr;
    std::string fileModifiedStr;
    std::string fileTypeStr;  // e.g. "PDF Document", "C++ Source", "PNG Image"
    bool        canQuickLook = false;
};

// Lifecycle
void init();
void shutdown();

// Keystroke query across all Omarchy providers
void query(const std::string& text);

// Provider queries
bool isAgentQuery(const std::string& query);
std::vector<SSearchItem> searchAgents(const std::string& query);
std::optional<SSearchItem> evaluateCalculator(const std::string& query);
std::vector<SSearchItem> searchMenu(const std::string& query, size_t limit = 10);
std::vector<SSearchItem> fileResults();

// Menu hierarchy navigation
void navigateMenu(const std::string& routeId);
bool navigateMenuBack();
bool canNavigateMenuBack();
std::string menuCurrentRoute();
std::string menuCurrentRouteTitle();
void resetMenuRoute();

// Activate an item (runs command, launches agent, copies calc, or opens file)
bool activate(const SSearchItem& item);

// Quick Look (Sushi) state and actions
bool isSushiActive();
void toggleSushi(const std::string& filePath);
void closeSushi();
void updateSushiIfActive(const std::string& filePath);
bool isOverviewSushiWindow(const std::string& windowClass, uint32_t pid = 0);
std::vector<SSearchItem> recentFiles();

} // namespace OmarchySearch
