#ifndef ANALYZER_SPECTROGRAM_H
#define ANALYZER_SPECTROGRAM_H

#include <cstddef>
#include <vector>

class Spectrogram {
public:
    explicit Spectrogram(size_t history_size = 100);

    /**
     * Добавить новый срез данных.
     * Если длина среза изменилась — производится resize всей спектрограммы.
     * @return false, если spectrum пуст
     */
    void clear();
    bool push(const std::vector<float> &spectrum);

    /**
     *  Изменить глубину истории без сброса данных:
     * - увеличение: добавляет нули в начало (старые позиции)
     * - уменьшение: отбрасывает наиболее старые срезы
     * @return false, если new_size == 0.
     */
    bool set_history_size(size_t new_size);

    // Получить временной ряд одного канала [0..channels-1], от старых к новым.
    // Если channel_index >= channels(), возвращает пустой вектор.
    std::vector<float> get_channel_history(size_t channel_index) const;

    // Получить всю спектрограмму: [channel][time], от старых к новым.
    std::vector<std::vector<float> > get_spectrogram() const;

    // Получить временной ряд одного канала [0..channels-1], от новых к старым.
    // Если channel_index >= channels(), возвращает пустой вектор.
    std::vector<float> get_channel_history_reversed(size_t channel_index) const;

    // Получить всю спектрограмму: [channel][time], от новых к старым.
    std::vector<std::vector<float> > get_spectrogram_reversed() const;

    size_t history_size() const; // максимальный размер истории
    size_t channels() const; // число каналов (длина последнего среза)
    size_t filled_count() const; // реально заполненных срезов

    bool is_full() const;


    std::vector<std::vector<float> > get_vector_spectrogram() const;

private:
    // Пересоздать data_ под новое число каналов, данные сбрасываются в ноль.
    void reset(size_t new_channels);

    // Скопировать данные из кольцевого буфера в линейный порядок (старые→новые)
    // для одного канала в output[0..history_size_-1].
    void unwrap_channel(size_t channel_index, std::vector<float> &output) const;

    size_t history_size_;
    size_t channels_;
    size_t write_index_; // следующая позиция записи в кольцевом буфере
    size_t filled_count_; // сколько позиций реально заполнено

    // Транспонированное хранение: data_[channel][time_slot]
    std::vector<std::vector<float> > data_;
};

#endif // ANALYZER_SPECTROGRAM_H
