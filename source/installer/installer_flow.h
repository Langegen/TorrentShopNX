#pragma once
// =============================================================================
// Единая реализация pacing + watermark потока установки.
//
// ОДИН И ТОТ ЖЕ код работает в HybridNspInstaller (Switch) и в pctest/apptest
// (PC): PC-прогон воспроизводит поведение железа, а не приблизительную копию.
//
// Смысл:
//   - watermark: перед блокирующим read() из ring buffer ждём накопления
//     low_watermark байт (кроме хвоста файла). Батчит чтение в крупные
//     последовательные записи на SD и не даёт инсталлеру мгновенно выпивать
//     буфер по 100 КБ.
//   - pacing: не даём инсталлеру стабильно обгонять сеть. Потребление
//     меряется в СЖАТЫХ байтах, вычитанных из ring buffer (для NSZ скорость
//     установки в распакованных байтах систематически обгоняет сеть в
//     1/ratio раз и задушила бы NSZ-установку).
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace installer_flow {

struct FlowConfig {
    size_t   low_watermark        = 8 * 1024 * 1024;
    int64_t  watermark_timeout_ms = 15000;
    bool     enable_pacing        = false; // Disabled: let 128MB RingBuffer naturally buffer without artificial sleep
    double   pace_ratio           = 0.92;
    int      pace_min_src_kbps    = 200;   // при неизвестной/нулевой скорости не пасуем
    int      pace_max_sleep_ms    = 3000;  // потолок на одно торможение
    int      pace_sleep_step_ms   = 100;   // шаг сна — отменяемость
};

class InstallerFlow {
public:
    explicit InstallerFlow(const FlowConfig& cfg = FlowConfig()) : cfg_(cfg) {}

    // Перед блокирующим read() из ring buffer. stream_pos/total_payload —
    // позиция и объём СЖАТОГО payload (сумма размеров entry, а не
    // total_bytes_: для NSZ total_bytes_ = распакованный размер).
    void waitForWatermark(
        uint64_t stream_pos,
        uint64_t total_payload,
        const std::function<bool()>& eof,
        const std::function<bool()>& cancel,
        const std::function<bool()>& has_error,
        const std::function<size_t()>& available,
        const std::function<void()>& sleep_step) {
        if (cancel() || has_error() || eof()) return;
        if (stream_pos >= total_payload) return;
        if (total_payload - stream_pos <= cfg_.low_watermark * 2) return; // хвост — не ждём
        if (available() >= cfg_.low_watermark) return;
        const auto wm_start = std::chrono::steady_clock::now();
        while (!cancel() && !has_error() && !eof() &&
               available() < cfg_.low_watermark) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wm_start).count();
            if (waited >= cfg_.watermark_timeout_ms) break;
            sleep_step();
        }
    }

    // После обработки порции данных. consumed — сжатые байты, вычитанные из
    // ring buffer (payload_consumed_ в инсталлере). Если потребление за
    // прошедшие >=5 с заметно выше скорости сети — спим так, чтобы средняя
    // скорость потребления сравнялась с ~pace_ratio от скорости скачивания.
    void pace(
        bool has_source,
        int src_kbps,
        uint64_t consumed,
        const std::function<bool()>& cancel,
        const std::function<bool()>& has_error,
        const std::function<void()>& sleep_step,
        const std::function<void(const std::string&)>& log_line) {
        if (!cfg_.enable_pacing || cfg_.pace_ratio <= 0.0) return;
        if (!has_source || cancel() || has_error()) return;
        if (src_kbps <= cfg_.pace_min_src_kbps) return;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - pace_start_).count();
        if (elapsed < 5.0) return;
        const uint64_t delta = consumed >= pace_start_consumed_
            ? consumed - pace_start_consumed_
            : 0;
        pace_start_ = now;
        pace_start_consumed_ = consumed;
        const double inst_kbps = elapsed > 0.0
            ? static_cast<double>(delta) / 1024.0 / elapsed
            : 0.0;
        if (inst_kbps > src_kbps / cfg_.pace_ratio) {
            double excess_ms = (1.0 - src_kbps / (inst_kbps * cfg_.pace_ratio)) * elapsed * 1000.0;
            if (excess_ms > 0.0) {
                excess_ms = std::min(excess_ms, static_cast<double>(cfg_.pace_max_sleep_ms));
                log_line("pace sleep " + std::to_string(static_cast<int>(excess_ms)) +
                         "ms cons=" + std::to_string(static_cast<int>(inst_kbps)) + "KB/s src=" +
                         std::to_string(src_kbps) + "KB/s");
                int64_t remaining_ms = static_cast<int64_t>(excess_ms);
                while (remaining_ms > 0 && !cancel() && !has_error()) {
                    sleep_step();
                    remaining_ms -= cfg_.pace_sleep_step_ms;
                }
            }
        }
    }

private:
    FlowConfig cfg_;
    std::chrono::steady_clock::time_point pace_start_ = std::chrono::steady_clock::now();
    uint64_t pace_start_consumed_ = 0;
};

} // namespace installer_flow