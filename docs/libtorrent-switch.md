# Libtorrent 1.2 Integration (Switch)

The project vendors upstream libtorrent **v1.2.20**:

- `_external/libtorrent-v1.2.20` (tag `v1.2.20`, commit `2e537ee7afcfa92d02862d81c53db2a363bfdd1e`)

## Build Mode

`Makefile` now enables libtorrent by default:

```sh
make -j4
```

To build without local client backend:

```sh
make -j4 USE_LIBTORRENT=0
```

## Required Static Libraries

You still must provide switch-compiled static libs:

- `libtorrent-rasterbar`
- `boost_system`
- `boost_random`
- `boost_chrono`

If they are not in default linker search paths, pass:

```sh
make -j4 \
  LIBTORRENT_LIBDIR=/path/to/libtorrent/lib \
  BOOST_LIBDIR=/path/to/boost/lib
```

## Local Client Features

With `TSNX_USE_LIBTORRENT=1`, local mode supports:

- metadata/file-list probing from magnet (for pre-download file selection),
- sequential streaming reads for installer,
- sparse storage (no preallocation),
- low-memory profile:
  - `tick_interval=1000`
  - `cache_size=1024`
  - `announce_to_all_trackers=true`
