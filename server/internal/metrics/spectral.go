package metrics

import (
	"math"

	"gonum.org/v1/gonum/dsp/fourier"
)

// SpectralResult holds the averaged-spectrum metrics for a segment.
type SpectralResult struct {
	FFTSize       int                `json:"fft_size"`
	Window        string             `json:"window"`
	CentroidHz    float64            `json:"centroid_hz"`
	Flatness      float64            `json:"flatness"`
	Rolloff85Hz   float64            `json:"rolloff_85_hz"`
	Hum50DB       float64            `json:"hum_50_db"`
	Hum60DB       float64            `json:"hum_60_db"`
	HighFreqRatio float64            `json:"high_freq_ratio"`
	BandEnergies  map[string]float64 `json:"band_energies"`
}

// SpectralMetrics averages Hann-windowed FFT power spectra with 50% overlap.
func SpectralMetrics(pcm []int16, sampleRate int) SpectralResult {
	out := SpectralResult{Window: "hann", BandEnergies: map[string]float64{}}
	if len(pcm) < 256 || sampleRate <= 0 {
		out.Hum50DB = -200
		out.Hum60DB = -200
		return out
	}
	n := nextPow2(2048)
	if n > len(pcm) {
		n = nextPow2(len(pcm))
	}
	if n < 256 {
		n = 256
	}
	if n > 8192 {
		n = 8192
	}
	out.FFTSize = n
	frame := make([]float64, n)
	window := hann(n)
	fft := fourier.NewFFT(n)
	bins := n/2 + 1
	accum := make([]float64, bins)
	frames := 0
	hop := n / 2
	for start := 0; start+n <= len(pcm); start += hop {
		for i := 0; i < n; i++ {
			frame[i] = float64(pcm[start+i]) / 32768.0 * window[i]
		}
		coeff := fft.Coefficients(nil, frame)
		for i := 0; i < bins && i < len(coeff); i++ {
			re := real(coeff[i])
			im := imag(coeff[i])
			accum[i] += re*re + im*im
		}
		frames++
	}
	if frames == 0 {
		return out
	}
	total := 0.0
	for i := 1; i < bins; i++ {
		accum[i] /= float64(frames)
		total += accum[i]
	}
	if total <= 0 {
		return out
	}
	binHz := float64(sampleRate) / float64(n)

	weighted, powerSum := 0.0, 0.0
	logSum := 0.0
	nonZero := 0
	for i := 1; i < bins; i++ {
		p := accum[i]
		f := float64(i) * binHz
		weighted += f * p
		powerSum += p
		if p > 0 {
			logSum += math.Log(p)
			nonZero++
		}
	}
	if powerSum > 0 {
		out.CentroidHz = weighted / powerSum
	}
	if nonZero > 0 {
		geo := math.Exp(logSum / float64(nonZero))
		arith := powerSum / float64(bins-1)
		if arith > 0 {
			out.Flatness = geo / arith
		}
	}

	cum := 0.0
	for i := 1; i < bins; i++ {
		cum += accum[i]
		if cum >= 0.85*total {
			out.Rolloff85Hz = float64(i) * binHz
			break
		}
	}

	out.Hum50DB = humDB(accum, binHz, 50, total)
	out.Hum60DB = humDB(accum, binHz, 60, total)

	hf := 0.0
	for i := 1; i < bins; i++ {
		if float64(i)*binHz >= 4000 {
			hf += accum[i]
		}
	}
	out.HighFreqRatio = hf / total

	bands := []struct {
		name string
		lo   float64
		hi   float64
	}{
		{"0-300", 0, 300},
		{"300-1000", 300, 1000},
		{"1k-4k", 1000, 4000},
		{"4k-nyquist", 4000, math.Inf(1)},
	}
	for _, b := range bands {
		e := 0.0
		for i := 1; i < bins; i++ {
			f := float64(i) * binHz
			if f >= b.lo && f < b.hi {
				e += accum[i]
			}
		}
		out.BandEnergies[b.name] = e / total
	}
	return out
}

// AverageSpectrum returns frequency (Hz) and power (dB) arrays for plotting.
func AverageSpectrum(pcm []int16, sampleRate int) ([]float64, []float64) {
	if len(pcm) < 256 || sampleRate <= 0 {
		return nil, nil
	}
	n := nextPow2(2048)
	if n > len(pcm) {
		n = nextPow2(len(pcm))
	}
	if n < 256 {
		n = 256
	}
	if n > 8192 {
		n = 8192
	}
	window := hann(n)
	frame := make([]float64, n)
	fft := fourier.NewFFT(n)
	bins := n/2 + 1
	accum := make([]float64, bins)
	frames := 0
	hop := n / 2
	for start := 0; start+n <= len(pcm); start += hop {
		for i := 0; i < n; i++ {
			frame[i] = float64(pcm[start+i]) / 32768.0 * window[i]
		}
		coeff := fft.Coefficients(nil, frame)
		for i := 0; i < bins && i < len(coeff); i++ {
			accum[i] += real(coeff[i])*real(coeff[i]) + imag(coeff[i])*imag(coeff[i])
		}
		frames++
	}
	if frames == 0 {
		return nil, nil
	}
	binHz := float64(sampleRate) / float64(n)
	freqs := make([]float64, bins)
	dbs := make([]float64, bins)
	for i := 0; i < bins; i++ {
		accum[i] /= float64(frames)
		freqs[i] = float64(i) * binHz
		dbs[i] = 10 * math.Log10(accum[i]+1e-20)
	}
	return freqs, dbs
}

func humDB(power []float64, binHz, hz, total float64) float64 {
	if total <= 0 || binHz <= 0 {
		return -200
	}
	center := int(math.Round(hz / binHz))
	best := 0.0
	for i := center - 1; i <= center+1; i++ {
		if i <= 0 || i >= len(power) {
			continue
		}
		if power[i] > best {
			best = power[i]
		}
	}
	if best <= 0 {
		return -200
	}
	return 10 * math.Log10(best/total)
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
