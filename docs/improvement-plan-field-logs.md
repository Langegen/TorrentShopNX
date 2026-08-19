# План улучшения по итогам полевого прогона (engine1.log + log1.txt)

## Контекст

Полевой прогон на Switch (Divinity Original Sin 2, NSP 11.2 ГБ + обновление 870 МБ, затем Pets Survivors NSZ 89 МБ):

- Установка Divinity **успешна**, но с **542** событиями голодания буфера (`starvation_total=542`), ожидания до 30 с (`buffer wait wait_ms=30372`). Обновление — 42 события.
- Загрузка Pets Survivors **зависла**: `live=1` (один пир 51.38.58.132, остальные 12 не handshake), starvation recovery эскалировала `round 1 → 9` без доставки данных; пользователь закрыл приложение, установка отменена.
- CNMT in-memory извлечение упало в обоих файлах (`insufficient data, need 3368553 but have 3584`), спас fallback через ContentMeta FS.
- Массовые одновременные `UDP socket failed` + `Couldn't connect to server` (20:49–20:53) — сетевой/сокетный сбой.
- Половина трекеров в дефолтном списке мертва (DNS-фейлы, таймауты, invalid response).

## План работ

### Этап 1. Метрики и регрессионная база (P0)

Проблема: сейчас «успех» измеряется только `starvation_total`, остальное — шум в логе.

1. Написать парсер `engine1.log`/`log1.txt` (аналог `pctest/`): считает `starvation_count`, p50/p95/max `wait_ms`, время в stall, `live_peer_min/avg`, число `UDP socket failed`/`Couldn't connect`, успешность каждого трекера (announce → peers>0), `NO_PEER_ON_PIECE`.
2. Session summary в конце установки (`hybrid_nsp_installer.cpp:1047` рядом с `starvation_events`): добавить `wait_p95_ms`, `wait_max_ms`, `stall_total_ms`, `live_peers_min`, per-tracker stats.
3. Целевые SLO:
   - хороший рой (≥3 живых пира): `starvation_events` < 50 на 10 ГБ, p95 `wait_ms` < 3 с;
   - плохой рой (1-2 живых пира): отсутствие бесконечного stall, регулярный progress, UI-индикация состояния.

### Этап 2. Голодание буфера инсталлера (P1)

Проблема: сеть не успевает за записью на SD; инсталлер упирается в пустой ring buffer (542 раза, до 30 с).

1. **Backpressure вместо чтения любой ценой.** В цикле чтения (`hybrid_nsp_installer.cpp:754-771` и ncz-путь `:833-857`) не блокироваться навсегда: если `rb_avail` упал ниже watermark (например, 8 МБ), а скорость сети < скорости установки, приостанавливать запись на `wait_ms` вместо мгновенного retry — свести количество длинных ожиданий к плавному дросселированию.
2. **Адаптивный prebuffer.** `LOCAL_PREBUFFER_TIMEOUT_MS` уже есть (`:730`), но цель статическая. Считать целевой prebuffer динамически: `target = max(16 МБ, install_speed / download_speed_ewma * 64 МБ)` с потолком 128 МБ. Для роя с `live<=2` — всегда 128 МБ.
3. **Водяные знаки вместо бинарного «полон/пуст».** Читать chunk только при `rb_avail >= watermark_low`, ставить запись на паузу до `watermark_high` — гистерезис убирает «дёргание» инсталлера.
4. **UI:** в `DownloadsView` показывать фазу «буферизация» с прогрессом наполнения буфера, а не «установка 0%» с зависшим прогресс-баром.

### Этап 3. Зависшая загрузка с одним живым пиром (P1)

Проблема: 12 пиров в пуле, `live=1`, starvation recovery 9 раундов без эффекта, пользователь не понимает, что происходит.

1. **Эскалация без петель.** В `torrentfs.c:2562-2617` раунды только паузы (5/15/30 тиков). После `round >= 2` добавить активные действия: принудительный reannounce трекерам + `dht_search` boost (сигнал голода уже есть, `:2539`), ротация из резерва, повторная попытка handshake для «подключённых, но не handshake» адресов (в логе именно такой случай — 12 таких).
2. **Не осуждать единственного живого пира.** В condemnation (порог 4/6 провалов) добавить правило: если `live <= 1`, не выкидывать единственный живой пир из пула (аналог правила «не банить при <2 полезных пиров» из `docs/download-stability-plan.md`).
3. **Диагностика «1 живой пир».** Если `live==1` дольше 60 с — писать разовый snapshot: сколько адресов ждут дозвона, откуда они (tracker/PEX/DHT), сколько в резерве.
4. **UI-индикация.** Показывать `live_peers` в `DownloadsView`; при 0 прогресса > 30 с — статус «нет пиров/мало пиров» вместо молчания (Pets Survivors отменён именно из-за тишины).

### Этап 4. Трекеры: чистка и cooldown (P1)

Проблема: `DEFAULT_TRACKERS` (`torrent_meta.c:79-105`) наполовину мёртв — DNS-фейлы (gbitt.info, moeking.me, pomf.se, endpot.com, acgnxtracker, tamersunion), таймауты (opentrackr, files.fm), `invalid tracker response` (openbittorrent HTTP), `no connect reply` (archive.org). Работают: t-ru.org (из магнита), exodus.desync, dler.org, torrent.eu.org (с перебоями).

1. По статистике из Этапа 1 вычистить список: оставить подтверждённо живые, убрать/закомментировать мёртвые.
2. **Cooldown трекера:** после N последовательных провалов одного типа (DNS/timeout) не трогать трекер 10-30 мин — меньше шума в логе и меньше потраченного бюджета дозвона.
3. **Per-tracker счётчики** в `torrent_meta.c` (или в session summary): announce_ok/fail, peers_obtained — чтобы не гадать, что работает.

### Этап 5. Массовые сокетные сбои (P1)

Проблема: `UDP socket failed` у всех UDP-трекеров + `Couldn't connect to server` у всех HTTP одновременно — похоже на исчерпание fd/сокетов при пиковой нагрузке (DHT + дозвоны + announce).

1. Ограничить одновременные UDP-announce: сериализовать или держать ≤ 2-3 активных UDP-тракер-сокета (сейчас каждый announce может открывать свой).
2. При детекции `socket failed`/`Couldn't connect` — общий backoff для новых дозвонов на 5-10 с (защита NAT-таблицы уже есть через `DIAL_BUDGET_PER_TICK`, но сокеты открываются и вне этого бюджета).
3. Проверить связку с лимитом poll() (POLL_CHUNK_MAX) и netloop: не вытесняются ли трекерные сокеты из ротации.

### Этап 6. CNMT: основной путь через ContentMeta FS (P2)

Проблема: `cnmt_parser.cpp` (`extractFromNca:137`, `insufficient data:91`) пытается распарсить CNMT-файл из ring buffer, но получает лишь первые байты NCA (3584/7168 из нужных 3.3/2 МБ) — оба раза упало, спас fallback.

1. Сделать первичным путь, который уже работает: чтение CNMT из `@SdCardContent://registered/...` (ContentMeta FS) — он устойчив к размеру файла.
2. In-memory путь либо убрать, либо дочитывать NCA целиком (он крошечный, ~3.4 КБ) с retry из ring buffer, а не парсить неполные данные.
3. Логировать, какой путь сработал, в session summary.

### Этап 7. Мелочи (P3)

1. **DHT-шум** (`dhtclient.c:439`): `background nodes=...` каждые 2 с — логировать при изменении (рост/падение узлов) или раз в 30 с.
2. **UPnP** (`upnp.c`): ретраи с бэк-оффом, один failure-лог за сессию; IGD часто просто отсутствует.
3. **Health/session restarts:** в логе движка повторяются `session restart requested` — если это код TorrServer (вне репозитория), отметить как known-issue и добавить ограничение частоты рестартов; если локализуется в этом репо — добавить min-interval 5 мин.
4. **404 на обложки** (fastpic.org): опционально обновить URL в каталоге или скрыть битые.

## Статус внедрения (2026-08-19)

### Сделано

1. **Парсер логов** — `pctest/log_stats.ps1 -AppLog log1.txt [-EngineLog engine1.log]`: starvation events, wait p50/p95/max, install/source speed, slow-peer boosts, pace sleeps, CNMT-пути, 404-обложки; per-tracker ok/attempts/peers, причины фейлов, starvation rounds, live peers, DHT-спам.
2. **Session summary** — `hybrid_nsp_installer.cpp`: в конце установки и при отмене пишется `installer: summary starvation_events=… wait_p50_ms=… wait_p95_ms=… wait_max_ms=… stall_total_ms=… install_speed_avg/max… source_speed_avg/max… live_peers_min=…`. Метрика `live_peers_min` через новый `IDataSource::livePeers()` (backend → `status().peers`, в custom engine это live-сессии).
3. **Pacing инсталлера** — ограничение потребления ~92% от `downloadSpeedKBps()` (5-сек окно, сон ≤3 с, отменяемо) + watermark 8 МБ перед блокирующим `read()` (хвост файла не ждёт). Полевой прогон: 542 starvation-события, p95 15.6 с — цель <50 событий и p95 <3 с на 10 ГБ. Pacing меряет потребление **сжатого** payload из ring buffer (`payload_consumed_`), а не `bytes_installed_`: для NSZ установленный объём в 1/ratio раз больше скачанного (сжатие до 60%), и pacing по нему задушил бы NSZ-установку до ~ratio скорости сети. В summary добавлены `consume_speed_avg/max` (потребление payload); watermark-хвост считается по сумме размеров entry (для NSZ `total_bytes_` = распакованный размер, сравнение с `stream_pos` было бы неверным).
4. **Адаптивный prebuffer** — target = 10 сек текущей скорости сети (16-128 МБ), для медленного роя максимум.
5. **Starvation recovery эскалация** (`torrentfs.c`): round 4+ — hard reset backoff всего пула (кроме 0xFF-осуждённых, включая трекерных пиров); reannounce при live≤1 раз в 20 с вместо 60 с; cap round на 12; диагностический snapshot состава пула (tracker/pex/dht/busy) на round 3 и дальше каждый 6-й. Живые пиры защищены от condemnation через `peer_busy` (проверено).
6. **Чистка трекеров** (`torrent_meta.c`): из 25 дефолтных оставлено 4 подтверждённо живых (torrent.eu.org, exodus.desync, dler.org, filemail) — по статистике обоих полевых прогонов. Меньше UDP-сокетов (митигирует `UDP socket failed`-шторм), меньше шума. Cooldown (3 фейла → 10 мин) уже был и работал.
7. **CNMT** (`hybrid_nsp_installer.cpp`): основной путь теперь ContentMeta FS (стабилен в поле), in-memory парсинг из буфера — запасной.
8. **P3**: DHT-лог пишется только при изменении узлов/пиров или раз в 60 с (было каждые 2 с, 2325 строк за прогон); UPnP — 3 попытки SSDP с бэк-оффом 10 с, один лог за сессию; UI — статус «Установка: нет пиров» при peers=0 и нулевой скорости (`DownloadsView.cpp`).

### Проверка

- `pctest\build.ps1` — все 8 таргетов собраны, `test_engine` 16/16, `test_torrent_meta` 3/3, `test_magnet` 3/3, `test_bencode` 8/8.
- `make` (devkitPro) — `TorrentShopNX.nro` собран.
- Что проверить на железе: повторный прогон Divinity (starvation_events, p95), Pets Survivors с 1 сидом (UI «нет пиров» вместо молчания), поведение нового prebuffer на медленном рое.

## Приоритет внедрения

1. **P0:** парсер логов + session summary — без метрик нельзя сравнивать прогоны.
2. **P1:** backpressure + адаптивный prebuffer (Этап 2).
3. **P1:** эскалация при `live<=1` + защита единственного живого пира (Этап 3).
4. **P1:** чистка трекеров + cooldown (Этап 4), лимит UDP-сокетов (Этап 5).
5. **P2:** CNMT через ContentMeta FS (Этап 6).
6. **P3:** лог-шум, UPnP, health-рестарты (Этап 7).

## Проверка

- Повторный прогон Divinity: `starvation_events` < 50 на 10 ГБ, p95 `wait_ms` < 3 с; в логе нет серий `buffer wait wait_ms>10000`.
- Pets Survivors с 1 сидом: либо установка завершается, либо UI честно показывает «мало пиров» вместо молчаливого зависания; starvation recovery не ходит по кругу `round 1..9`.
- В логе движка нет `UDP socket failed`-штормов и DNS-спама от мёртвых трекеров.
- CNMT берётся через ContentMeta FS без `insufficient data`.
- `pctest` сборка (`pctest\build.ps1`) и Switch-сборка (`make`) проходят.