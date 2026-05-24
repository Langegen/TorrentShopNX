# TorrentShopNX

A Nintendo Switch homebrew app for browsing decentralized torrent catalogs and streaming installs while downloading. It communicates with TorrServer over HTTP API.

## Features

- Catalog browser for JSON, RSS, magnet indexes, and torrent-distributed catalogs
- Torrent queue and download tracking
- Stream install pipeline with PFS0/NSP extraction to SD (streamed)
- FAT32-safe design by streaming data in chunks
- Minimal UI using libnx console
- HTTPS support via libcurl (portlibs)
- Catalog cache stored on SD
- TorrServer backend (local on-device or remote over network)

## Requirements

- devkitPro with libnx
- portlibs (libcurl, mbedtls)
- SD card with `/switch/TorrentShopNX/`
- TorrServer instance reachable from Switch (default `http://127.0.0.1:8090`)

## Setup

1. Install devkitPro + libnx + portlibs:
   - Follow the official devkitPro instructions for your OS.
2. Build:

```sh
make
```

3. Copy the NRO:
   - Output: `TorrentShopNX.nro`
   - Place it in `sdmc:/switch/TorrentShopNX/`

4. Optional TorrServer config file:
   - `sdmc:/switch/TorrentShopNX/torrserver.conf`
   - Example:

```ini
torrserver_url=http://127.0.0.1:8090
download_dir=sdmc:/switch/TorrentShopNX/downloads
```

## Catalog Sources

Edit `data/sources.json` to add sources. Example:

```json
{
  "sources": [
    {"name": "Community Catalog", "type": "json", "url": "http://example.org/catalog.json"},
    {"name": "RSS Feed", "type": "rss", "url": "http://example.org/rss"}
  ]
}
```

### Supported source types

- `json`: HTTP/HTTPS JSON catalogs with entries containing `title`, `size`, `magnet`, `category`, `description`, `icon`
- `rss`: RSS feed with items containing `title` and magnet link (in `magnet` or `link`)
- `magnet`: Direct magnet link in `url`
- `torrent`: Direct torrent or magnet URL in `url`

## Cache

- Cache folder: `sdmc:/switch/TorrentShopNX/cache/`
- Default TTL: 30 minutes

## Stream Install (while downloading)

- While downloading, the app streams `.nsp`/`.pfs0` data from partially written files and extracts to `sdmc:/switch/TorrentShopNX/stream_install/<name>/`.
- This avoids FAT32 4GB single-file limits by using libtorrent's file storage piece support.
- This is a safe extraction pipeline; integrating a real system install is left as a future step.

## Future Features (Placeholders)

- Stream install optimization
- Peer statistics
- Auto-seeding
- Catalog caching policies
- Offline mode
- Plugin catalogs

## License

This project is licensed under GPLv3 (or later) due to the embedded libtorrent dependency.
