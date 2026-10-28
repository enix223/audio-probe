package analysis

import (
	"math"
	"testing"
)

const pi = 3.14159265358979323846

func sine(hz, rate int, n int, ampDB float64, harmonicDB float64) []int16 {
	amp := math.Pow(10, ampDB/20) * 32767
	var hAmp float64
	if !math.IsInf(harmonicDB, 0) {
		hAmp = math.Pow(10, harmonicDB/20) * 32767
	}
	out := make([]int16, n)
	for i := range out {
		v := amp * math.Sin(2*pi*float64(hz)*float64(i)/float64(rate))
		if hAmp != 0 {
			v += hAmp * math.Sin(2*pi*2*float64(hz)*float64(i)/float64(rate))
		}
		out[i] = int16(math.Round(math.Max(-32768, math.Min(32767, v))))
	}
	return out
}

func TestAnalyzePureTone(t *testing.T) {
	pcm := sine(1000, 16000, 16000, -6, math.Inf(1))
	r := AnalyzeTone(pcm, 16000, 1000)
	if r.NoSignal {
		t.Fatal("no signal")
	}
	if math.Abs(r.MeasuredHz-1000) > 1 {
		t.Fatalf("measured %.2f", r.MeasuredHz)
	}
	if math.Abs(r.AmplitudeDBFS-(-6)) > 0.5 {
		t.Fatalf("amplitude %.2f", r.AmplitudeDBFS)
	}
	if r.THDDb > -55 {
		t.Fatalf("thd %.1f", r.THDDb)
	}
	if r.SFDRDb < 50 {
		t.Fatalf("sfdr %.1f", r.SFDRDb)
	}
}

func TestAnalyzeToneFrequencyError(t *testing.T) {
	pcm := sine(1001, 16000, 16000, -6, math.Inf(1))
	r := AnalyzeTone(pcm, 16000, 1000)
	if math.Abs(r.FrequencyErrorPPM-1000) > 150 {
		t.Fatalf("ppm %.1f measured %.2f", r.FrequencyErrorPPM, r.MeasuredHz)
	}
}

func TestAnalyzeToneHarmonic(t *testing.T) {
	pcm := sine(1000, 16000, 16000, -6, -46)
	r := AnalyzeTone(pcm, 16000, 1000)
	if r.NoSignal {
		t.Fatal("no signal")
	}
	// The second harmonic at -40 dB relative is mixed at -6 dBFS, so the
	// harmonic is -40 dB below the fundamental.
	if math.Abs(r.THDDb-(-40)) > 3 {
		t.Fatalf("thd %.1f", r.THDDb)
	}
	if r.THDNDb > r.THDDb+1 {
		t.Fatalf("thd+n %.1f worse than thd %.1f", r.THDNDb, r.THDDb)
	}
}

func TestGainSweepComparison(t *testing.T) {
	steps := []GainStep{
		{Label: "g0", GainDB: 0, RMSDBFS: -40, SNRDB: 20},
		{Label: "g12", GainDB: 12, RMSDBFS: -28, SNRDB: 30},
		{Label: "g24", GainDB: 24, RMSDBFS: -16, SNRDB: 40},
		{Label: "g36", GainDB: 36, RMSDBFS: -4, ClipPct: 2.0, SNRDB: 38},
	}
	res := CompareGainSteps(steps)
	if !res.Monotonic {
		t.Fatal("expected monotonic")
	}
	if math.Abs(res.SlopeDBPerDB-1.0) > 0.05 {
		t.Fatalf("slope %.3f", res.SlopeDBPerDB)
	}
	if res.ClippingGainDB == nil || *res.ClippingGainDB != 36 {
		t.Fatalf("clipping gain %v", res.ClippingGainDB)
	}
	if res.BestSNRGainDB == nil || *res.BestSNRGainDB != 24 {
		t.Fatalf("best gain %v", res.BestSNRGainDB)
	}
}
