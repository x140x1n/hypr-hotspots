# hypr-hotspots

A Hyprland plugin that creates invisible screen regions for waybar control and command execution based on mouse movement.

## Overview

hypr-hotspots allows you to define screen regions that respond to mouse movement by:

- Showing/hiding waybar instances when hovering over specific areas
- Executing custom commands when entering or leaving regions
- Supporting multiple monitors with independent region configurations
- Providing hysteresis behavior to prevent unwanted flickering

## Features

### Waybar Control
- Hover-based waybar visibility toggle
- Separate enter/leave areas to prevent flickering
- Configurable hide delays
- Support for multiple waybar instances with custom process names
- Optional key-based toggle modes (hold/press)

### Command Regions
- Execute commands on mouse enter/leave events
- Background command execution (non-blocking)
- Independent operation from waybar regions
- Configurable region sizes and positions

### Configuration Options
- Per-monitor region setup
- Precise pixel positioning
- Expandable leave areas for better UX
- Multiple toggle modes (hover, key-hold, key-press)

## Installation

```bash
hyprpm add https://github.com/x140x1n/hypr-hotspots.git
hyprpm enable hypr-hotspots
```

## Configuration

hypr-hotspots uses a two-part configuration structure:

1. **Plugin settings** go inside the plugin block
2. **Region definitions** are top-level keywords

```haskell
plugin {
  hypr_hotspots {
    // Plugin configuration options here
    hide_delay = 500
    leave_expand_down = 20
  }
}

// Region definitions (top-level)
hypr-waybar-region = eDP-1, 0, 0, 1920, 30
hypr-command-region = eDP-1, 0, 0, 100, 100, rofi -show drun
```

## Configuration Options

### Plugin Settings (inside plugin block)

#### toggle_bind
Specifies a key for manual region toggling when mouse is over the region.

See [xkbcommon keysyms](https://github.com/xkbcommon/libxkbcommon/blob/master/include/xkbcommon/xkbcommon-keysyms.h) for key names (use the part after `XKB_KEY_`).

**Example:** `toggle_bind = Caps_Lock`

#### toggle_mode
Controls how the toggle key behaves:
- `hold` - Region only active while key is held and mouse is over region (default)
- `press` - Key acts as on/off switch for normal hover behavior

**Example:** `toggle_mode = press`

#### hide_delay
Delay in milliseconds before hiding waybar after leaving the region.

**Default:** `0`
**Example:** `hide_delay = 500`

#### Leave Area Expansion
Expands the "leave area" beyond the enter area to create hysteresis behavior:

- `leave_expand_left` - Expand left (pixels)
- `leave_expand_right` - Expand right (pixels)  
- `leave_expand_up` - Expand up (pixels)
- `leave_expand_down` - Expand down (pixels)

**Default:** `0` for all directions

```haskell
leave_expand_left = 10
leave_expand_right = 50
leave_expand_up = 5
leave_expand_down = 15
```

#### show_on_workspace_change
Controls whether waybar should be shown temporarily after workspace changes.

When enabled, waybar will appear after switching workspaces and hide after the configured `hide_delay`. This only works when:
- Waybar regions are configured
- `hide_delay` is greater than 0

**Default:** `1` (enabled)
**Example:** `show_on_workspace_change = 0` (to disable)

### Region Definitions (top-level)

#### hypr-waybar-region
Defines a screen area that toggles a waybar process when interacted with.

**Usage:** `hypr-waybar-region = MONITOR, X, Y, WIDTH, HEIGHT, [WAYBAR_PROCESS_NAME]`

Parameters use monitor-local coordinates (0,0 = top-left corner of monitor).

To name a waybar process:
```bash
exec -a process_name waybar
```

**Example:** `hypr-waybar-region = DP-1, 0, 0, 200, 60, waybar-workspace-dp-1`

#### hypr-command-region
Defines a region that executes commands on mouse enter/leave events.

**Usage:** `hypr-command-region = MONITOR, X, Y, WIDTH, HEIGHT, ENTER_COMMAND, [LEAVE_COMMAND]`

**Examples:**
```haskell
// Launch application menu on corner hover
hypr-command-region = eDP-1, 0, 0, 100, 100, rofi -show drun

// Workspace switching with edge regions
hypr-command-region = eDP-1, 1900, 400, 20, 200, hyprctl dispatch workspace +1

// Notification with both enter and leave commands
hypr-command-region = DP-1, 1820, 980, 100, 100, notify-send "Entered", notify-send "Left"
```

## Example Configurations

### Basic Auto-hiding Waybar
```haskell
plugin {
  hypr_hotspots {
    hide_delay = 500
    leave_expand_down = 20
  }
}

hypr-waybar-region = eDP-1, 0, 0, 1920, 30
```

### Basic Auto-hiding Waybar with Workspace Preview
```haskell
plugin {
  hypr_hotspots {
    hide_delay = 1000
    show_on_workspace_change = 1
    leave_expand_down = 20
  }
}

hypr-waybar-region = eDP-1, 0, 0, 1920, 30
```

### Multi-Monitor with Key Toggle
```haskell
plugin {
  hypr_hotspots {
    toggle_bind = Super_L
    toggle_mode = hold
    hide_delay = 300
    leave_expand_left = 15
    leave_expand_right = 15
    leave_expand_down = 25
  }
}

hypr-waybar-region = DP-1, 0, 0, 200, 60, waybar-workspace-dp-1
hypr-waybar-region = DP-1, 1180, 0, 200, 60, waybar-clock-dp-1
hypr-waybar-region = DP-2, 0, 0, 1920, 40, waybar-secondary
```

### Edge Navigation Setup
```haskell
plugin {
  hypr_hotspots {
    hide_delay = 200
    leave_expand_up = 30
  }
}

// Left/right edge workspace switching
hypr-command-region = eDP-1, 0, 200, 5, 600, hyprctl dispatch workspace -1
hypr-command-region = eDP-1, 1915, 200, 5, 600, hyprctl dispatch workspace +1

// Corner launcher
hypr-command-region = eDP-1, 0, 0, 150, 150, rofi -show drun

// Bottom edge waybar
hypr-waybar-region = eDP-1, 0, 1070, 1920, 10
```

## Usage Notes

### Multiple Waybar Instances
Launch waybar with custom process names:
```bash
exec-once = exec -a waybar-top waybar -c ~/.config/waybar/top.json
exec-once = exec -a waybar-bottom waybar -c ~/.config/waybar/bottom.json
```

Reference in configuration:
```haskell
hypr-waybar-region = DP-1, 0, 0, 1920, 30, waybar-top
hypr-waybar-region = DP-1, 0, 1050, 1920, 30, waybar-bottom
```

### Preventing Accidental Activation
Use leave area expansion and hide delays:
```haskell
plugin {
  hypr_hotspots {
    hide_delay = 300
    leave_expand_down = 20
    leave_expand_left = 10
    leave_expand_right = 10
  }
}
```

### Command Region Guidelines
- Use small regions (5-20px) for edge-based triggers
- Use larger regions (100px+) for corner actions
- Test commands in terminal before adding to config
- Commands execute in background threads automatically

## Common Use Cases

**Auto-hiding panel:**
```haskell
plugin {
  hypr_hotspots {
    hide_delay = 300
    leave_expand_down = 20
  }
}

hypr-waybar-region = eDP-1, 0, 0, 1920, 5
```

**Corner launcher:**
```haskell
hypr-command-region = eDP-1, 0, 0, 100, 100, rofi -show drun
```

**Edge workspace switching:**
```haskell
hypr-command-region = eDP-1, 0, 200, 5, 600, hyprctl dispatch workspace -1
hypr-command-region = eDP-1, 1915, 200, 5, 600, hyprctl dispatch workspace +1
```

## Technical Notes

### Why Two Configuration Formats?

Due to Hyprland's configuration system limitations:
- **Simple values** (numbers, strings, booleans) can be nested inside `plugin { hypr_hotspots { } }`
- **Complex parsers** (region definitions) must be registered as top-level keywords

This is a constraint of Hyprland's config parser, not a design choice of this plugin.
