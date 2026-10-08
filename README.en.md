<div align="center">

<img src="resources/icon/icon.png" alt="TorrentShopNX Logo" width="120"/>

# TorrentShopNX

**BitTorrent client, game & retro console catalog with direct stream installation for Nintendo Switch**

[![Download Latest Release](https://img.shields.io/badge/Download-Latest_Release-2ea44f?style=for-the-badge&logo=nintendo-switch&logoColor=white)](https://github.com/Langegen/TorrentShopNX/releases/latest)
[![Switch to Russian](https://img.shields.io/badge/Language-Русский-0969da?style=for-the-badge&logo=googletranslate&logoColor=white)](README.md)

[![GitHub Release](https://img.shields.io/github/v/release/Langegen/TorrentShopNX?color=blue&label=Release)](https://github.com/Langegen/TorrentShopNX/releases/latest)
[![GitHub Downloads](https://img.shields.io/github/downloads/Langegen/TorrentShopNX/total?color=success&label=Downloads)](https://github.com/Langegen/TorrentShopNX/releases)
[![License](https://img.shields.io/badge/License-GPL--3.0-orange)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Nintendo%20Switch-e60012?logo=nintendoswitch&logoColor=white)](https://github.com/Langegen/TorrentShopNX)
[![UI Engine](https://img.shields.io/badge/UI-Borealis-8a2be2)](https://github.com/natinusala/borealis)

[🇷🇺 Русский](README.md) | **🇬🇧 English**

---
</div>

**TorrentShopNX** is an all-in-one homebrew application for Nintendo Switch combining an extensive game catalog, a built-in high-performance BitTorrent client, direct on-the-fly stream installation, an 8,200+ retro games catalog for 20 platforms, an emulator & BIOS manager, and a dual-pane file manager with built-in archive tools. It enables you to discover, download, and install games, updates, DLCs, and retro ROMs directly onto your console without requiring a PC or external servers.

> [!IMPORTANT]
> **Notice:** This project is developed strictly for educational and research purposes. Only use this application with content that you have the legal right to access and use.

---

## 📸 Screenshots

<div align="center">
  <img src="docs/screenshots/dashboard.jpg" alt="TorrentShopNX v2.14 Main Menu & Dashboard" width="95%"/>
  <p><em>Main Menu & Dashboard: Quick access to the game catalog, retro platforms, game manager, downloads, and storage stats</em></p>
  <br/>

  <img src="docs/screenshots/catalog.jpg" alt="Game Catalog & Curated Collections" width="95%"/>
  <p><em>Game Catalog: Main sections (new releases, Metacritic Top 100, favorites) and 14 genre collections</em></p>
  <br/>

  <img src="docs/screenshots/retro_games.jpg" alt="Retro Games Catalog" width="95%"/>
  <p><em>Retro Games Catalog: Over 8,200 games across 20 systems (Nintendo, Sony PlayStation, Sega) with direct saving to RetroArch folders</em></p>
</div>

---

## ✨ Key Features

- ⚡ **Built-in BitTorrent Engine (Custom Engine)**:
  - Fully standalone — runs natively on Nintendo Switch without requiring an external server or PC.
  - Comprehensive protocol support: DHT (Kademlia), µTP, PEX, UDP/HTTP(S) trackers, fast magnet metadata exchange (BEP 9 / ut_metadata).
  - Intelligent piece picker with a sliding-window pre-buffer optimized for direct streaming installations into RAM.
  - High speed and stability: optimized choke/unchoke rotation, rock-solid support for large torrents with 16 MB piece sizes without speed degradation.
  - **Swarm Inspector**: live monitoring of connected peers, seeds, piece states, and aggregate swarm download speeds.
  - Inbound connection port forwarding support (default TCP `6882`) to maximize reachable peers in swarms.

- 🎮 **Direct On-The-Fly Stream Installation (Stream Installer)**:
  - Stream install NSP, NCZ, XCI, updates, and DLCs directly into console storage (SD card or NAND memory).
  - **FAT32-friendly**: eliminates the need to download huge 20–40+ GB intermediate archive files before installation, saving storage space and SD card write cycles.
  - **Firmware / SDK Compatibility Check**: automatic warnings if an update or DLC requires a higher Horizon OS / SDK version than currently running on your console.
  - **Unpacked Size Calculation**: pre-calculates the actual decompressed size of NSZ/NCZ packages prior to installation to prevent out-of-space issues.

- 🕹️ **Retro Games Catalog, ROMs & Emulator Manager**:
  - Massive library: **over 8,200+ games across 20 gaming platforms**:
    - **Nintendo (10 systems)**: NES / Famicom, Super Nintendo (SNES), Nintendo 64, Game Boy / Color, Game Boy Advance, Nintendo DS, Nintendo 3DS, GameCube, Wii, Wii U.
    - **Sony (4 systems)**: PlayStation 1, PlayStation 2, PlayStation Portable (PSP), PlayStation Vita.
    - **Sega (6 systems)**: Mega Drive / Genesis, Master System, Game Gear, Dreamcast, Saturn, SG-1000.
  - Includes original clean dumps, localized releases, and popular fan hacks.
  - **Built-in Emulator & BIOS Manager**: one-click download and updates for standalone emulators (RetroArch, pSNES, mGBA, PPSSPP, etc.) along with necessary BIOS packages.
  - **Folder Tree Explorer**: browse nested folder structures inside torrents without file limits (256-file restriction removed) with selective downloading support for romsets.
  - Automatic download to emulator folders (default `sdmc:/roms/<console>/`) or custom storage paths.
  - Online retro database updates directly from GitHub via the `-` button.

- 🗂️ **Built-in Dual-Pane File Manager & Utilities**:
  - Fast access from anywhere in the app with the `+` button.
  - **Dual-Pane Mode (Split Screen)**: quickly toggle split view with `-` to browse two directories side by side and copy/move files effortlessly.
  - **Archive Extractor & Packer (7zsdk)**: streaming unpacking for `.7z`, `.zip`, `.rar` archives with live extraction progress, and on-console `.zip` archive creation.
  - **Package Installer**: install NSP, NSZ, XCI, XCZ files directly from local SD card storage.
  - **Built-in Text Viewer**: view NFO files, game guides, and text documents directly on your Switch.
  - Batch file operations: multi-select via `Y`, select all with `LB`, deselect all with `RB`.
  - Press `A` on completed non-game downloads to reveal them immediately in the file manager.

- 🎨 **System Theming & UI Customization**:
  - Default Light Theme, Dark Theme, and 6 vibrant color palettes: Emerald, Amber, Amethyst, Cyberpunk, Graphite, Ruby.
  - Custom Wallpaper Support: choose from bundled atmospheric themes or load any custom image directly from your SD card.
  - Fine-grained Background Blur (0–3) and Dim (0–4) controls to ensure high text contrast and legibility.
  - Option to hide the bottom dashboard statistics footer for a cleaner, minimalist layout.

- 📚 **Rich Game Catalog & Curated Collections**:
  - Built-in, regularly updated catalogs in English, Russian, and additional languages.
  - **ETag Delta Updates**: instant diff-based catalog sync without re-downloading entire databases.
  - **Main Sections**: "All Catalog" (thousands of games), "New Releases", "Metacritic Top 100", and "Favorites".
  - **14 Curated Genre & Thematic Collections**, alphabetical A-Z navigation, and instant search using the native Switch on-screen keyboard.
  - Multi-select filters powered by unified genre taxonomies.

- 🖼️ **Detailed Game Cards**:
  - High-resolution box art covers with local SD thumbnail caching for smooth scrolling.
  - Built-in screenshot gallery with a full-screen image viewer.
  - Detailed descriptions, download sizes, voice and subtitle language information, and required SDK versions.
  - Favorites system for bookmarking titles you want to install later.

- 📱 **Remote Torrent Adding (Web UI & QR Code)**:
  - Built-in local HTTP server on the Switch (port `8080`) displaying an instant QR code on the screen.
  - Easily send magnet links or upload `.torrent` files directly from your smartphone or PC.

- 🎯 **Selective File Downloading**:
  - Inspect torrent contents with a full directory tree and choose specific files (base game, specific updates, individual DLCs, or specific ROMs) prior to download.

- 📥 **Advanced Downloads Manager**:
  - Real-time download progress, transfer speeds, peer/seed counts, install phase status, and Swarm Inspector.
  - Ability to pause, cancel, and manage installation queues.

- 💾 **Game Manager & Storage Monitor**:
  - View installed titles, versions, DLCs, and running SDK version info.
  - Real-time monitoring of free and used storage on both SD card and NAND memory in the dashboard header.

- 🔋 **Power Management & Comfort**:
  - Prevents console sleep mode during active downloads.
  - **Safe Backlight Wakeup**: the first button press only wakes the screen without accidentally triggering buttons or menus.
  - Configurable screen backlight timeout to save battery and protect OLED screens during long downloads.
  - Applet Mode detection and warning with advice to run via Title Override for full memory allocation.

- 🔄 **Built-in Auto Updater**:
  - Automatically checks for and installs application updates directly from GitHub Releases with safe hot-rebooting.

- 🌐 **Alternative Backend (TorrServer)**:
  - Option to connect to an external TorrServer instance on your local network if you prefer caching on a home server or NAS.

---

## 🎮 Controls & Shortcuts

| Button | Action |
| :---: | :--- |
| **`+`** | Open **File Manager** (from any screen) / Start download (in file selector) / Exit File Manager |
| **`-`** | **Remote Add** (Web UI / QR) / **Update Databases** (in Retro Games) / Toggle split-screen mode (in File Manager) |
| **`A`** | Select / Confirm / Open folder / Folder tree navigation in romsets / Open download in File Manager |
| **`B`** | Back / Close dialog / Cancel |
| **`X`** | Quick search / Actions menu (in File Manager) / Toggle all (in file selector) / Refresh dashboard |
| **`Y`** | Filter multi-select & sort / Add to Favorites / Toggle item selection (in File Manager) / Emulator Manager (in Retro Consoles) |
| **`LT` / `RT`** | Switch between active panels (in File Manager split mode) |
| **`LB` / `RB`** | Select all / Deselect all (in File Manager); navigate screenshot gallery |

---

## 🚀 Quick Start & Installation

### 1. Download
Download the latest `TorrentShopNX.nro` from the [Releases](https://github.com/Langegen/TorrentShopNX/releases/latest) page or click the download button at the top of this page.

### 2. Copy to SD Card
Place `TorrentShopNX.nro` onto your SD card at the following path:
```text
sdmc:/switch/TorrentShopNX/TorrentShopNX.nro
```

### 3. Launching the App

> [!WARNING]
> **Important: Run with Full Memory Access (Title Override)!**
> 
> Do **not** launch the app via the **Album (Applet Mode)**. In Applet Mode, the console only allocates ~400 MB of RAM to homebrew, which will cause out-of-memory errors during game extraction and installation.
> 
> **How to launch properly:**
> 1. Press and hold the **`R`** button on your controller.
> 2. Launch **any installed game** from the Switch main menu while holding `R`.
> 3. The Homebrew Menu will open with access to the console's full memory pool (High Memory Mode).
> 4. Launch **TorrentShopNX**.

---

## 📱 Adding Torrents from Smartphone or PC

If a game is not available in the built-in catalog, you can instantly push any torrent or magnet link from your phone or computer:

```text
+-----------------------------------------------------------+
| 1. Press "-" or open the "Remote Add" section in the app  |
| 2. Scan the displayed QR code with your phone camera      |
|    (or navigate to the URL shown, e.g.                    |
|     http://192.168.1.50:8080)                             |
| 3. Paste a magnet link or upload a .torrent file          |
| 4. Tap "Send" — the Switch will open the file picker      |
+-----------------------------------------------------------+
```

---

## ⚙️ Configuration (`config.ini`)

The configuration file is created automatically on first launch:
```text
sdmc:/switch/TorrentShopNX/config.ini
```

### Example Configuration:
```ini
[general]
data_mode=local_client
torrserver_url=http://192.168.1.100:8090
catalog_source_url=https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/EN_catalog.json
install_location=auto
keep_awake_during_downloads=true
backlight_timeout=0
cache_cover_thumbnails=false
listen_port=6882
app_update_url=https://api.github.com/repos/Langegen/TorrentShopNX/releases/latest
auto_app_update=true
language=en

# Retro Games settings
retro_roms_mode=retroarch
retro_custom_path=
retro_auto_extract=false
retro_romset_mode=full

# UI Theming and Appearance
theme=light
background_mode=auto
custom_background_path=
background_blur=0
background_dim=2
show_bottom_dashboard=true
```

### Settings Reference:

| Parameter | Possible Values | Description |
| :--- | :--- | :--- |
| `data_mode` | `local_client` / `torrserver` | Backend mode: built-in custom engine (`local_client`) or external TorrServer (`torrserver`). |
| `catalog_source_url` | `http(s)://...` | URL to the remote JSON game catalog (if left blank, picked automatically based on language). |
| `install_location` | `auto` / `sd` / `nand` | Target storage for game installation: Automatic (`auto`), SD Card (`sd`), or internal NAND (`nand`). |
| `torrserver_url` | `http(s)://...` | Address of external TorrServer (when `data_mode=torrserver`). |
| `keep_awake_during_downloads`| `true` / `false` | Prevents the console from going to sleep while downloading. |
| `backlight_timeout` | `0`, `15`, `30`, `60`, `120` | Seconds before screen backlight dims/turns off (`0` = never turn off). |
| `cache_cover_thumbnails` | `true` / `false` | Enables local thumbnail caching on SD card for smoother scrolling. |
| `listen_port` | `1024–65535` (default `6882`) | Inbound TCP listen port for peer connections. Forward this port on your router for maximum speed. |
| `auto_app_update` | `true` / `false` | Automatically check for app updates on startup. |
| `app_update_url` | `http(s)://...` | GitHub API endpoint for release updates. |
| `language` | `auto` / `en` / `ru` / `es` / `fr` / `de` / `it` / `pt-BR` / `zh-Hans` / `ja` | Application interface language (`auto` uses console system language). |
| `theme` | `light` / `emerald` / `cyberpunk` / `ruby` / `amethyst` / `amber` / `graphite` | Color theme for UI elements and highlights (defaults to `light`). |
| `background_mode` | `auto` / `custom` / `<theme_name>` | Background wallpaper mode: dynamic by theme (`auto`), preset wallpaper, or custom file (`custom`). |
| `custom_background_path` | `sdmc:/...` | SD card file path to custom wallpaper image (when `background_mode=custom`). |
| `background_blur` | `0` (Off), `1` (Low), `2` (Medium), `3` (High) | Blur level applied to the background image. |
| `background_dim` | `0` (0%), `1` (15%), `2` (28%), `3` (45%), `4` (65%) | Dimming overlay applied to the background to ensure high text readability. |
| `show_bottom_dashboard` | `true` / `false` | Whether to display the bottom statistics footer on the dashboard. |
| `retro_roms_mode` | `retroarch` / `downloads` / `custom` | Target folder for ROM downloads: RetroArch (`sdmc:/roms/<console>/`), downloads folder, or custom path. |
| `retro_custom_path` | `sdmc:/...` | Custom directory path for ROMs (used when `retro_roms_mode=custom`). |
| `retro_auto_extract` | `true` / `false` | Automatically extract downloaded ROM archives upon completion. |
| `retro_romset_mode` | `full` / `select` | Romset download mode: download the full set archive (`full`) or open folder tree file selection (`select`). |

---

## 📋 Catalog Source Format

TorrentShopNX supports custom JSON game catalogs matching the following structure:

```json
[
  {
    "title": "Super Game Odyssey",
    "size": "5.42 GB",
    "magnet": "magnet:?xt=urn:btih:EXAMPLEHASH&dn=Game...",
    "topic_id": "1234567",
    "url": "https://example.com/topic/1234567",
    "year": "2023",
    "genre": "Platformer, Adventure",
    "developer": "Awesome Studio",
    "publisher": "Awesome Publisher",
    "image_format": "NSP",
    "interface_lang": "English, Russian",
    "voice_lang": "English",
    "cover": "https://example.com/covers/game.png",
    "screenshots": [
      "https://example.com/screens/1.jpg",
      "https://example.com/screens/2.jpg"
    ],
    "description": "Detailed description of the game..."
  }
]
```

---

## 🛠️ Building from Source

### Prerequisites:
- [devkitPro](https://devkitpro.org/) toolchain (`devkitA64`).
- Libraries: `libnx`, `switch-curl`, `switch-mbedtls`, `switch-zlib`, `switch-libpng`, `switch-libjpeg-turbo`.
- [Borealis](https://github.com/natinusala/borealis) UI submodule (included in `_external/borealis`).

### Build:
```bash
# Clone the repository with submodules
git clone --recursive https://github.com/Langegen/TorrentShopNX.git
cd TorrentShopNX

# Build the NRO binary
make -j$(nproc)
```

Build artifact:
```text
TorrentShopNX.nro
```

---

## 📂 Project Structure

```text
TorrentShopNX/
├── source/
│   ├── 7zsdk/         # Archive extraction engine (7z, ZIP, RAR)
│   ├── buffer/        # Ring buffer and piece pool management
│   ├── catalog/       # Catalog parsing, retro database, search, filtering, and collections
│   ├── config/        # Configuration reader and writer (config.ini)
│   ├── datasource/    # Data source abstractions (Custom Engine & TorrServer)
│   ├── download/      # Download queue manager and scheduling
│   ├── engine/        # Custom BitTorrent engine (DHT, uTP, Bencode, Wire)
│   ├── installer/     # Stream content installer (NSP, NCZ, XCI, NCM)
│   ├── net/           # Networking utilities, HTTP client (curl), and Web UI server
│   ├── rss/           # RSS feed parsing
│   ├── torrent/       # Torrent files and magnet links processing
│   ├── ui/            # Borealis UI views, cards, and dialogs
│   │   └── dashboard/ # Main dashboard widgets and statistics
│   └── utils/         # System utilities, logging, file helpers
├── resources/         # Fonts, icons, localization files (i18n), sounds and RomFS assets
└── docs/              # Technical documentation, architecture plans, and screenshots
    └── screenshots/   # Application interface screenshots
```

---

## 📄 License

This project is licensed under the **GNU General Public License v3.0 (GPLv3)**. See [LICENSE](LICENSE) for the full license text.

---

<div align="center">
  <sub>Made with ❤️ for the Nintendo Switch Homebrew Community</sub>
</div>
