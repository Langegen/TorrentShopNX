#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <atomic>

namespace net {

struct HttpResponse {
    int status_code = 0;
    std::string headers;
    std::string body;
};

/// Callback для потокового приёма данных.
/// Получает указатель на данные и размер, возвращает количество обработанных байт.
using StreamCallback = std::function<size_t(const void* data, size_t size)>;
using ProgressCallback = std::function<void(int64_t dltotal, int64_t dlnow)>;

/// HTTP-клиент с поддержкой Range-запросов и Keep-Alive.
/// Работает через сокеты (HTTP) или libcurl (HTTPS).
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    /// Простой GET-запрос
    HttpResponse httpGet(const std::string& url);

    /// GET с дополнительными заголовками (Range, Keep-Alive и т.д.)
    HttpResponse httpGet(const std::string& url, const std::vector<std::string>& extra_headers);

    /// POST с JSON-телом
    HttpResponse httpPost(const std::string& url, const std::string& json_body,
                          const std::vector<std::string>& extra_headers = {});

    /// Потоковый GET с Range-заголовком и callback.
    /// Данные передаются callback'у по мере получения.
    /// @param url          адрес
    /// @param offset       начальный байт (для Range)
    /// @param length       количество запрашиваемых байт (0 = до конца)
    /// @param cb           callback для обработки данных
    /// @param cancel_flag  опциональный флаг мгновенной отмены
    /// @return HTTP-код ответа (200/206 = успех)
    int httpGetStream(const std::string& url, uint64_t offset, uint64_t length,
                      StreamCallback cb, const std::atomic<bool>* cancel_flag = nullptr);

    /// Установить таймаут (секунды)
    void setTimeout(int seconds) { timeout_sec_ = seconds; }

    /// Включить/выключить Keep-Alive
    void setKeepAlive(bool enabled) { keep_alive_ = enabled; }

    /// Установить флаг отмены для вызовов
    void setCancelFlag(const std::atomic<bool>* cancel_flag) { cancel_flag_ = cancel_flag; }

    /// Установить колбэк прогресса загрузки (dltotal, dlnow)
    void setProgressCallback(ProgressCallback cb) { progress_cb_ = cb; }

private:
    HttpResponse request(const std::string& method, const std::string& url,
                         const std::string& body,
                         const std::vector<std::string>& extra_headers);

    bool parseUrl(const std::string& url, std::string& host, std::string& path, int& port);

    /// Построить Range-заголовок
    std::string buildRangeHeader(uint64_t offset, uint64_t length) const;

    int  timeout_sec_ = 30;
    bool keep_alive_  = true;
    const std::atomic<bool>* cancel_flag_ = nullptr;
    ProgressCallback progress_cb_ = nullptr;
    void* curl_handle_ = nullptr;
};

} // namespace net
