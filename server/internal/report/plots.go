package report

import (
	"os"
	"path/filepath"

	"gonum.org/v1/plot"
	"gonum.org/v1/plot/plotter"
	"gonum.org/v1/plot/vg"

	"audio-probe/server/internal/metrics"
)

// WriteSegmentPlots writes plots/waveform.png and plots/spectrum.png.
func WriteSegmentPlots(dir string, pcm []int16, sampleRate, channels int) error {
	if len(pcm) == 0 || sampleRate <= 0 || channels <= 0 {
		return nil
	}
	plotsDir := filepath.Join(dir, "plots")
	if err := os.MkdirAll(plotsDir, 0o755); err != nil {
		return err
	}
	if err := writeWaveform(filepath.Join(plotsDir, "waveform.png"), pcm, sampleRate, channels); err != nil {
		return err
	}
	return writeSpectrum(filepath.Join(plotsDir, "spectrum.png"), pcm, sampleRate)
}

func writeWaveform(path string, pcm []int16, sampleRate, channels int) error {
	frames := len(pcm) / channels
	if frames == 0 {
		return nil
	}
	const maxPoints = 2000
	step := frames / maxPoints
	if step < 1 {
		step = 1
	}
	pts := make(plotter.XYs, 0, frames/step+1)
	for i := 0; i < frames; i += step {
		t := float64(i) / float64(sampleRate)
		v := float64(pcm[i*channels]) / 32768.0
		pts = append(pts, plotter.XY{X: t, Y: v})
	}
	p := plot.New()
	p.Title.Text = "Waveform"
	p.X.Label.Text = "Time (s)"
	p.Y.Label.Text = "Amplitude (FS)"
	p.Y.Min = -1.05
	p.Y.Max = 1.05
	line, err := plotter.NewLine(pts)
	if err != nil {
		return err
	}
	p.Add(line)
	return p.Save(8*vg.Inch, 3*vg.Inch, path)
}

func writeSpectrum(path string, pcm []int16, sampleRate int) error {
	freqs, dbs := metrics.AverageSpectrum(pcm, sampleRate)
	if len(freqs) == 0 {
		return nil
	}
	pts := make(plotter.XYs, 0, len(freqs))
	for i := range freqs {
		if freqs[i] < 10 {
			continue
		}
		d := dbs[i]
		if d < -200 {
			d = -200
		}
		pts = append(pts, plotter.XY{X: freqs[i], Y: d})
	}
	p := plot.New()
	p.Title.Text = "Average spectrum"
	p.X.Label.Text = "Frequency (Hz)"
	p.Y.Label.Text = "Power (dB)"
	p.X.Min = 10
	p.X.Max = float64(sampleRate) / 2
	p.Y.Min = -140
	p.Y.Max = 20
	line, err := plotter.NewLine(pts)
	if err != nil {
		return err
	}
	p.Add(line)
	return p.Save(8*vg.Inch, 4*vg.Inch, path)
}
