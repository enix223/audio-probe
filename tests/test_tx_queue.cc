#include "cl_test.h"

#include <string>

#include "cl_tx_queue.h"

namespace {

recorder::TxRecord Audio(uint32_t seq, std::size_t payload_bytes, uint16_t frame_ms = 20) {
  recorder::TxRecord r;
  r.kind = recorder::TxRecord::Kind::kAudio;
  r.format = recorder::AudioFormat::kPcmS16le;
  r.segment_id = 1;
  r.seq = seq;
  r.timestamp_ms = seq * frame_ms;
  r.samples = static_cast<uint16_t>(payload_bytes / 2);
  r.frame_ms = frame_ms;
  r.payload.assign(payload_bytes, static_cast<uint8_t>(seq & 0xFF));
  return r;
}

recorder::TxRecord Control(std::string text) {
  recorder::TxRecord r;
  r.kind = recorder::TxRecord::Kind::kControl;
  r.control = std::move(text);
  return r;
}

}  // namespace

TEST(tx_queue_preserves_fifo_and_frames) {
  recorder::TxQueue q(recorder::TxQueue::Config{16, 1u << 20});
  EXPECT_TRUE(q.PushControl(Control("start")));
  EXPECT_TRUE(q.PushAudio(Audio(0, 640)));
  EXPECT_TRUE(q.PushControl(Control("end")));

  recorder::TxRecord out;
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_TRUE(out.kind == recorder::TxRecord::Kind::kControl);
  EXPECT_EQ(out.control, std::string("start"));
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_TRUE(out.kind == recorder::TxRecord::Kind::kAudio);
  EXPECT_EQ(out.payload.size(), 640u);
  EXPECT_EQ(out.seq, 0u);
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_TRUE(out.kind == recorder::TxRecord::Kind::kControl);
  EXPECT_EQ(out.control, std::string("end"));
  EXPECT_FALSE(q.Pop(&out));
}

TEST(tx_queue_drops_oldest_audio_but_never_control) {
  recorder::TxQueue q(recorder::TxQueue::Config{4, 1u << 20});
  EXPECT_TRUE(q.PushControl(Control("hello")));
  for (uint32_t i = 0; i < 10; ++i) {
    q.PushAudio(Audio(i, 128));
  }
  const auto stats = q.Stats();
  EXPECT_TRUE(stats.audio_dropped > 0);

  recorder::TxRecord out;
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_TRUE(out.kind == recorder::TxRecord::Kind::kControl);
  EXPECT_EQ(out.control, std::string("hello"));

  // The first audio record still queued must carry DISCONTINUITY because
  // earlier audio was dropped.
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_TRUE(out.kind == recorder::TxRecord::Kind::kAudio);
  EXPECT_TRUE((out.flags & recorder::AudioFlag(recorder::kFlagDiscontinuity)) != 0);
}

TEST(tx_queue_control_is_rejected_when_only_controls_remain) {
  recorder::TxQueue q(recorder::TxQueue::Config{2, 1u << 20});
  EXPECT_TRUE(q.PushControl(Control("a")));
  EXPECT_TRUE(q.PushControl(Control("b")));
  EXPECT_FALSE(q.PushControl(Control("c")));
  EXPECT_TRUE(q.Stats().control_rejected == 1);
}

TEST(tx_queue_clear_audio_keeps_controls) {
  recorder::TxQueue q(recorder::TxQueue::Config{8, 1u << 20});
  EXPECT_TRUE(q.PushControl(Control("start")));
  EXPECT_TRUE(q.PushAudio(Audio(0, 640)));
  EXPECT_TRUE(q.PushAudio(Audio(1, 640)));
  q.ClearAudio();
  EXPECT_EQ(q.Level(), 1u);
  recorder::TxRecord out;
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_TRUE(out.kind == recorder::TxRecord::Kind::kControl);
  EXPECT_FALSE(q.Pop(&out));
}

TEST(tx_queue_audio_level_tracks_frame_duration) {
  recorder::TxQueue q(recorder::TxQueue::Config{16, 1u << 20});
  EXPECT_EQ(q.AudioLevelMs(), 0);
  EXPECT_TRUE(q.PushAudio(Audio(0, 640, 20)));
  EXPECT_TRUE(q.PushAudio(Audio(1, 640, 40)));
  EXPECT_EQ(q.AudioLevelMs(), 60);
  recorder::TxRecord out;
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_EQ(q.AudioLevelMs(), 40);
}

TEST(tx_queue_requeue_front_preserves_order) {
  recorder::TxQueue q(recorder::TxQueue::Config{8, 1u << 20});
  EXPECT_TRUE(q.PushControl(Control("a")));
  recorder::TxRecord out;
  ASSERT_TRUE(q.Pop(&out));
  q.RequeueFront(out);
  EXPECT_TRUE(q.PushControl(Control("b")));
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_EQ(out.control, std::string("a"));
  ASSERT_TRUE(q.Pop(&out));
  EXPECT_EQ(out.control, std::string("b"));
}
