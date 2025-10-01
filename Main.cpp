#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/LayerSurface.hpp>
#include <hyprutils/string/VarList.hpp>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <regex>
#include <format>
#include <unordered_map>
#include <signal.h>
#include <xkbcommon/xkbcommon.h>
#include <atomic>
#include <cstdarg>

extern "C" {
    #include <wayland-server.h>
}

using namespace std::literals;
using namespace Hyprutils::String;

enum class ToggleMode
{
    Hover, Hold, Press
};

// Forward declare PluginState and global_plugin_state before WaybarRegion
struct PluginState;
extern std::unique_ptr<PluginState> global_plugin_state;

struct WaybarRegion
{
    std::string process_name;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    bool visible = false;
    
    // Cache leave area expansion values
    int32_t leave_expand_left = 0;
    int32_t leave_expand_right = 0;
    int32_t leave_expand_up = 0;
    int32_t leave_expand_down = 0;

    void toggle();
    bool is_actually_visible() const;
    
    // Keep the simple method inline
    bool is_in_enter_area(int32_t px, int32_t py) const {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
    
    // Just declare this method - implement it later
    bool is_in_leave_area(int32_t px, int32_t py) const;
    
    // Update leave area cache from global config
    void update_leave_area_cache();
};

struct CommandRegion
{
    std::string enter_command;
    std::string leave_command; // Optional
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    
    bool is_in_area(int32_t px, int32_t py) const {
        return px >= x && px <= x + width && py >= y && py <= y + height;
    }
    
    void execute_enter_command() const {
        if (!enter_command.empty()) {
            std::thread([cmd = enter_command]() {
                std::system(cmd.c_str());
            }).detach();
        }
    }
    
    void execute_leave_command() const {
        if (!leave_command.empty()) {
            std::thread([cmd = leave_command]() {
                std::system(cmd.c_str());
            }).detach();
        }
    }
};

struct PluginState
{
    HANDLE handle;
    bool allow_show_waybar;
    ToggleMode toggle_mode;
    WaybarRegion* hovered_region;
    std::optional<uint32_t> toggle_bind_keycode;
    std::vector<std::vector<WaybarRegion>> monitor_regions;
    std::unordered_map<std::string, uint32_t> keycode_cache;
    
    int hide_delay_ms;
    std::atomic<bool> hide_timer_active{false};
    std::atomic<bool> workspace_timer_active{false};  // New timer for workspace changes
    std::atomic<uint64_t> timer_generation{0};  // Generation counter to invalidate old timers
    std::mutex regions_mutex;
    std::atomic<bool> toggle_in_progress{false};
    
    // Add tracking for previous leave area state
    bool was_in_leave_area_last_frame = false;
    bool was_in_enter_area_last_frame = false;

    // New members for command regions
    std::vector<std::vector<CommandRegion>> monitor_command_regions;
    CommandRegion* hovered_command_region = nullptr;

    PluginState(HANDLE handle) : handle(handle) { reset(); }

    void reset()
    {
        hovered_region = nullptr;
        hovered_command_region = nullptr;
        allow_show_waybar = true;
        toggle_mode = ToggleMode::Hover;
        toggle_bind_keycode = std::nullopt;
        hide_delay_ms = 0;
        was_in_leave_area_last_frame = false;
        was_in_enter_area_last_frame = false;
        
        // Cancel any active timers
        hide_timer_active = false;
        workspace_timer_active = false;
        timer_generation++; // Invalidate all existing timers
    }

    void initialize_timer_thread() {
        // No longer needed - using simple thread-per-timer approach
    }

    void start_timer_thread() {
        // No longer needed - using simple thread-per-timer approach
    }
    
    void start_hide_timer() {
        if (hide_delay_ms <= 0) {
            hide_all_immediate();
            return;
        }
        
        // Cancel any existing timer and increment generation
        hide_timer_active = false;
        uint64_t current_generation = ++timer_generation;
        
        if (global_plugin_state) {
            int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
            if (debug_enabled) {
                FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                if (debug_file) {
                    fprintf(debug_file, "Starting hide timer (%d ms) gen %lu\n", hide_delay_ms, current_generation);
                    fflush(debug_file);
                    fclose(debug_file);
                }
            }
        }
        
        // Start new timer in detached thread
        std::thread([this, current_generation]() {
            hide_timer_active = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(hide_delay_ms));
            
            // Only execute if timer wasn't cancelled and generation is still current
            if (hide_timer_active && timer_generation == current_generation) {
                // Check debug at runtime within lambda
                if (global_plugin_state) {
                    int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
                    if (debug_enabled) {
                        FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                        if (debug_file) {
                            fprintf(debug_file, "Hide timer expired gen %lu - hiding waybar\n", current_generation);
                            fflush(debug_file);
                            fclose(debug_file);
                        }
                    }
                }
                hide_all_immediate();
            } else {
                // Check debug at runtime within lambda
                if (global_plugin_state) {
                    int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
                    if (debug_enabled) {
                        FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                        if (debug_file) {
                            fprintf(debug_file, "Hide timer gen %lu was cancelled (current gen: %lu)\n", current_generation, timer_generation.load());
                            fflush(debug_file);
                            fclose(debug_file);
                        }
                    }
                }
            }
        }).detach();
    }
    
    void cancel_hide_timer_if_active() {
        hide_timer_active = false;
    }
    
    void start_workspace_timer() {
        if (hide_delay_ms <= 0) {
            return;
        }
        
        // Cancel any existing timers and increment generation
        hide_timer_active = false;
        workspace_timer_active = false;
        uint64_t current_generation = ++timer_generation;
        
        if (global_plugin_state) {
            int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
            if (debug_enabled) {
                FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                if (debug_file) {
                    fprintf(debug_file, "Starting workspace timer (1000 ms debounce) gen %lu\n", current_generation);
                    fflush(debug_file);
                    fclose(debug_file);
                }
            }
        }
        
        // Start new workspace timer - this waits before starting the actual hide timer
        std::thread([this, current_generation]() {
            workspace_timer_active = true;
            // Wait 1 second after workspace change before starting hide timer
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            
            // Only start hide timer if workspace timer wasn't cancelled and generation is still current
            if (workspace_timer_active && timer_generation == current_generation) {
                // Check debug at runtime within lambda
                if (global_plugin_state) {
                    int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
                    if (debug_enabled) {
                        FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                        if (debug_file) {
                            fprintf(debug_file, "Workspace timer expired gen %lu - starting hide timer\n", current_generation);
                            fflush(debug_file);
                            fclose(debug_file);
                        }
                    }
                }
                start_hide_timer();
            } else {
                // Check debug at runtime within lambda
                if (global_plugin_state) {
                    int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
                    if (debug_enabled) {
                        FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                        if (debug_file) {
                            fprintf(debug_file, "Workspace timer gen %lu was cancelled (current gen: %lu)\n", current_generation, timer_generation.load());
                            fflush(debug_file);
                            fclose(debug_file);
                        }
                    }
                }
            }
        }).detach();
    }
    
    void cancel_workspace_timer_if_active() {
        workspace_timer_active = false;
    }
    
    // Clean shutdown method for plugin exit
    void shutdown() {
        // Cancel any active timers
        hide_timer_active = false;
        workspace_timer_active = false;
        timer_generation++; // Invalidate all existing timers
    }
    
private:
    void hide_all_immediate() {
        if (!g_pCompositor) {
            return;
        }
        
        // Use a separate try-lock to avoid potential deadlocks
        std::unique_lock<std::mutex> lock(regions_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return; // Skip if we can't get the lock immediately
        }
        
        for (auto& regions : monitor_regions) {
            for (auto& region : regions) {
                if (region.is_actually_visible()) {
                    region.toggle();
                }
            }
        }
    }
};

// Now define the global_plugin_state
std::unique_ptr<PluginState> global_plugin_state;

void try_update_hovered_region_state();

bool is_debug_enabled() {
    if (!global_plugin_state) return false;
    int64_t debug_enabled = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug")->getDataStaticPtr());
    return debug_enabled != 0;
}

void debug_log(const char* format, ...) {
    if (!is_debug_enabled()) return;
    
    FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
    if (debug_file) {
        va_list args;
        va_start(args, format);
        vfprintf(debug_file, format, args);
        va_end(args);
        fflush(debug_file);
        fclose(debug_file);
    }
}

void add_notification(std::string_view message)
{
    // Only write to debug file if debug is enabled
    if (!is_debug_enabled()) return;
    
    FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
    if (debug_file) {
        fprintf(debug_file, "[hypr-hotspots]: %s\n", message.data());
        fflush(debug_file);
        fclose(debug_file);
    }
}

auto fetch_process_pid(std::string_view name) -> pid_t
{
    std::string cmd = "pidof -s " + std::string(name);

    char buf[512];
    auto* pidof = popen(cmd.c_str(), "r");
    fgets(buf, 512, pidof);
    auto pid = strtoul(buf, nullptr, 10);
    pclose(pidof);
    return pid;
}

bool WaybarRegion::is_actually_visible() const
{
    if (!g_pCompositor || g_pCompositor->m_monitors.empty()) {
        return false;
    }
    
    for (auto& monitor : g_pCompositor->m_monitors) {
        if (!monitor || monitor->m_layerSurfaceLayers.size() <= 2) {
            continue;
        }
        
        for (auto& layer : monitor->m_layerSurfaceLayers[2]) {
            if (layer && layer->m_namespace == process_name) {
                return true;
            }
        }
    }
    
    return false;
}

void WaybarRegion::toggle()
{
    if (global_plugin_state->toggle_in_progress.exchange(true)) {
        // Skip - toggle already in progress
        return;
    }
    
    auto pid = fetch_process_pid(process_name);
    
    if (pid <= 0) {
        global_plugin_state->toggle_in_progress = false;
        return;
    }

    kill(pid, SIGUSR1);    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        global_plugin_state->toggle_in_progress = false;
    }).detach();
}

auto keycode_from_name(const std::string& name) -> std::optional<uint32_t>
{
    if (name.empty()) {
        return {};
    }

    if (global_plugin_state->keycode_cache.find(name) != global_plugin_state->keycode_cache.end()) {
        return global_plugin_state->keycode_cache.at(name);
    }

    auto sym = xkb_keysym_from_name(name.data(), XKB_KEYSYM_CASE_INSENSITIVE);

    if (sym == XKB_KEY_NoSymbol) {
        return std::nullopt;
    }

    auto* keymap = g_pSeatManager->m_keyboard->m_xkbKeymap;
    auto* xkb_state = g_pSeatManager->m_keyboard->m_xkbState;
    auto keycode_min = xkb_keymap_min_keycode(keymap);
    auto keycode_max = xkb_keymap_max_keycode(keymap);

    for (auto kc = keycode_min; kc <= keycode_max; ++kc) {
        if (sym == xkb_state_key_get_one_sym(xkb_state, kc)) {
            global_plugin_state->keycode_cache[name] = kc - 8;
            return kc - 8;
        }
    }

    return std::nullopt;
}

APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

void hide_all()
{
    global_plugin_state->start_hide_timer();
}

void update_mouse(int32_t mx, int32_t my)
{
    auto active_monitor = g_pCompositor->getMonitorFromCursor();
    if (!active_monitor) {
        return;
    }

    // Check if there's a fullscreen window on the active monitor
    auto workspace = g_pCompositor->getWorkspaceByID(active_monitor->activeWorkspaceID());
    if (workspace && workspace->m_hasFullscreenWindow) {
        // Don't process hotspots when there's a fullscreen window
        return;
    }

    if (static_cast<size_t>(active_monitor->m_id) >= global_plugin_state->monitor_regions.size()) {
        // Debug: Log monitor region issue
        static bool logged_monitor_issue = false;
        if (!logged_monitor_issue) {
            debug_log("Monitor ID %ld exceeds regions size %zu\n", (long)active_monitor->m_id, global_plugin_state->monitor_regions.size());
            logged_monitor_issue = true;
        }
        return;
    }

    auto& regions = global_plugin_state->monitor_regions[active_monitor->m_id];
    if (regions.empty()) {
        // Debug: Log empty regions
        static bool logged_empty_regions = false;
        if (!logged_empty_regions) {
            debug_log("No regions configured for monitor %ld\n", (long)active_monitor->m_id);
            logged_empty_regions = true;
        }
        return;
    }

    auto monitor_bounds = active_monitor->logicalBox();
    auto monitor_local_x = mx - static_cast<int32_t>(monitor_bounds.pos().x);
    auto monitor_local_y = my - static_cast<int32_t>(monitor_bounds.pos().y);

    WaybarRegion* new_region = nullptr;
    bool was_in_leave_area = global_plugin_state->was_in_leave_area_last_frame;
    bool was_in_enter_area = global_plugin_state->was_in_enter_area_last_frame;
    bool is_in_leave_area = false;
    bool is_in_enter_area = false;

    // Find which region we're in (if any)
    for (auto& region : regions) {
        if (region.is_in_enter_area(monitor_local_x, monitor_local_y)) {
            new_region = &region;
            is_in_enter_area = true;
            is_in_leave_area = true; // Enter area is also part of leave area
            break;
        }
        else if (region.is_in_leave_area(monitor_local_x, monitor_local_y)) {
            new_region = &region;
            is_in_leave_area = true;
            is_in_enter_area = false;
            break;
        }
    }

    global_plugin_state->hovered_region = new_region;

    // State transition logic
    if (is_in_enter_area && !was_in_enter_area) {
        // Entered enter area - show waybar
        global_plugin_state->cancel_hide_timer_if_active();
        if (new_region) {
            new_region->toggle();
        }
    }
    else if (!is_in_leave_area && was_in_leave_area) {
        // Left leave area completely - start hide timer
        hide_all();
    }
    else if (is_in_leave_area) {
        // In any part of leave area - cancel timer to prevent hiding
        global_plugin_state->cancel_hide_timer_if_active();
    }

    global_plugin_state->was_in_leave_area_last_frame = is_in_leave_area;
    global_plugin_state->was_in_enter_area_last_frame = is_in_enter_area;
}

void on_config_pre_reload()
{
    std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
    for (auto& regions : global_plugin_state->monitor_regions) {
        regions.clear();
    }
    for (auto& command_regions : global_plugin_state->monitor_command_regions) {
        command_regions.clear();
    }
}

void on_config_reloaded()
{
    // Don't call reset() here - it stops the timer thread
    // Just reset the state variables we need
    global_plugin_state->hovered_region = nullptr;
    global_plugin_state->hovered_command_region = nullptr;
    global_plugin_state->was_in_leave_area_last_frame = false;
    global_plugin_state->was_in_enter_area_last_frame = false;

    std::string toggle_bind_str = static_cast<Hyprlang::STRING>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:toggle_bind")->getDataStaticPtr());
    std::string_view toggle_mode_str = static_cast<Hyprlang::STRING>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:toggle_mode")->getDataStaticPtr());
    int64_t hide_delay = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:hide_delay")->getDataStaticPtr());

    global_plugin_state->toggle_bind_keycode = keycode_from_name(toggle_bind_str);
    global_plugin_state->hide_delay_ms = static_cast<int>(hide_delay);

    if (global_plugin_state->toggle_bind_keycode.has_value()) {
        global_plugin_state->allow_show_waybar = false;

        if (toggle_mode_str == "hold") {
            global_plugin_state->toggle_mode = ToggleMode::Hold;
        }
        else if (toggle_mode_str == "press") {
            global_plugin_state->toggle_mode = ToggleMode::Press;
        }
        else {
            // add_notification("Invalid value for toggle_mode");
        }
    }
    else {
        global_plugin_state->allow_show_waybar = true;
        global_plugin_state->toggle_mode = ToggleMode::Hover;
        if (!toggle_bind_str.empty()) {
            // add_notification("Invalid key name for toggle_bind");
        }
    }

    // Update leave area cache for all existing regions
    std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
    for (auto& regions : global_plugin_state->monitor_regions) {
        for (auto& region : regions) {
            region.update_leave_area_cache();
        }
    }

    // Timer thread no longer needed - using simple detached threads
}

Hyprlang::CParseResult register_waybar_region(const char* cmd, const char* v)
{
    auto result = Hyprlang::CParseResult{};
    auto value = std::string{ v };
    auto vars = CVarList{ value };

    if (vars.size() < 5) {
        add_notification("Invalid number of parameters passed to hypr-waybar-region");
        result.setError("[hypr-hotspots]: Invalid number of parameters passed to hypr-waybar-region");
        return result;
    }

    auto monitor = g_pCompositor->getMonitorFromName(vars[0]);

    if (!monitor) {
        add_notification("Monitor not found");
        result.setError("[hypr-hotspots]: Failed to find monitor.");
        return result;
    }

    auto region = WaybarRegion{};

    try {
        region.x = std::stoi(vars[1]);
        region.y = std::stoi(vars[2]);
        region.width = std::stoi(vars[3]);
        region.height = std::stoi(vars[4]);
    }
    catch (std::exception& ex) {
        add_notification("Failed to parse `hypr-waybar-region` parameters as integers.");
        result.setError("[hypr-hotspots]: Failed to parse parameters as integers.");
        return result;
    }

    region.process_name = "waybar";

    if (vars.size() == 6) {
        region.process_name = vars[5];
    }

    // Update the leave area cache for this region
    region.update_leave_area_cache();

    {
        std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
        global_plugin_state->monitor_regions[monitor->m_id].emplace_back(region);
    }
    return result;
}

Hyprlang::CParseResult register_command_region(const char* cmd, const char* v)
{
    auto result = Hyprlang::CParseResult{};
    auto value = std::string{ v };
    
    // Manual parsing to handle commands with spaces
    auto comma_pos = std::vector<size_t>{};
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == ',') {
            comma_pos.push_back(i);
        }
    }
    
    if (comma_pos.size() < 5) {
        add_notification("Invalid number of parameters passed to hypr-command-region. Expected at least 6.");
        result.setError("[hypr-hotspots]: Invalid number of parameters passed to hypr-command-region");
        return result;
    }
    
    // Extract parameters manually
    auto monitor_name = value.substr(0, comma_pos[0]);
    auto x_str = value.substr(comma_pos[0] + 1, comma_pos[1] - comma_pos[0] - 1);
    auto y_str = value.substr(comma_pos[1] + 1, comma_pos[2] - comma_pos[1] - 1);
    auto width_str = value.substr(comma_pos[2] + 1, comma_pos[3] - comma_pos[2] - 1);
    auto height_str = value.substr(comma_pos[3] + 1, comma_pos[4] - comma_pos[3] - 1);
    
    std::string enter_command;
    std::string leave_command;
    
    // Handle enter and leave commands
    if (comma_pos.size() == 5) {
        // Only enter command provided
        enter_command = value.substr(comma_pos[4] + 1);
    } else if (comma_pos.size() >= 6) {
        // Both enter and leave commands provided
        enter_command = value.substr(comma_pos[4] + 1, comma_pos[5] - comma_pos[4] - 1);
        leave_command = value.substr(comma_pos[5] + 1);
    }
    
    // Trim whitespace
    monitor_name = std::regex_replace(monitor_name, std::regex("^\\s+|\\s+$"), "");
    x_str = std::regex_replace(x_str, std::regex("^\\s+|\\s+$"), "");
    y_str = std::regex_replace(y_str, std::regex("^\\s+|\\s+$"), "");
    width_str = std::regex_replace(width_str, std::regex("^\\s+|\\s+$"), "");
    height_str = std::regex_replace(height_str, std::regex("^\\s+|\\s+$"), "");
    enter_command = std::regex_replace(enter_command, std::regex("^\\s+|\\s+$"), "");
    leave_command = std::regex_replace(leave_command, std::regex("^\\s+|\\s+$"), "");

    auto monitor = g_pCompositor->getMonitorFromName(monitor_name);
    if (!monitor) {
        add_notification("Monitor not found for command region");
        result.setError("[hypr-hotspots]: Failed to find monitor.");
        return result;
    }

    auto region = CommandRegion{};

    try {
        region.x = std::stoi(x_str);
        region.y = std::stoi(y_str);
        region.width = std::stoi(width_str);
        region.height = std::stoi(height_str);
    }
    catch (std::exception& ex) {
        add_notification("Failed to parse `hypr-command-region` parameters as integers.");
        result.setError("[hypr-hotspots]: Failed to parse parameters as integers.");
        return result;
    }

    region.enter_command = enter_command;
    region.leave_command = leave_command;

    {
        std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
        if (global_plugin_state->monitor_command_regions.size() <= static_cast<size_t>(monitor->m_id)) {
            global_plugin_state->monitor_command_regions.resize(monitor->m_id + 1);
        }
        global_plugin_state->monitor_command_regions[monitor->m_id].emplace_back(region);
    }
    return result;
}

void try_update_hovered_region_state()
{
    if (!g_pCompositor || !global_plugin_state || !global_plugin_state->hovered_region) {
        return;
    }
    
    if (global_plugin_state->toggle_in_progress) {
        return;
    }
    
    bool actually_visible = global_plugin_state->hovered_region->is_actually_visible();
    
    global_plugin_state->hovered_region->visible = actually_visible;
    
    if (global_plugin_state->allow_show_waybar && !actually_visible) {
        global_plugin_state->hovered_region->toggle();
    }
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    // Write to debug file immediately
    FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "w");
    if (debug_file) {
        fprintf(debug_file, "PLUGIN_INIT called\n");
        fflush(debug_file);
        fclose(debug_file);
    }
    
    // Try stderr output too
    fprintf(stderr, "[hypr-hotspots] PLUGIN_INIT called\n");
    fflush(stderr);
    
    try {
        // Remove or comment out this version check:
        // if (HASH != GIT_COMMIT_HASH) {
        //     throw std::runtime_error("[hypr-hotspots] Version mismatch");
        // }
        
        // Don't call reset() in constructor - manually initialize instead
        global_plugin_state = std::make_unique<PluginState>(handle);
        
        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Created PluginState\n");
            fflush(debug_file);
            fclose(debug_file);
        }
        
        // Manually initialize state variables
        global_plugin_state->hovered_region = nullptr;
        global_plugin_state->hovered_command_region = nullptr;
        global_plugin_state->allow_show_waybar = true;
        global_plugin_state->toggle_mode = ToggleMode::Hover;
        global_plugin_state->toggle_bind_keycode = std::nullopt;
        global_plugin_state->hide_delay_ms = 0;
        global_plugin_state->was_in_leave_area_last_frame = false;

        // Check if compositor is available
        if (!g_pCompositor) {
            throw std::runtime_error("[hypr-hotspots] Compositor not available");
        }
        
        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Compositor available\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        // Initialize monitor regions safely
        if (!g_pCompositor->m_monitors.empty()) {
            global_plugin_state->monitor_regions.resize(g_pCompositor->m_monitors.size());
            global_plugin_state->monitor_command_regions.resize(g_pCompositor->m_monitors.size());
        }

        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "About to add config values\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        // Add config values
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:toggle_bind", Hyprlang::STRING{""});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:toggle_mode", Hyprlang::STRING{"hold"});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:hide_delay", Hyprlang::INT{0});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_left", Hyprlang::INT{0});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_right", Hyprlang::INT{0});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_up", Hyprlang::INT{0});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_down", Hyprlang::INT{0});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:show_on_workspace_change", Hyprlang::INT{1});
        HyprlandAPI::addConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:debug", Hyprlang::INT{0});

        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Added config values\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        // Register config keywords with the new nested structure:
        HyprlandAPI::addConfigKeyword(global_plugin_state->handle, "hypr-waybar-region", register_waybar_region, Hyprlang::SHandlerOptions{});
        HyprlandAPI::addConfigKeyword(global_plugin_state->handle, "hypr-command-region", register_command_region, Hyprlang::SHandlerOptions{});

        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Added config keywords\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        // Register callbacks with throttling
        static auto mouse_move = HyprlandAPI::registerCallbackDynamic(global_plugin_state->handle, "mouseMove", [](void* handle, SCallbackInfo& callback_info, std::any value) {
            if (!global_plugin_state) return;
            
            // Throttle mouse updates to every 16ms (~60fps) to prevent system sluggishness
            static auto last_update = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update).count() < 16) {
                return; // Skip this update
            }
            last_update = now;
            
            auto pos = std::any_cast<const Vector2D>(value);
            update_mouse(static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y));
        });

        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Registered mouse callback\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        static auto pre_config_reload = HyprlandAPI::registerCallbackDynamic(global_plugin_state->handle, "preConfigReload", [&](void* self, SCallbackInfo& info, std::any data) { 
            if (global_plugin_state) on_config_pre_reload(); 
        });
        
        static auto config_reloaded = HyprlandAPI::registerCallbackDynamic(global_plugin_state->handle, "configReloaded", [&](void* self, SCallbackInfo& info, std::any data) { 
            if (global_plugin_state) on_config_reloaded(); 
        });

        static auto workspace_changed = HyprlandAPI::registerCallbackDynamic(global_plugin_state->handle, "workspace", [](void* handle, SCallbackInfo& callback_info, std::any value) {
            if (!global_plugin_state) return;
            
            int64_t show_on_workspace_change = *static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:show_on_workspace_change")->getDataStaticPtr());
            
            if (show_on_workspace_change && !global_plugin_state->monitor_regions.empty()) {
                if (global_plugin_state->hide_delay_ms > 0) {
                    if (is_debug_enabled()) {
                        FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                        if (debug_file) {
                            fprintf(debug_file, "Workspace changed - canceling timers and showing waybar\n");
                            fflush(debug_file);
                            fclose(debug_file);
                        }
                    }
                    
                    // Cancel any existing timers to prevent hiding during workspace switching
                    global_plugin_state->cancel_hide_timer_if_active();
                    global_plugin_state->cancel_workspace_timer_if_active();
                    
                    // Show all waybar regions that aren't currently visible
                    std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
                    for (auto& regions : global_plugin_state->monitor_regions) {
                        for (auto& region : regions) {
                            if (!region.is_actually_visible()) {
                                region.toggle();
                            }
                        }
                    }
                    
                    // Only start workspace timer if mouse is NOT in a leave area
                    if (!global_plugin_state->was_in_leave_area_last_frame) {
                        // Start debounced workspace timer - only starts hide timer after 1 second of no workspace changes
                        global_plugin_state->start_workspace_timer();
                        
                        if (is_debug_enabled()) {
                            FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                            if (debug_file) {
                                fprintf(debug_file, "Started workspace timer (mouse not in leave area)\n");
                                fflush(debug_file);
                                fclose(debug_file);
                            }
                        }
                    } else {
                        if (is_debug_enabled()) {
                            FILE* debug_file = fopen("/tmp/hypr-hotspots.log", "a");
                            if (debug_file) {
                                fprintf(debug_file, "Mouse in leave area - not starting workspace timer\n");
                                fflush(debug_file);
                                fclose(debug_file);
                            }
                        }
                    }
                }
            }
        });

        static auto key_press = HyprlandAPI::registerCallbackDynamic(global_plugin_state->handle, "keyPress", [](void* handle, SCallbackInfo& callback_info, std::any value) {
            if (!global_plugin_state || !global_plugin_state->toggle_bind_keycode.has_value()) {
                return;
            }
            
            auto storage = std::any_cast<std::unordered_map<std::string, std::any>>(value);
            auto key_event = std::any_cast<IKeyboard::SKeyEvent>(storage.at("event"));

            if (key_event.keycode == global_plugin_state->toggle_bind_keycode) {
                switch (global_plugin_state->toggle_mode) {
                case ToggleMode::Hold:
                    global_plugin_state->allow_show_waybar = key_event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
                    break;
                case ToggleMode::Press:
                    if (key_event.state == WL_KEYBOARD_KEY_STATE_RELEASED) {
                        global_plugin_state->allow_show_waybar = !global_plugin_state->allow_show_waybar;
                    }
                    break;
                default:
                    break;
                }

                try_update_hovered_region_state();
            }
        });

        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Registered all callbacks\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        // Start timer thread last
        global_plugin_state->initialize_timer_thread();

        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Started timer thread\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        // Debug: Add notification to confirm plugin loaded
        add_notification("Plugin loaded successfully!");
        
        // Write to debug file
        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Plugin initialization complete\n");
            fflush(debug_file);
            fclose(debug_file);
        }

        return { "hypr-hotspots", "hyprland hotspots plugin", "x140x1n", "1.0" };
    }
    catch (const std::exception& e) {
        // Log the exception before cleanup
        debug_file = fopen("/tmp/hypr-hotspots.log", "a");
        if (debug_file) {
            fprintf(debug_file, "Exception caught: %s\n", e.what());
            fflush(debug_file);
            fclose(debug_file);
        }
        
        fprintf(stderr, "[hypr-hotspots] Exception: %s\n", e.what());
        fflush(stderr);
        
        // Don't call add_notification here - it might cause another crash
        // Just clean up and let the plugin fail to load
        global_plugin_state.reset();
        throw;
    }
}

APICALL EXPORT void PLUGIN_EXIT()
{
    if (global_plugin_state) {
        global_plugin_state->shutdown();
    }
}

// Now implement the is_in_leave_area method after PluginState is fully defined
bool WaybarRegion::is_in_leave_area(int32_t px, int32_t py) const {
    // Use cached values instead of expensive config calls
    int32_t leave_x = x - leave_expand_left;
    int32_t leave_y = y - leave_expand_up;
    int32_t leave_width = width + leave_expand_left + leave_expand_right;
    int32_t leave_height = height + leave_expand_up + leave_expand_down;
    
    return px >= leave_x && px <= leave_x + leave_width && py >= leave_y && py <= leave_y + leave_height;
}

void WaybarRegion::update_leave_area_cache() {
    // Fix these to use hypr_hotspots instead of hypr_waybar
    leave_expand_left = static_cast<int32_t>(*static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_left")->getDataStaticPtr()));
    leave_expand_right = static_cast<int32_t>(*static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_right")->getDataStaticPtr()));
    leave_expand_up = static_cast<int32_t>(*static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_up")->getDataStaticPtr()));
    leave_expand_down = static_cast<int32_t>(*static_cast<const Hyprlang::INT*>(*HyprlandAPI::getConfigValue(global_plugin_state->handle, "plugin:hypr_hotspots:leave_expand_down")->getDataStaticPtr()));
}
