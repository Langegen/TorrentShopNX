# Сервер отчётов об ошибках (VPS)

Сервер для кнопки «Отправить разработчику» (Настройки → «Отчёт об ошибке»).
Приложение отправляет на него ZIP с логами, а он пересылает архив в ваш
закрытый Telegram-чат с подписью:

```
🐞 R-7F3A2C от @username
TorrentShopNX 2.21 · FW 23.0.1 · ru
Прошлый запуск: unexpected · 84 КБ

<описание проблемы от пользователя>
```

`@username` — Telegram, который пользователь указал в отчёте (необязательно).
По нему можно ответить человеку. Код отчёта пользователь видит после отправки
и может написать его в теме ошибок https://t.me/TorrentShopNX/553.

- Один файл `server.py`, Python 3.8+ и только стандартная библиотека.
- **Хранение.** Каждый отчёт лежит в `/var/lib/tsnx-reports` не дольше
  `RETENTION_HOURS` (по умолчанию 72 часа), потом удаляется, даже если не
  дошёл до Telegram. Проверка идёт каждые 5 минут.
- **Если Telegram недоступен**, сервер всё равно отвечает приложению кодом
  отчёта и повторяет доставку каждые 5 минут, пока отчёт не удалён.
- IP-адреса не записываются в журнал. Не больше 3 отчётов в минуту с одного
  адреса и 30 в минуту всего; архив до 10 МБ, только ZIP от приложения.
- Токен бота хранится только в `/etc/tsnx-reports.env` на сервере.

Второй способ, «Отправить самому в Telegram» (QR → телефон → тема ошибок),
работает без этого сервера.

## Установка

Команды для Debian/Ubuntu с systemd.

1. **Бот и чат.** В Telegram откройте @BotFather → `/newbot` и сохраните
   токен. Создайте закрытую группу (или используйте личный чат с ботом),
   добавьте туда бота и напишите любое сообщение. Не используйте публичную
   группу: в логах видно, какие игры качал пользователь. Откройте
   `https://api.telegram.org/bot<ТОКЕН>/getUpdates` и найдите
   `"chat":{"id":...}` (у групп id отрицательный, например `-1001234567890`).

2. **Файлы.** Скопируйте папку на VPS и разложите:

   ```bash
   sudo mkdir -p /opt/tsnx-reports
   sudo cp server.py /opt/tsnx-reports/
   sudo install -m 600 tsnx-reports.env.example /etc/tsnx-reports.env
   sudo nano /etc/tsnx-reports.env
   ```

   Впишите `TELEGRAM_BOT_TOKEN` и `TELEGRAM_CHAT_ID`, при желании поменяйте
   `RETENTION_HOURS` (48–96).

3. **Сервис.**

   ```bash
   sudo cp tsnx-reports.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now tsnx-reports
   journalctl -u tsnx-reports -f
   ```

4. **HTTPS (рекомендуется).** Логи лучше передавать зашифрованными. Если у
   VPS есть домен, проще всего поставить Caddy: он сам получит сертификат.
   Ключ нужен **RSA**: по умолчанию Caddy выпускает ECDSA, а SSL-служба
   Switch с такими сертификатами работает ненадёжно (см. комментарий к
   `imageProxyFallback` в `source/GameData.hpp`). `/etc/caddy/Caddyfile`:

   ```
   {
       key_type rsa2048
   }

   reports.example.com {
       reverse_proxy 127.0.0.1:8787
   }
   ```

   Затем `sudo systemctl reload caddy`. Адрес для приложения:
   `https://reports.example.com/v1/report`.

   **Без домена** поставьте в `/etc/tsnx-reports.env` `LISTEN_HOST=0.0.0.0` и
   `TRUST_PROXY=0`, откройте порт (`sudo ufw allow 8787/tcp`). Адрес для
   приложения: `http://<IP-VPS>:8787/v1/report`. Работает, но архив идёт
   по сети без шифрования.

5. **Проверка.**

   ```bash
   curl https://reports.example.com/health
   curl -X POST -H "X-TSNX-Version: test" -H "X-TSNX-Contact: %40durov" \
        --data-binary @test.zip https://reports.example.com/v1/report
   ```

   В ответ придёт `{"ok": true, "id": "R-..."}`, а архив появится в чате.

6. **Адрес в приложении.** Впишите адрес из шага 4 в `TSNX_REPORT_URL` в
   `source/utils/bug_report.cpp` (сейчас используется
   `http://177.3.213.173:8787/v1/report`) и пересоберите приложение. Пока адрес
   пустой, кнопка «Отправить разработчику» скрыта. В PC-сборке адрес можно
   подставить без пересборки через переменную окружения `TSNX_REPORT_URL`.

## Обновление

```bash
sudo cp server.py /opt/tsnx-reports/ && sudo systemctl restart tsnx-reports
```

## Протокол

`POST /v1/report`, тело — ZIP.

| Заголовок | Значение |
|---|---|
| `X-TSNX-Version` | версия приложения (обязателен) |
| `X-TSNX-Firmware` | прошивка консоли или `pc` |
| `X-TSNX-Lang` | язык интерфейса |
| `X-TSNX-Prev` | как завершился прошлый запуск: `clean`, `fatal`, `unexpected`, `none` |
| `X-TSNX-Contact` | Telegram пользователя `@username` в percent-encoding, может быть пустым |
| `X-TSNX-Message` | описание проблемы в percent-encoding, до 600 символов |

Ответы: `200 {"ok": true, "id": "R-XXXXXX"}`; ошибки — `{"ok": false, "error": ...}`
с кодами `400 bad_request/not_a_zip`, `411 length_required`, `413 too_large`,
`429 rate_limited`, `500 server_not_configured`.
