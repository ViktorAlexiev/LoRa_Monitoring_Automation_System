// Заместител на Arduino Wire.h (I2C). Тестовете зареждат опашка от байтове, които
// requestFrom()/read() да върнат, симулирайки отговор на конкретен I2C сензор -
// позволява да тестваме sht21_read() с точно контролирани "сурови" I2C байтове,
// вкл. случаи, в които редът MSB/LSB би имал значение.
#ifndef FAKE_WIRE_H
#define FAKE_WIRE_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>

// sht21_read() прави отделна транзакция (beginTransmission/write/endTransmission/
// requestFrom/read...) за температура и за влажност - т.е. sensors_read() извиква
// последователността НЯКОЛКО пъти. Опашка от отговори (FIFO), по един за всяка
// поредна I2C транзакция, симулира това коректно.
class TwoWireMock {
public:
  void begin() {}
  void beginTransmission(uint8_t) {}
  void write(uint8_t) {}
  uint8_t endTransmission() {
    if (!endTransmissionQueue_.empty()) {
      uint8_t r = endTransmissionQueue_.front();
      endTransmissionQueue_.pop_front();
      return r;
    }
    return 0;
  }
  uint8_t requestFrom(uint8_t, uint8_t count) {
    if (!responseQueue_.empty()) {
      currentResponse_ = responseQueue_.front();
      responseQueue_.pop_front();
    } else {
      currentResponse_.clear();
    }
    readPos_ = 0;
    uint8_t avail = (uint8_t)currentResponse_.size();
    return count < avail ? count : avail;
  }
  int available() { return (int)(currentResponse_.size() - readPos_); }
  uint8_t read() {
    if (readPos_ >= currentResponse_.size()) return 0;
    return currentResponse_[readPos_++];
  }

  // --- тестови помощни функции ---
  // добавя отговор за следващата I2C транзакция (requestFrom+read серия)
  void queueResponse(std::vector<uint8_t> bytes) { responseQueue_.push_back(bytes); }
  // добавя резултат за следващия endTransmission() (0 = ОК, различно от 0 = грешка)
  void queueEndTransmissionResult(uint8_t r) { endTransmissionQueue_.push_back(r); }
  void reset() { responseQueue_.clear(); endTransmissionQueue_.clear(); currentResponse_.clear(); readPos_ = 0; }

private:
  std::deque<std::vector<uint8_t>> responseQueue_;
  std::deque<uint8_t> endTransmissionQueue_;
  std::vector<uint8_t> currentResponse_;
  size_t readPos_ = 0;
};
extern TwoWireMock Wire;

#endif
