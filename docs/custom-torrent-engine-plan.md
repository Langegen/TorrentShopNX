# План перехода TorrentShopNX на собственный BitTorrent-движок

## 1. Текущая ситуация и зачем менять

**Что сейчас:**
- `source/torrent/torrent_engine.cpp` — огромная обёртка над `libtorrent 1.2.17`.
- `source/datasource/local_libtorrent_backend.cpp` — `IContentBackend`, который использует `lt::session` / `lt::torrent_handle`.
- Boost используется почти исключительно косвенно через libtorrent (Asio, System, Error code).
- Сборка компилирует сотни файлов libtorrent + kademlia + ed25519 прямо в бинарник.

**Проблемы:**
- Размер бинарника и RAM-потребление.
- Сложность тюнинга планировщика под Switch (NO_PEER_ON_PIECE, stalls, ring buffer пустой).
- Жёсткая зависимость от GPL-логики libtorrent.

**Решение:** новый самописный движок, вдохновлённый `NX-torrent-player`, но адаптированный под мульти-торрент очередь, выбор файлов и NSP/NSZ-инсталляцию TorrentShopNX.

---

## 2. Архитектурный подход: двигатель за абстракцией

TorrentShopNX уже имеет отличную абстракцию:

```
UI / DownloadManager
    ↓
DataSourceManager → IContentBackend ← BackendDataSource → IDataSource → Installer
    ↓
LocalLibtorrentBackend / ExternalTorrServerBackend
```

**План интеграции:**
1. Добавить `BackendType::CustomEngine`.
2. Реализовать `CustomEngineBackend : public IContentBackend`.
3. Оставить `LocalLibtorrentBackend` за флагом `USE_LIBTORRENT=1` для регрессионного сравнения.
4. Движок предоставлять через новый `TorrentEngine` фасад, но уже без libtorrent/boost.
5. UI не трогать — только переключить `data_mode=custom_engine` в настройках.

---

## 3. Компоненты нового движка

| Компонент | Реализация | Что делает |
|-----------|------------|------------|
| `bencode` | Собственный C/C++ | Парсинг .torrent и DHT-сообщений |
| `magnet` | Собственный | Парсинг `magnet:?xt=urn:btih:...` |
| `torrent_meta` | Собственный | Хранение info_hash, файлов, piece hashes |
| `sha1` | mbedtls (`mbedcrypto`) | Проверка целостности кусков |
| `peer_wire` | Собственный | Handshake, choke, interested, request, piece, cancel |
| `peer_session` | Собственный | Неблокирующее TCP-соединение + state machine |
| `netloop` | `poll()` | Один цикл на все пиры (критично для Switch: лимит 16 BSD-сессий) |
| `udp_tracker` | BEP 15 | Анонсы UDP-трекеров |
| `http_tracker` | curl | Анонсы HTTP(S)-трекеров |
| `dht` | vendored `jech/dht` | Mainline DHT: bootstrap, find_peers, announce_peer |
| `dhtclient` | Собственный обвязка | Запуск DHT на Switch, интеграция с пулом пиров |
| `piece_picker` | Собственный | Критические/urgent/prefetch/speculative/tail зоны |
| `piece_cache` | Собственный | RAM-хранилище кусков, eviction, chunked SD-cache |
| `torrent_session` | Собственный | Жизненный цикл одного торрента |
| `engine` | Собственный | Мульти-торрент менеджер, статусы, API |

---

## 4. Этапы разработки

### Этап 0. Подготовка инфраструктуры (1 неделя)

- Создать ветку `feature/custom-engine`.
- Добавить `source/engine/` и `include/engine/`.
- Вендорить:
  - `jech/dht` (`engine/dht.c`, `engine/dht.h`)
  - `libutp` (`engine/utp.cpp`, `engine/utp_utils.cpp`, `engine/utpbridge.cpp`)
- Настроить `pctest/` — PC-сборку движка для быстрой итерации (как у NX-torrent-player).
- Добавить флаг `USE_CUSTOM_ENGINE ?= 0` в Makefile и условную компиляцию.
- Определить C-интерфейс движка, чтобы C++ backend вызывал чистые C-функции.

**Критерий:** `make pctest` собирает движок на Linux/Windows и открывает тестовый торрент.

---

### Этап 1. Core protocol (1–2 недели)

1. **Bencode** — полный парсер/сериализатор:
   - строки, integers, lists, dictionaries.
   - zero-copy там, где возможно.
2. **Magnet parser** — btih hex/base32, display name, trackers.
3. **Torrent parser** — `info` dict, файлы, piece length, hashes.
4. **SHA-1** — через `mbedtls_sha1_*`.
5. **Peer handshake** — 68 байт, reserved bytes, info_hash, peer_id.
   - Peer ID: `"-TSNX01-"` + 12 рандомных байт.
6. **Peer wire framing**:
   - keep-alive, choke/unchoke, interested/not-interested.
   - have, bitfield, request, piece, cancel.
   - Пока без Fast Extension / PEX / LSD / encryption.

**Критерий:** Unit-тесты: bencode round-trip, magnet parse, torrent parse, handshake encode/decode.

---

### Этап 2. Peer management + netloop (2 недели)

1. **Неблокирующие TCP-соединения**:
   - `socket()` + `fcntl(O_NONBLOCK)` + `connect()` + `poll(POLLOUT)`.
   - Проверка `SO_ERROR`.
2. **Poll-driven netloop**:
   - Один поток `engine_netloop_thread()`.
   - До 24 одновременных сессий (баланс между Switch-лимитами и производительностью).
   - Обработка чтения/записи/ошибок/таймаутов.
3. **Peer session state machine**:
   - Connecting → Handshake → Bitfield → Interested/Unchoked → Requesting.
4. **Bitfield tracking**:
   - Хранение per-peer bitfield.
   - Обновление по `have`.
5. **Request pipeline**:
   - Block size = 16 KiB.
   - Pipeline depth ≈ 48 blocks на пира (~768 KB in-flight).
6. **Active piece assembly buffers**:
   - `aq_entry`: буфер piece_len + флаги have/req.
   - **Park/adopt**: если пир отвалился, частичный кусок сохраняется и передаётся другому пиру.

**Критерий:** PC-тест качает торрент с нескольких пиров, SHA-1 совпадает.

---

### Этап 3. Discovery: trackers + DHT (1–2 недели)

1. **UDP tracker client** (BEP 15):
   - connect, announce.
   - Поддержка `event=started`, `event=stopped`.
2. **HTTP(S) tracker client**:
   - Через curl или существующий `net::HttpClient`.
   - Парсинг bencoded ответа.
3. **DHT integration**:
   - `jech/dht` event loop в отдельном потоке.
   - Bootstrap nodes:
     - `router.bittorrent.com:6881`
     - `router.utorrent.com:6881`
     - `dht.transmission.com:6881`
     - `dht.libtorrent.org:6881`
   - `find_peers` по info_hash.
   - Дедупликация пиров из трекеров и DHT.
4. **Magnet metadata fetch** (BEP 9):
   - Extension protocol handshake.
   - ut_metadata запросы.
   - Проверка info_hash после сборки metadata.

**Критерий:** Magnet-ссылка без трекеров, только DHT, разрешается в файл-лист.

---

### Этап 4. Piece picker + streaming scheduler (2 недели)

Это самая важная часть для замены libtorrent.

1. **Piece states**:
   - `NEEDED`, `ACTIVE`, `WRITING`, `DONE`.
2. **Зоны приоритета**:
   - **Critical** — текущий кусок, который читает инсталлер.
   - **Urgent** — следующие 4–8 кусков.
   - **Prefetch** — readahead окно.
   - **Speculative** — дальше окна.
   - **Tail** — последние куски файла (важно для moov/NSP).
3. **Streaming window**:
   - 32 MB по умолчанию.
   - Адаптация под RAM-бюджет.
4. **Piece claiming**:
   - Каждый пир получает один `claim` — кусок, который он качает.
   - Быстрый пир может отбирать urgent-кусок у медленного после grace period.
5. **Calm mode / governor**:
   - Если буфер инсталлера полон — снижать скорость запросов.
   - Предотвращает пакетные бури и фризы Switch.
6. **Stall recovery**:
   - Если на critical-куске нет пира → top priority + deadline 0.
   - Временный ban медленных пиров (<16 KiB/s, RTT >2s).
   - Escalation: снять дедлайны → понизить prefetch → disconnect peer.

**Критерий:** В PC-тесте `stream` режим поддерживает sequential read без постоянных stalls.

---

### Этап 5. Storage: RAM-cache и chunked SD-cache (1–2 недели)

1. **RAM mode** (по умолчанию для Switch):
   - `piece_cache` хранит DONE-куски в RAM.
   - Бюджет: 128–256 MB.
   - Eviction только позади playhead.
2. **SD-cache mode** (опционально):
   - Chunked файлы по 1 GB (FAT32-friendly).
   - Append-only запись.
   - Слайсы по 256 KB, чтобы не блокировать чтение.
3. **Writer thread**:
   - SHA-1 verification.
   - Запись в RAM/SD.
   - Освобождение `aq_entry`.
4. **Read API**:
   - `engine_read(hash, file_index, offset, buf, size)`.
   - Блокирующее ожидание, пока куски не станут DONE.

**Критерий:** Инсталлер читает поток через `IDataSource` без пропусков.

---

### Этап 6. Интеграция в TorrentShopNX (2 недели)

1. **Новый backend**:
   - `source/datasource/custom_engine_backend.h/.cpp`.
   - `CustomEngineBackend : public IContentBackend`.
   - Реализует `open()`, `prebuffer()`, `read()`, `status()`, `close()`.
2. **TorrentEngine façade**:
   - Переписать `torrent::TorrentEngine` так, чтобы он держал внутри `engine_session_t*` из нового движка.
   - Сохранить публичные методы: `addMagnet`, `addTorrentFile`, `getTorrentList`, `getTorrentFiles`, `setFileWanted`, `removeTorrent`, `prepareStream`, `readPreparedAvailable`.
3. **Фабрика backend**:
   - `BackendType::CustomEngine`.
   - `create_backend()` возвращает `CustomEngineBackend` когда `cfg.mode == custom_engine`.
4. **File selection**:
   - `setFileWanted()` обновляет `wanted_files` и ставит приоритеты.
   - Невыбранные файлы не скачиваются.
5. **Multi-torrent**:
   - Движок поддерживает несколько активных торрентов.
   - Global peer budget распределяется между ними.

**Критерий:** Приложение собирается с `USE_CUSTOM_ENGINE=1`, UI работает, каталог, избранное, добавление magnet/torrent, выбор файлов, установка — всё функционирует.

---

### Этап 7. Оптимизации и Switch-специфика (2–3 недели)

1. **uTP через libutp**:
   - `utpbridge.cpp` даёт blocking socket-like API.
   - Интегрировать в netloop.
   - NAT-friendly, но TCP — primary.
2. **Switch thread priorities**:
   - netloop: `0x2B` (выше приложения).
   - writer: `0x2D` (ниже приложения).
3. **BSD socket limit**:
   - Никаких blocking вызовов в отдельных потоках.
   - Всё через poll.
4. **Memory pressure**:
   - Лимит одновременных active pieces.
   - Graceful eviction.
5. **DHT persistence**:
   - Сохранять routing table в `sdmc:/switch/TorrentShopNX/cache/dht/dht_cache.bin`.
6. **No upload / leech-only**:
   - Не принимать incoming connections.
   - Не отвечать на `request`.
   - Достаточно для установки игр.

**Критерий:** Стабильная работа на реальном Switch в течение 30+ минут.

---

### Этап 8. Тестирование и стабилизация (2–3 недели)

1. **PC-регрессионные тесты**:
   - Сравнение с libtorrent backend на одних и тех же magnet/torrent.
   - Метрики: stall_count, p95 piece wait, avg install speed.
2. **A/B на Switch**:
   - `USE_LIBTORRENT=1` vs `USE_CUSTOM_ENGINE=1`.
   - Хороший рой, средний рой, плохой рой.
3. **Edge cases**:
   - Magnet без трекеров.
   - Торрент с одним файлом и с множеством файлов.
   - NSZ сжатые (последовательное чтение важно).
   - Обрыв соединения, переподключение.
4. **Log parser**:
   - Автоматический подсчёт метрик из `log.txt`.
   - Целевые SLO из `download-stability-plan.md`.

**Критерий:** На хорошем рое `stall_time_ratio < 2%`, p95 ожидания куска `< 1500 мс`.

---

## 5. Сравнение с NX-torrent-player: что позаимствовать, что доработать

| Что позаимствовать | Что доработать под TorrentShopNX |
|---|---|
| poll()-based netloop | Мульти-торрент очередь |
| Park/adopt piece assembly | Выбор файлов в торренте |
| Calm mode / governor | Интеграция с NSP/NSZ инсталлером |
| RAM streaming mode | Полноценный `IDataSource` backend |
| jech/dht wrapper | DHT persistence |
| uTP bridge | HTTP tracker announces |
| Chunked SD-cache | Progress/status UI |

---

## 6. Риски и как их митигировать

| Риск | Митигация |
|---|---|
| Плохая связность в DHT-only swarms | Несколько bootstrap nodes + fallback trackers |
| Медленные/редкие рои | Тот же stall recovery + регрессионное сравнение с libtorrent |
| Память на Switch | RAM budget 128–256 MB + eviction позади playhead |
| Разные .torrent edge cases | Обширная библиотека тестовых .torrent в pctest |
| Время разработки | Постепенная миграция, libtorrent остаётся за флагом |
| Лицензии | jech/dht MIT, libutp MIT, mbedtls Apache — совместимо с GPLv3 |

---

## 7. Итоговая схема сборки

```makefile
# По умолчанию пока libtorrent
USE_LIBTORRENT      ?= 1
USE_CUSTOM_ENGINE   ?= 0

ifeq ($(USE_CUSTOM_ENGINE),1)
    SOURCES += source/engine engine/dht engine/utp
    CFLAGS += -DTSNX_USE_CUSTOM_ENGINE=1
    # libtorrent/boost исключаются
endif
```

Переключение в runtime:
```ini
[general]
data_mode=custom_engine
```

---

## 8. Первые конкретные шаги

1. Создать `source/engine/` и `include/engine/`.
2. Вендорить `bencode.c/h`, `magnet.c/h`, `torrent.c/h`, `peer.c/h`, `torrentfs.c/h`, `dht.c/h`, `dhtclient.c/h` из NX-torrent-player.
3. Настроить `pctest/` для PC-сборки.
4. Написать собственный `torrent_meta` и `bencode` unit-тесты.
5. Определить C-API между движком и `CustomEngineBackend`.
