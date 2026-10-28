// Package analysis implements test-tone and sweep measurements.
package analysis

import (
	"math"

	"gonum.org/v1/gonum/dsp/fourier"
)

// ToneResult is the tone block in metrics.json (docs/audio-quality-metrics.md §10).
type ToneResult struct {
	ToneID            string     `json:"tone_id"`
	Waveform          string     `json:"waveform,omitempty"`
	ExpectedHz        float64    `json:"expected_hz"`
	MeasuredHz        float64    `json:"measured_hz"`
	FrequencyErrorPPM float64    `json:"frequency_error_ppm"`
	AmplitudeDBFS     float64    `json:"amplitude_dbfs"`
	THDDb             float64    `json:"thd_db"`
	THDNDb            float64    `json:"thd_n_db"`
	SFDRDb            float64    `json:"sfdr_db"`
	AnalysisBandHz    [2]float64 `json:"analysis_band_hz"`
	FFTSize           int        `json:"fft_size"`
	Window            string     `json:"window"`
	Harmonics         int        `json:"harmonics"`
	NoSignal          bool       `json:"no_signal,omitempty"`
}

// AnalyzeTone measures a steady sine. `expectedHz` selects the search window;
// pass 0 to use the strongest bin in the analysis band.
func AnalyzeTone(pcm []int16, sampleRate int, expectedHz float64) ToneResult {
	res := ToneResult{
		ExpectedHz:     expectedHz,
		AnalysisBandHz: [2]float64{20, 0},
		Window:         "hann",
		Harmonics:      10,
	}
	if sampleRate <= 0 || len(pcm) < 256 {
		res.NoSignal = true
		res.MeasuredHz = 0
		res.AmplitudeDBFS = -200
		res.THDDb = -200
		res.THDNDb = -200
		res.SFDRDb = -200
		return res
	}

	// Use the first channel; interleaved multi-channel stimuli are analyzed
	// per the same fundamental.
	n := nextPow2(4096)
	if n > len(pcm) {
		n = nextPow2(len(pcm))
	}
	if n < 1024 {
		n = 1024
	}
	if n > 32768 {
		n = 32768
	}
	res.FFTSize = n
	res.AnalysisBandHz[1] = float64(sampleRate) / 2

	window := hann(n)
	frame := make([]float64, n)
	for i := 0; i < n; i++ {
		frame[i] = float64(pcm[i]) / 32768.0 * window[i]
	}
	fft := fourier.NewFFT(n)
	coeff := fft.Coefficients(nil, frame)
	bins := n/2 + 1
	power := make([]float64, bins)
	for i := 0; i < bins && i < len(coeff); i++ {
		power[i] = real(coeff[i])*real(coeff[i]) + imag(coeff[i])*imag(coeff[i])
	}
	binHz := float64(sampleRate) / float64(n)

	minBin := int(math.Ceil(res.AnalysisBandHz[0] / binHz))
	if minBin < 1 {
		minBin = 1
	}
	maxBin := int(math.Floor(res.AnalysisBandHz[1] * 0.95 / binHz))
	if maxBin >= bins {
		maxBin = bins - 1
	}
	if maxBin <= minBin {
		res.NoSignal = true
		return res
	}

	peakBin := minBin
	if expectedHz > 0 {
		center := int(math.Round(expectedHz / binHz))
		search := 6
		lo := center - search
		hi := center + search
		if lo < minBin {
			lo = minBin
		}
		if hi > maxBin {
			hi = maxBin
		}
		if hi >= lo {
			peakBin = lo
			for i := lo; i <= hi; i++ {
				if power[i] > power[peakBin] {
					peakBin = i
				}
			}
		}
	} else {
		for i := minBin; i <= maxBin; i++ {
			if power[i] > power[peakBin] {
				peakBin = i
			}
		}
	}

	// Parabolic interpolation on log magnitude.
	delta := 0.0
	if peakBin > 0 && peakBin+1 < bins {
		a := math.Log(power[peakBin-1] + 1e-30)
		b := math.Log(power[peakBin] + 1e-30)
		c := math.Log(power[peakBin+1] + 1e-30)
		den := a - 2*b + c
		if math.Abs(den) > 1e-20 {
			delta = 0.5 * (a - c) / den
			if delta > 0.5 {
				delta = 0.5
			} else if delta < -0.5 {
				delta = -0.5
			}
		}
	}
	res.MeasuredHz = (float64(peakBin) + delta) * binHz
	if expectedHz > 0 {
		res.FrequencyErrorPPM = (res.MeasuredHz - expectedHz) / expectedHz * 1e6
	}

	// Hann coherent gain: sum(window)/2 = N/4 for the positive-frequency
	// component; amplitude = 2*|X|/sum(window).
	sumWindow := 0.0
	for _, w := range window {
		sumWindow += w
	}
	amp := 2 * math.Sqrt(power[peakBin]) / sumWindow
	res.AmplitudeDBFS = 20 * math.Log10(amp+1e-12)

	// Fundamental power: main lobe around the peak.
	pFund := 0.0
	for i := peakBin - 2; i <= peakBin+2; i++ {
		if i > 0 && i < bins {
			pFund += power[i]
		}
	}
	pTotal := 0.0
	for i := minBin; i <= maxBin; i++ {
		pTotal += power[i]
	}
	pHarm := 0.0
	if res.MeasuredHz > 0 {
		for h := 2; h <= res.Harmonics; h++ {
			f := res.MeasuredHz * float64(h)
			if f >= res.AnalysisBandHz[1]*0.95 {
				break
			}
			hb := int(math.Round(f / binHz))
			for i := hb - 2; i <= hb+2; i++ {
				if i > 0 && i < bins {
					pHarm += power[i]
				}
			}
		}
	}
	if pFund > 0 {
		thd := math.Sqrt(pHarm) / math.Sqrt(pFund)
		res.THDDb = 20 * math.Log10(thd+1e-12)
		thdn := math.Sqrt(math.Max(pTotal-pFund, 0)) / math.Sqrt(pFund)
		res.THDNDb = 20 * math.Log10(thdn+1e-12)
	} else {
		res.THDDb = -200
		res.THDNDb = -200
		res.NoSignal = true
	}

	// SFDR: strongest spur outside the fundamental's main lobe.
	spur := 0.0
	for i := minBin; i <= maxBin; i++ {
		if i >= peakBin-3 && i <= peakBin+3 {
			continue
		}
		if power[i] > spur {
			spur = power[i]
		}
	}
	if spur > 0 && pFund > 0 {
		res.SFDRDb = 20 * math.Log10(math.Sqrt(pFund)/math.Sqrt(spur)+1e-12)
	} else {
		res.SFDRDb = -200
	}

	// Amplitude sanity: a pure sine at -6 dBFS should read ~-6 dBFS.
	if res.AmplitudeDBFS < -120 {
		res.NoSignal = true
	}
	return res
}

// GainStep is one labeled capture in a server-orchestrated gain sweep.
type GainStep struct {
	Label     string      `json:"label"`
	GainDB    float64     `json:"gain_db"`
	RMSDBFS   float64     `json:"rms_dbfs"`
	PeakDBFS  float64     `json:"peak_dbfs"`
	ClipPct   float64     `json:"clip_pct"`
	NoiseDBFS float64     `json:"noise_floor_dbfs"`
	SNRDB     float64     `json:"snr_db"`
	Tone      *ToneResult `json:"tone,omitempty"`
}

// GainSweepResult compares gain steps: monotonic level, clipping point, and
// the best-SNR gain without clipping.
type GainSweepResult struct {
	Steps          []GainStep `json:"steps"`
	SlopeDBPerDB   float64    `json:"slope_db_per_db"`
	Monotonic      bool       `json:"monotonic"`
	ClippingGainDB *float64   `json:"clipping_gain_db,omitempty"`
	BestSNRGainDB  *float64   `json:"best_snr_gain_db,omitempty"`
}

// CompareGainSteps computes the sweep summary. Steps must be in increasing
// gain order.
func CompareGainSteps(steps []GainStep) GainSweepResult {
	res := GainSweepResult{Steps: steps}
	if len(steps) < 2 {
		return res
	}
	res.Monotonic = true
	for i := 1; i < len(steps); i++ {
		if steps[i].RMSDBFS+0.2 < steps[i-1].RMSDBFS {
			res.Monotonic = false
		}
		if steps[i].ClipPct > 0.1 && res.ClippingGainDB == nil {
			g := steps[i].GainDB
			res.ClippingGainDB = &g
		}
	}
	first := steps[0]
	last := steps[len(steps)-1]
	dg := last.GainDB - first.GainDB
	if math.Abs(dg) > 1e-9 {
		res.SlopeDBPerDB = (last.RMSDBFS - first.RMSDBFS) / dg
	}
	best := steps[0]
	for _, s := range steps {
		if s.ClipPct <= 0.1 && s.SNRDB > best.SNRDB {
			best = s
		}
	}
	g := best.GainDB
	res.BestSNRGainDB = &g
	return res
}

func hann(n int) []float64 {
	w := make([]float64, n)
	for i := 0; i < n; i++ {
		w[i] = 0.5 - 0.5*math.Cos(2*math.Pi*float64(i)/float64(n-1))
	}
	return w
}

func nextPow2(n int) int {
	p := 1
	for p < n {
		p <<= 1
	}
	return p
}
