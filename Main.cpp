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
    std::thread hide_timer_thread;
    std::atomic<bool> stop_timer_thread{false};
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    std::atomic<bool> timer_reset_requested{false};
    std::mutex regions_mutex;
    std::atomic<bool> toggle_in_progress{false};
    
    // Add tracking for previous leave area state
    bool was_in_leave_area_last_frame = false;

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
        
        // Only stop timer thread if it's already running
        if (hide_timer_thread.joinable()) {
            stop_timer_thread = true;
            timer_cv.notify_all();
            hide_timer_thread.join();
        }
        stop_timer_thread = false;
        timer_reset_requested = false;
        // Don't start timer thread here - do it after full initialization
    }

    void initialize_timer_thread() {
        start_timer_thread();
    }

    void start_timer_thread() {
        global_plugin_state->hide_timer_thread = std::thread([&]() {
            while (true) {
                // ONLY CHANGE: Add this null check to prevent crash on unload
                if (!global_plugin_state) {
                    break;
                }
                
                // REST OF YOUR ORIGINAL CODE EXACTLY AS IT WAS
                std::unique_lock<std::mutex> lock(global_plugin_state->timer_mutex);
                
                if (global_plugin_state->stop_timer_thread) {
                    break;
                }
                
                global_plugin_state->timer_cv.wait(lock, []() {
                    return global_plugin_state->timer_reset_requested || global_plugin_state->stop_timer_thread; 
                });
                
                if (global_plugin_state->stop_timer_thread) {
                    break;
                }
                
                global_plugin_state->timer_reset_requested = false;
                
                if (global_plugin_state->hide_delay_ms <= 0) {
                    continue;
                }
                
                bool timeout = !global_plugin_state->timer_cv.wait_for(lock, 
                    std::chrono::milliseconds(global_plugin_state->hide_delay_ms), 
                    []() { return global_plugin_state->timer_reset_requested || global_plugin_state->stop_timer_thread; });
                
                if (timeout && !global_plugin_state->timer_reset_requested && !global_plugin_state->stop_timer_thread) {
                    lock.unlock();
                    hide_all_immediate();
                    lock.lock();
                }
            }
        });
    }
    
    void start_hide_timer() {
        if (hide_delay_ms <= 0) {
            hide_all_immediate();
            return;
        }
        
        std::lock_guard<std::mutex> lock(timer_mutex);
        timer_reset_requested = true;
        timer_cv.notify_one();
    }
    
    void cancel_hide_timer_if_active() {
        std::lock_guard<std::mutex> lock(timer_mutex);
        timer_reset_requested = false;
        timer_cv.notify_one();
    }
    
    // Clean shutdown method for plugin exit
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            stop_timer_thread = true;
            timer_cv.notify_all();
        }
        if (hide_timer_thread.joinable()) {
            hide_timer_thread.join();
        }
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

void add_notification(std::string_view message)
{
    HyprlandAPI::addNotification(
        global_plugin_state->handle,
        std::format("[hypr-hotspots]: {}", message),
        CHyprColor(1.0, 0.2, 0.2, 1.0), 5000);
}

auto fetch_process_pid(std::string_view name) -> pid_t
{
    auto cmd = std::format("pidof -s {}", name);

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
        return;
    }
    
    kill(fetch_process_pid(process_name), SIGUSR1);
    
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        global_plugin_state->toggle_in_progress = false;
    }).detach();
}

auto keycode_from_name(const std::string& name) -> std::optional<uint32_t>
{
    if (name.empty()) {
        return {};
    }

    if (global_plugin_state->keycode_cache.contains(name)) {
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
        return; // Early return if no monitor found
    }
    
    // Check if monitor ID is valid
    if (static_cast<size_t>(active_monitor->m_id) >= global_plugin_state->monitor_regions.size()) {
        return; // Early return if monitor ID is out of bounds
    }
    
    auto& regions = global_plugin_state->monitor_regions[active_monitor->m_id];
    
    // Get command regions - use non-const access
    std::vector<CommandRegion>* command_regions_ptr = nullptr;
    if (static_cast<size_t>(active_monitor->m_id) < global_plugin_state->monitor_command_regions.size()) {
        command_regions_ptr = &global_plugin_state->monitor_command_regions[active_monitor->m_id];
    }

    // Handle waybar regions (existing logic)
    auto monitor_bounds = active_monitor->logicalBox();
    auto monitor_local_x = mx - static_cast<int32_t>(monitor_bounds.pos().x);
    auto monitor_local_y = my - static_cast<int32_t>(monitor_bounds.pos().y);

    // Remove unused variable warning
    WaybarRegion* new_region = nullptr;
    bool was_in_leave_area = global_plugin_state->was_in_leave_area_last_frame;
    bool is_in_leave_area = false;
    bool is_in_enter_area = false;

    // Find which region we're in (if any) - check enter area first
    for (auto& region : regions) {
        if (region.is_in_enter_area(monitor_local_x, monitor_local_y)) {
            new_region = &region;
            is_in_enter_area = true;
            is_in_leave_area = true; // Enter area is part of leave area
            break;
        }
        else if (region.is_in_leave_area(monitor_local_x, monitor_local_y)) {
            new_region = &region;
            is_in_leave_area = true;
            is_in_enter_area = false; // In leave area but not enter area
            break;
        }
    }

    global_plugin_state->hovered_region = new_region;

    // Handle waybar show/hide logic
    if (is_in_leave_area && !was_in_leave_area) {
        // Entered leave area (from completely outside)
        global_plugin_state->cancel_hide_timer_if_active();
        
        // Only show waybar if we're in the enter area
        if (is_in_enter_area && global_plugin_state->allow_show_waybar && new_region && !new_region->is_actually_visible()) {
            new_region->toggle();
        }
    }
    else if (!is_in_leave_area && was_in_leave_area) {
        // Left leave area completely
        hide_all();
    }
    else if (is_in_leave_area && was_in_leave_area) {
        // Still in leave area - cancel any hide timer that might be running
        global_plugin_state->cancel_hide_timer_if_active();
        
        // If we moved from leave-only area to enter area, show waybar
        if (is_in_enter_area && global_plugin_state->allow_show_waybar && new_region && !new_region->is_actually_visible()) {
            new_region->toggle();
        }
    }

    // Update state for next frame
    global_plugin_state->was_in_leave_area_last_frame = is_in_leave_area;

    // Handle command regions (unchanged)
    auto* old_command_region = global_plugin_state->hovered_command_region;
    CommandRegion* new_command_region = nullptr;

    if (command_regions_ptr && !command_regions_ptr->empty()) {
        for (auto& region : *command_regions_ptr) {
            if (region.is_in_area(monitor_local_x, monitor_local_y)) {
                new_command_region = &region;
                break;
            }
        }
    }

    global_plugin_state->hovered_command_region = new_command_region;

    // Command region transitions
    if (new_command_region && !old_command_region) {
        new_command_region->execute_enter_command();
    }
    else if (!new_command_region && old_command_region) {
        old_command_region->execute_leave_command();
    }
    else if (new_command_region && old_command_region && new_command_region != old_command_region) {
        old_command_region->execute_leave_command();
        new_command_region->execute_enter_command();
    }
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
            add_notification(std::format("Invalid value '{}' for toggle_mode", toggle_mode_str));
        }
    }
    else {
        global_plugin_state->allow_show_waybar = true;
        global_plugin_state->toggle_mode = ToggleMode::Hover;
        if (!toggle_bind_str.empty()) {
            add_notification(std::format("Invalid key name `{}` for toggle_bind", toggle_bind_str));
        }
    }

    // Update leave area cache for all existing regions
    std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
    for (auto& regions : global_plugin_state->monitor_regions) {
        for (auto& region : regions) {
            region.update_leave_area_cache();
        }
    }

    // At the end, ensure timer thread is running
    if (!global_plugin_state->hide_timer_thread.joinable()) {
        global_plugin_state->stop_timer_thread = false;
        global_plugin_state->timer_reset_requested = false;
        global_plugin_state->initialize_timer_thread();
    }
}

Hyprlang::CParseResult register_waybar_region(const char* cmd, const char* v)
{
    auto result = Hyprlang::CParseResult{};
    auto value = std::string{ v };
    auto vars = CVarList{ value };

    if (vars.size() < 5) {
        add_notification(std::format("Invalid number of parameters passed to hypr-waybar-region. Expected at least 5 but got {}", vars.size()));
        result.setError("[hypr-hotspots]: Invalid number of parameters passed to hypr-waybar-region");
        return result;
    }

    auto monitor = g_pCompositor->getMonitorFromName(vars[0]);

    if (!monitor) {
        add_notification(std::format("No monitor with name {} was found.", vars[0]));
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
        add_notification(std::format("No monitor with name {} was found.", monitor_name));
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

void show_all_waybars_temporarily() {
    if (!g_pCompositor || !global_plugin_state) {
        return;
    }
    
    // Show all configured waybar regions that aren't currently visible
    std::lock_guard<std::mutex> lock(global_plugin_state->regions_mutex);
    for (auto& regions : global_plugin_state->monitor_regions) {
        for (auto& region : regions) {
            if (!region.is_actually_visible()) {
                region.toggle();
            }
        }
    }
    
    // Start the hide timer to hide them after the configured delay
    global_plugin_state->start_hide_timer();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    try {
        // Remove or comment out this version check:
        // if (HASH != GIT_COMMIT_HASH) {
        //     throw std::runtime_error("[hypr-hotspots] Version mismatch");
        // }
        
        // Don't call reset() in constructor - manually initialize instead
        global_plugin_state = std::make_unique<PluginState>(handle);
        
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

        // Initialize monitor regions safely
        if (!g_pCompositor->m_monitors.empty()) {
            global_plugin_state->monitor_regions.resize(g_pCompositor->m_monitors.size());
            global_plugin_state->monitor_command_regions.resize(g_pCompositor->m_monitors.size());
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

        // Register config keywords with the new nested structure:
        HyprlandAPI::addConfigKeyword(global_plugin_state->handle, "hypr-waybar-region", register_waybar_region, Hyprlang::SHandlerOptions{});
        HyprlandAPI::addConfigKeyword(global_plugin_state->handle, "hypr-command-region", register_command_region, Hyprlang::SHandlerOptions{});

        // Register callbacks
        static auto mouse_move = HyprlandAPI::registerCallbackDynamic(global_plugin_state->handle, "mouseMove", [](void* handle, SCallbackInfo& callback_info, std::any value) {
            if (!global_plugin_state) return;
            auto pos = std::any_cast<const Vector2D>(value);
            update_mouse(static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y));
        });

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
                    global_plugin_state->cancel_hide_timer_if_active();
                    show_all_waybars_temporarily();
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

        // Start timer thread last
        global_plugin_state->initialize_timer_thread();

        return { "hypr-hotspots", "hyprland hotspots plugin", "x140x1n", "1.0" };
    }
    catch (const std::exception& e) {
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
