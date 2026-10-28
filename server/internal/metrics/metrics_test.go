package metrics

import (
	"math"
	"testing"

	"audio-probe/server/internal/protocol"
)

const pi = 3.14159265358979323846

func sine(dbfs float64, hz, rate int, n int, dc float64) []int16 {
	amp := math.Pow(10, dbfs/20) * 32767
	out := make([]int16, n)
	for i := range out {
		v := amp*math.Sin(2*pi*float64(hz)*float64(i)/float64(rate)) + dc
		out[i] = int16(math.Round(math.Max(-32768, math.Min(32767, v))))
	}
	return out
}

func TestFrameLevels(t *testing.T) {
	pcm := sine(0, 1000, 16000, 16000, 0)
	st := AnalyzeFrame(pcm, 1)
	if math.Abs(st.RMSDBFS-(-3.01)) > 0.05 {
		t.Fatalf("rms %.2f", st.RMSDBFS)
	}
	if math.Abs(st.PeakDBFS-0.0) > 0.05 {
		t.Fatalf("peak %.2f", st.PeakDBFS)
	}
	if math.Abs(st.DCLSB) > 1 {
		t.Fatalf("dc %.3f", st.DCLSB)
	}
	if st.Samples != len(pcm) {
		t.Fatalf("samples %d", st.Samples)
	}
}

func TestFrameDCAndClip(t *testing.T) {
	pcm := sine(-6, 1000, 16000, 16000, 1000)
	st := AnalyzeFrame(pcm, 1)
	if math.Abs(st.DCLSB-1000) > 1 {
		t.Fatalf("dc %.1f", st.DCLSB)
	}
	clipped := sine(0, 1000, 16000, 16000, 0)
	for i := range clipped {
		v := int(clipped[i]) * 3
		if v < -32768 {
			v = -32768
		}
		if v > 32767 {
			v = 32767
		}
		clipped[i] = int16(v)
	}
	cs := AnalyzeFrame(clipped, 1)
	if cs.ClipPct <= 0 {
		t.Fatalf("expected clipping, got %.3f", cs.ClipPct)
	}
}

func TestPercentile(t *testing.T) {
	vals := make([]float64, 100)
	for i := range vals {
		vals[i] = float64(i)
	}
	if got := Percentile(vals, 10); math.Abs(got-10) > 0.001 {
		t.Fatalf("p10 %.3f", got)
	}
	if got := Percentile(nil, 50); got != -200 {
		t.Fatalf("empty %.1f", got)
	}
}

func TestDetectUtterances(t *testing.T) {
	rate := 16000
	var pcm []int16
	for i := 0; i < 50; i++ {
		pcm = append(pcm, make([]int16, 320)...) // 1 s silence
	}
	for i := 0; i < 50; i++ {
		pcm = append(pcm, sine(-20, 220, rate, 320, 0)...) // 1 s speech
	}
	for i := 0; i < 50; i++ {
		pcm = append(pcm, make([]int16, 320)...)
	}
	utts := DetectUtterances(pcm, rate, 1, DefaultVADConfig(rate))
	if len(utts) != 1 {
		t.Fatalf("utterances = %d", len(utts))
	}
	u := utts[0]
	if u.StartMS > 1000 || u.StartMS < 600 {
		t.Fatalf("start %.1f", u.StartMS)
	}
	if u.EndMS < 1900 || u.EndMS > 2800 {
		t.Fatalf("end %.1f", u.EndMS)
	}
	if u.SNRDB < 20 {
		t.Fatalf("snr %.1f", u.SNRDB)
	}
}

func TestSpectralMetrics(t *testing.T) {
	pcm := sine(-6, 1000, 16000, 16000, 0)
	sp := SpectralMetrics(pcm, 16000)
	if sp.FFTSize < 1024 {
		t.Fatalf("fft size %d", sp.FFTSize)
	}
	if math.Abs(sp.CentroidHz-1000) > 200 {
		t.Fatalf("centroid %.1f", sp.CentroidHz)
	}
	if sp.Flatness > 0.2 {
		t.Fatalf("flatness %.3f", sp.Flatness)
	}
	if sp.Hum50DB > -20 {
		t.Fatalf("hum50 %.1f", sp.Hum50DB)
	}

	// White-ish noise: deterministic LCG, flatness should be much higher.
	noise := make([]int16, 16000)
	state := uint32(1)
	for i := range noise {
		state = state*1664525 + 1013904223
		noise[i] = int16(state >> 16)
	}
	ns := SpectralMetrics(noise, 16000)
	if ns.Flatness < 0.2 {
		t.Fatalf("noise flatness %.3f", ns.Flatness)
	}
	if ns.Flatness <= sp.Flatness {
		t.Fatalf("noise flatness %.3f <= sine %.3f", ns.Flatness, sp.Flatness)
	}
}

func TestSegmentMetrics(t *testing.T) {
	rate := 16000
	var rows []FrameRow
	var pcm []int16
	for i := 0; i < 100; i++ {
		var chunk []int16
		flags := uint8(protocol.FlagStatsValid)
		if i >= 50 {
			chunk = sine(-20, 220, rate, 320, 0)
			flags |= protocol.FlagSpeech
		} else {
			chunk = make([]int16, 320)
		}
		pcm = append(pcm, chunk...)
		rows = append(rows, FrameRow{
			Seq: uint32(i), DeviceTSMS: uint32(i * 20), ArrivalMS: int64(i * 20),
			Bytes: 640, Flags: flags, Samples: 320,
		})
	}
	m := ComputeSegmentMetrics("cap", "pcm_s16le", rate, 1, 20, rows, pcm, 15, 0)
	if m.FramesReceived != 100 || m.FramesExpected != 100 || m.FramesLost != 0 {
		t.Fatalf("frames recv=%d expected=%d lost=%d", m.FramesReceived, m.FramesExpected, m.FramesLost)
	}
	if math.Abs(m.DurationMS-2000) > 1 {
		t.Fatalf("duration %.1f", m.DurationMS)
	}
	if m.SpeechMS < 900 || m.SpeechMS > 1100 {
		t.Fatalf("speech %.1f", m.SpeechMS)
	}
	if m.SNRDB < 20 {
		t.Fatalf("snr %.1f", m.SNRDB)
	}
	if m.NoiseFloorDBFS > -40 {
		t.Fatalf("noise %.1f", m.NoiseFloorDBFS)
	}
	if math.Abs(m.PreRollMS-300) > 1 {
		t.Fatalf("pre-roll %.1f", m.PreRollMS)
	}
}

func TestSegmentLossAccounting(t *testing.T) {
	var rows []FrameRow
	for i := 0; i < 10; i++ {
		if i == 5 {
			continue // transport loss
		}
		rows = append(rows, FrameRow{Seq: uint32(i), Samples: 320})
	}
	pcm := make([]int16, len(rows)*320)
	m := ComputeSegmentMetrics("cap", "pcm_s16le", 16000, 1, 20, rows, pcm, 0, 0)
	if m.FramesLost != 1 {
		t.Fatalf("lost %d", m.FramesLost)
	}
	// A discontinuity frame explains the gap as a device drop, not loss.
	rows = nil
	for i := 0; i < 10; i++ {
		if i == 5 {
			continue
		}
		flags := uint8(0)
		if i == 6 {
			flags = protocol.FlagDiscontinuity
		}
		rows = append(rows, FrameRow{Seq: uint32(i), Samples: 320, Flags: flags})
	}
	pcm = make([]int16, len(rows)*320)
	m = ComputeSegmentMetrics("cap", "pcm_s16le", 16000, 1, 20, rows, pcm, 0, 0)
	if m.FramesLost != 0 {
		t.Fatalf("discontinuity counted as loss: %d", m.FramesLost)
	}
}
