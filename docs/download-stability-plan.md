# План повышения стабильности и скорости скачивания

## Контекст из `log.txt`

Лог показывает установку через локальный libtorrent-клиент в режиме RAM-cache без локального HTTP-сервера. Торрент стартует с `sequential_download=false`, куском 4 MiB, файлом 3.18 GiB и окном планировщика `critical=0-3`, `urgent=4-11`.

Ключевые симптомы:

- Быстрый старт сети, но почти сразу появляется `Stalled (waiting for piece 0)` при наличии активных пиров.
- На протяжении лога повторяются переходы `StreamingOrInstalling -> Stalled -> StreamingOrInstalling`.
- Встречается диагноз `NO_PEER_ON_PIECE`: есть активные пиры и скорость роя, но ни один пир не качает кусок, который нужен инсталлеру прямо сейчас.
- Один и тот же медленный пир с RTT 1.3-6.3 с регулярно изолируется, но остаётся в рое и продолжает получать запросы.
- Ring buffer у инсталлера постоянно пустой (`rb_avail=0`), из-за чего downstream ограничен не скоростью записи, а starvation на источнике.

Итог: текущая проблема не выглядит как чисто «медленный интернет». Основной узкий участок — рассинхронизация между потребителем последовательного потока, приоритетами libtorrent и фактическими запросами к пирам.

## Наблюдения по коду

### 1. Планировщик корректно задаёт 5 зон, но stall-восстановление слишком мягкое

`TorrServerScheduler` декларирует модель Critical/Urgent/Prefetch/Speculative/Rarest и намеренно не включает `sequential_download`. В stall-режиме Prefetch отключается, Urgent сжимается, но Critical остаётся всего 4 куска. При диагностике `NO_PEER_ON_PIECE` это не гарантирует немедленное перенацеливание быстрых пиров на текущий кусок.

### 2. Slow-peer isolation диагностирует проблему, но не устраняет её достаточно жёстко

Сейчас медленный пир определяется по RTT/скорости и планировщик пытается снять дедлайн с обслуживаемого им куска. По логу это срабатывает десятки раз, но пир продолжает присутствовать и получать запросы. Нужен escalation path: от мягкого снятия дедлайна к clear/request reprioritization, затем к временной блокировке endpoint.

### 3. Глобальный `request_queue_time` конфликтует с текущей critical-позицией

`CongestionController` меняет глобальный `request_queue_time` на основе RTT и скорости. В логе видны большие очереди у быстрых пиров, которые иногда заняты будущими кусками, пока текущий critical кусок не обслуживается. Для стриминговой установки важнее latency до текущего куска, чем максимальная суммарная скорость роя.

### 4. RAM-cache работает на грани, но не является главным bottleneck в этом логе

PiecePool на 72 куска и ограничение RAM-кэша в 64 куска соответствуют примерно 256 MiB при размере куска 4 MiB. В логе есть регулярные eviction после выхода за лимит, но stalls начинаются задолго до pressure на кэш. Тем не менее eviction нужно сделать наблюдаемым и безопасным относительно `critical/urgent` окна.

### 5. Метрики уже близки к полезным, но не хватает агрегированных SLO

Лог содержит `PIECE_WAIT_STATE`, `SWARM status`, `buffer wait`, `partial read advance`, `evict`, но выводит их как события. Для регрессионного тестирования нужны агрегаты по сессии: число stall, p50/p95 ожидания куска, доля `NO_PEER_ON_PIECE`, средний `rb_avail`, доля времени в Stalled, эффективность slow-peer isolation.

## План работ

### Этап 0. Зафиксировать воспроизводимость и метрики

1. Добавить session summary в конец установки или при остановке backend:
   - `stall_count`, `stall_total_ms`, `stall_p95_ms`;
   - `piece_wait_no_peer`, `piece_wait_slow_delivery`, `piece_wait_all_choked`;
   - `installer_starvation_count`, `ring_buffer_empty_ms`;
   - `avg/p95 download_payload_rate`, `avg install_speed`;
   - `slow_peer_isolation_count`, `slow_peer_ban_count`.
2. Написать простой parser для `log.txt`, который считает эти метрики и печатает diff между двумя прогонами.
3. Ввести целевые SLO для 3 типов роев:
   - хороший рой: `stall_time_ratio < 2%`, p95 ожидания куска `< 1500 ms`;
   - средний рой: `stall_time_ratio < 8%`, p95 `< 5000 ms`;
   - плохой рой: отсутствие бесконечного stall, регулярный progress.

### Этап 1. Исправить `NO_PEER_ON_PIECE`

1. В `LocalLibtorrentBackend::read()` при `PIECE_WAIT_STATE => NO_PEER_ON_PIECE` вызвать отдельный метод scheduler, например `on_piece_starvation(piece, peers)`.
2. В этом методе:
   - выставлять текущему куску `top_priority` и `deadline=0` с флагом alert/duplicate request, если доступен в используемой версии libtorrent;
   - временно понижать priority будущего prefetch/speculative окна;
   - очищать stale deadlines вне `critical+urgent`, чтобы fast peer не продолжал качать будущие куски;
   - повторять escalation через 1-2 секунды, если `on_target` остаётся 0.
3. Для stall-режима расширить Critical с 4 до 6-8 кусков только если `NO_PEER_ON_PIECE` повторяется, а не всегда. Это снизит риск «дёргания» окна на слабом рое.

### Этап 2. Сделать slow-peer isolation жёстче и безопаснее

1. Вести per-peer score:
   - EWMA скорости;
   - RTT EWMA;
   - число раз, когда пир был единственным/последним держателем critical куска;
   - число подряд slow samples.
2. Добавить уровни реакции:
   - уровень 1: убрать deadlines с кусков, которые обслуживает пир;
   - уровень 2: понизить приоритет кусков, на которых он «залип»;
   - уровень 3: `disconnect_peer`/временный ban endpoint на 30-120 секунд, если пир стабильно < 16-32 KiB/s и RTT > 2 с.
3. Не банить пир, если в рое меньше 2 полезных пиров или если этот пир единственный источник rare piece.
4. Логировать не только факт isolation, но и итог: «после isolation target piece picked by peer X за N ms».

### Этап 3. Разделить latency-mode и throughput-mode для `request_queue_time`

1. В normal-mode оставить текущий адаптивный контроллер.
2. В latency-mode, когда `rb_avail == 0` или backend находится в `Stalled`, временно:
   - снижать `request_queue_time` до 1-2 секунд;
   - уменьшать speculative/prefetch;
   - повышать дедлайны только для `critical`.
3. Возвращаться в throughput-mode только после восстановления буфера, например когда `rb_avail >= 16-32 MiB` или нет stall 10 секунд.
4. Добавить hysteresis, чтобы не переключаться каждый тик.

### Этап 4. Prebuffer перед установкой

1. Не начинать фазу основной установки сразу после заголовка, пока нет минимального contiguous буфера:
   - минимум 16-32 MiB для хорошего роя;
   - 64 MiB для плохого/нестабильного роя;
   - адаптивно по `download_payload_rate / install_speed`.
2. Для NSP/NSZ отдельно учитывать, что парсинг и установка читают строго последовательно; целевой буфер должен быть в байтах, а не только в кусках.
3. В UI показывать «буферизация» вместо начала установки с пустым ring buffer.

### Этап 5. Пересмотреть приоритеты окон

1. Уменьшить или отключить Speculative при малом числе активных пиров (`active_peers <= 3`) — в логе именно такой случай.
2. Prefetch разрешать только когда:
   - нет stall;
   - current critical покрыт или уже есть пир на каждом critical куске;
   - ring buffer не пустой.
3. Для быстрых пиров держать короткую очередь ближайших кусков, для медленных — только non-critical или вообще ничего.
4. Если есть `NO_PEER_ON_PIECE`, весь bandwidth budget должен уходить в текущий и следующий кусок, а не в piece+10.

### Этап 6. RAM-cache и eviction safety

1. При eviction логировать принадлежность к зонам (`tail/critical/urgent/prefetch/speculative/old`).
2. Явно запрещать eviction `critical+urgent+tail` и последнего частично прочитанного куска.
3. Добавить counter fallback allocations у PiecePool и miss-rate в summary.
4. Если `pieces_.size() > limit`, но все куски нужны, логировать pressure и временно уменьшать prefetch/speculative вместо silent growth.

### Этап 7. Настройки discovery и подключений

1. Разделить DHT/bootstrap проблемы metadata и проблемы streaming-фазы: metadata приходит быстро, но `dht_nodes=0` на старте всё равно стоит отслеживать как отдельный warning.
2. Снизить шум от невалидных fallback-трекеров или помечать их cooldown, чтобы не тратить повторные попытки на `Host not found`.
3. Проверить пользу `allow_multiple_connections_per_ip=true`: она может помогать на NAT, но также может усиливать перекос к одному плохому endpoint.

## Приоритет внедрения

1. **P0:** session summary + log parser. Без этого невозможно объективно сравнивать улучшения.
2. **P1:** starvation recovery для `NO_PEER_ON_PIECE`.
3. **P1:** latency-mode при пустом ring buffer/stall.
4. **P2:** escalation slow-peer isolation до временного disconnect/ban.
5. **P2:** адаптивный prebuffer перед основной установкой.
6. **P3:** eviction safety counters и cleanup tracker/DHT diagnostics.

## Критерии готовности

- На текущем `log.txt` parser должен подтвердить снижение `NO_PEER_ON_PIECE` и `buffer wait` в новых прогонах.
- В хорошем/среднем рое установка не должна переходить в Stalled при каждом новом critical куске.
- При наличии одного явно плохого пира быстрые пиры должны обслуживать current piece в течение 1-2 секунд после диагностики starvation.
- Суммарная скорость не должна расти ценой пустого ring buffer: главным KPI является стабильная скорость установки, а не пиковый `download_payload_rate`.
