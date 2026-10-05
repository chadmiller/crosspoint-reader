# Provisioning Configuration System

## Overview

You probably don't need this! If you know you do, read on...

The provisioning system allows *advanced users* to configure a CrossPoint Reader device at boot time by placing a `/provision.json` file on the SD card root. Settings are applied automatically on first boot, applied items are removed from the file, and the file is deleted once all items are successfully applied.

## File Location

Place the provisioning config file at the **SD card root**: `/provision.json`

## How It Works

```
Boot sequence:
  1. Device loads existing settings, WiFi, and OPDS stores
  2. Checks for /provision.json on SD card
  3. For each setting in the file:
     ✓ Apply setting → remove from file
     ✗ Error → leave in file for retry on next boot
  4. Save remaining config (or delete if empty)
  5. Continue normal boot
```

## Configuration Structure

The file is JSON with these main sections:

### `wifi` — WiFi Networks
```json
{
  "wifi": {
    "networks": [
      {"ssid": "Home WiFi", "password": "plaintext-password"}
    ],
    "downloadFolder": "/Books",
    "filenameFormat": "author-title"
  }
}
```

- **Max networks**: 8
- **Passwords**: Plaintext in file, obfuscated with device MAC at storage
- Duplicates are rejected; re-adding fails silently (left in file)

### `opds` — OPDS Server Definitions
```json
{
  "opds": {
    "servers": [
      {
        "name": "Project Gutenberg",
        "url": "https://www.gutenberg.org/ebooks/search.opds/"
      },
      {
        "name": "Private Server",
        "url": "https://private-opds.example.com/catalog",
        "username": "user@example.com",
        "password": "plaintext-password"
      }
    ]
  }
}
```

- **Max servers**: 8
- **Passwords**: Plaintext in file, obfuscated with device MAC at storage
- **username & password**: Both optional; omit or leave empty if not required (defaults to empty strings)

### `display` — Display Settings
```json
{
  "display": {
    "nightMode": false,
    "orientation": "portrait",
    "sleepScreen": "dark",
    "sleepScreenCoverFit": "fit",
    "sleepScreenCoverFilter": "none",
    "fadingCompensation": false
  }
}
```

### `text` — Text Rendering (Most Important)
```json
{
  "text": {
    "fontFamily": "noto-serif",
    "fontSize": 14,
    "lineSpacing": "normal",
    "paragraphSpacing": true,
    "paragraphAlignment": "justified",
    "paragraphIndent": 2,
    "wordSpacing": 100,
    "characterSpacing": 0,
    "antiAliasing": true,
    "hyphenation": false,
    "screenMargin": 5
  }
}
```

### `statusbar` — Status Bar Configuration
```json
{
  "statusbar": {
    "showChapterPageCount": true,
    "showBookProgressPercent": true,
    "progressBar": "hidden",
    "progressBarThickness": "normal",
    "title": "chapter",
    "showBattery": true,
    "hideBatteryPercent": "never",
    "clock": "hidden",
    "clockFormat": "24h",
    "clockDst": "auto",
    "xtcMode": "hidden"
  }
}
```

### `ui` — UI Theme
```json
{
  "ui": {
    "theme": "lyra",
    "language": "EN"
  }
}
```

### `input` — Button & Touch Input
```json
{
  "input": {
    "sideButtons": "prev-next",
    "frontButtons": {
      "back": 0,
      "confirm": 1,
      "left": 2,
      "right": 3
    },
    "powerButton": {
      "shortPress": "ignore",
      "doubleClickFrontlight": true,
      "longPressPageTurn": "off",
      "returnFromFootnotes": true
    },
    "touchGestures": {
      "enabled": true,
      "pageForward": "swipe-only",
      "pageBackward": "swipe-only",
      "showReaderMenu": "tap"
    },
    "tiltPageTurn": "off"
  }
}
```

### `reader` — EPUB Reader Behavior
```json
{
  "reader": {
    "autoSleepMinutes": 10,
    "refreshFrequency": 15,
    "imageRendering": "display",
    "focusReading": false,
    "useEmbeddedStyles": true,
    "menuStyle": "list",
    "quickResume": "never"
  }
}
```

### `library` — File Browser & Library Settings
```json
{
  "library": {
    "useMetadata": true,
    "showHiddenFiles": false,
    "removeFinishedFromRecents": false,
    "moveFinishedToReadFolder": false,
    "shortBackToFileBrowser": false
  }
}
```

### `frontlight` — Hardware Frontlight (if available)
```json
{
  "frontlight": {
    "brightness": 60,
    "warmth": 50,
    "enabled": false,
    "restoreOnWake": true
  }
}
```

## Error Handling

| Scenario | Behavior |
|----------|----------|
| **Parsing error** | File left in place; logged |
| **Duplicate WiFi SSID** | Entry left in file; logged as skipped |
| **Out-of-range enum** | Clamped to valid range; entry removed from file |
| **Out-of-range value** | Clamped to min/max; entry removed from file |
| **Unknown font family** | Assumed to be SD card font; left in file if registry doesn't match |
| **Oversized string** | Truncated with warning; entry removed from file |
| **Missing required field** | Skipped; entry left in file |
| **File becomes empty** | Deleted after final save |

## Logging

All provisioning operations are logged under the module `PROV`. Examples:

```
[PROV] Loaded provisioning config from /provision.json
[PROV] WiFi: Added network 'Home WiFi'
[PROV] OPDS: Added server 'Gutenberg'
[PROV] Text: fontSize = 14 pt
[PROV] All settings applied; deleted /provision.json
```

## Example Configuration

See `provision-example.json` for a complete example with all available options and inline documentation.

## Security Considerations

- **Plaintext passwords in file**: Only obfuscated with device MAC after loading. Do not share files between devices.
- **MAC-tied obfuscation**: Moving credentials to a different device requires re-entry or manual re-obfuscation.
- **No encryption at rest**: File is readable but not cryptographically secure. Secure the file on any shared/public storage.

## Workflow

### First Boot (New Device)
1. User creates `/provision.json` on SD card
2. Device boots, loads settings from file
3. Each setting applied and removed from file
4. File deleted when complete
5. Device reboots with all settings in place

### Retry on Next Boot (Failed Settings)
1. User inserts SD card with partially-applied `/provision.json`
2. Device boots, re-tries remaining settings
3. Fixed settings removed from file
4. File deleted when complete

### Manual Editing
Users can:
- Edit `/provision.json` to fix invalid values
- Re-insert SD card and reboot to retry
- Partially apply settings by only including desired sections

## Implementation Files

- `src/ProvisioningConfigLoader.h` — Header with main class
- `src/ProvisioningConfigLoader.cpp` — Implementation with all setting mappers
- `provision-example.json` — Complete example with all options
- Integration: `src/main.cpp` — Called after all stores are loaded

## Testing

To test provisioning on your device:

1. Create a minimal `/provision.json`:
   ```json
   {
     "text": {
       "fontSize": 16
     }
   }
   ```

2. Place on SD root

3. Boot device (watch serial log for `[PROV]` messages)

4. Verify fontSize changed in Settings > Text

5. Confirm `/provision.json` was deleted
