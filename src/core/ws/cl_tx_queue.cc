#include "cl_tx_queue.h"

#include <algorithm>

namespace recorder {

TxQueue::TxQueue() : TxQueue(Config{}) {}

TxQueue::TxQueue(const Config& cfg) : cfg_(cfg) {
  if (cfg_.max_records == 0) cfg_.max_records = 1;
  if (cfg_.max_bytes < 4096) cfg_.max_bytes = 4096;
}

bool TxQueue::HasRoomFor(std::size_t bytes_added) const {
  return q_.size() + 1 <= cfg_.max_records && bytes_ + bytes_added <= cfg_.max_bytes;
}

bool TxQueue::DropOldestAudio() {
  for (auto it = q_.begin(); it != q_.end(); ++it) {
    if (it->kind != TxRecord::Kind::kAudio) continue;
    bytes_ -= it->WireBytes();
    audio_ms_ -= static_cast<int>(it->frame_ms);
    if (audio_ms_ < 0) audio_ms_ = 0;
    q_.erase(it);
    ++stats_.audio_dropped;
    discontinuity_pending_ = true;
    return true;
  }
  return false;
}

void TxQueue::AddRecord(TxRecord&& rec) {
  bytes_ += rec.WireBytes();
  if (rec.kind == TxRecord::Kind::kAudio) {
    audio_ms_ += static_cast<int>(rec.frame_ms);
    ++stats_.audio_enqueued;
  } else {
    ++stats_.control_enqueued;
  }
  q_.push_back(std::move(rec));
  stats_.high_water_records = std::max<uint64_t>(stats_.high_water_records, q_.size());
}

bool TxQueue::PushControl(TxRecord rec) {
  RecorderLockGuard lock(mu_);
  while (!HasRoomFor(rec.WireBytes())) {
    if (!DropOldestAudio()) {
      ++stats_.control_rejected;
      return false;
    }
  }
  AddRecord(std::move(rec));
  return true;
}

bool TxQueue::PushAudio(TxRecord rec) {
  RecorderLockGuard lock(mu_);
  while (!HasRoomFor(rec.WireBytes())) {
    if (!DropOldestAudio()) {
      // The queue is entirely control records. Drop the incoming frame and
      // mark the next audio record discontinuous.
      ++stats_.audio_dropped;
      discontinuity_pending_ = true;
      return false;
    }
  }
  AddRecord(std::move(rec));
  return true;
}

bool TxQueue::Pop(TxRecord* out) {
  RecorderLockGuard lock(mu_);
  if (q_.empty()) return false;
  *out = std::move(q_.front());
  q_.pop_front();
  bytes_ -= out->WireBytes();
  if (out->kind == TxRecord::Kind::kAudio) {
    audio_ms_ -= static_cast<int>(out->frame_ms);
    if (audio_ms_ < 0) audio_ms_ = 0;
    if (discontinuity_pending_) {
      out->flags |= AudioFlag(kFlagDiscontinuity);
      discontinuity_pending_ = false;
    }
  }
  return true;
}

void TxQueue::RequeueFront(TxRecord rec) {
  RecorderLockGuard lock(mu_);
  bytes_ += rec.WireBytes();
  if (rec.kind == TxRecord::Kind::kAudio) {
    audio_ms_ += static_cast<int>(rec.frame_ms);
  }
  q_.push_front(std::move(rec));
}

void TxQueue::ClearAudio() {
  RecorderLockGuard lock(mu_);
  for (auto it = q_.begin(); it != q_.end();) {
    if (it->kind == TxRecord::Kind::kAudio) {
      bytes_ -= it->WireBytes();
      audio_ms_ -= static_cast<int>(it->frame_ms);
      it = q_.erase(it);
    } else {
      ++it;
    }
  }
  audio_ms_ = 0;
  discontinuity_pending_ = false;
}

void TxQueue::ClearAll() {
  RecorderLockGuard lock(mu_);
  q_.clear();
  bytes_ = 0;
  audio_ms_ = 0;
  discontinuity_pending_ = false;
}

void TxQueue::ResetStats() {
  RecorderLockGuard lock(mu_);
  stats_ = TxQueueStats{};
}

std::size_t TxQueue::Level() const {
  RecorderLockGuard lock(mu_);
  return q_.size();
}

std::size_t TxQueue::Bytes() const {
  RecorderLockGuard lock(mu_);
  return bytes_;
}

int TxQueue::AudioLevelMs() const {
  RecorderLockGuard lock(mu_);
  return audio_ms_;
}

TxQueueStats TxQueue::Stats() const {
  RecorderLockGuard lock(mu_);
  return stats_;
}

}  // namespace recorder
