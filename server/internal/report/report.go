// Package report writes the human- and machine-readable capture reports.
package report

import (
	"encoding/csv"
	"fmt"
	"html/template"
	"os"
	"path/filepath"
	"strconv"
)

// SegmentRow is one capture summary row.
type SegmentRow struct {
	SegmentID  uint32
	Label      string
	Reason     string
	Format     string
	DurationMS float64
	SpeechMS   float64
	PreRollMS  float64
	Frames     int
	Lost       int
	LossPct    float64
	RMSDBFS    float64
	PeakDBFS   float64
	ClipPct    float64
	NoiseDBFS  float64
	SNRDB      float64
	THDDb      float64
	THDNDb     float64
}

func WriteSummaryCSV(dir string, rows []SegmentRow) error {
	path := filepath.Join(dir, "summary.csv")
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	w := csv.NewWriter(f)
	defer w.Flush()
	header := []string{"segment_id", "label", "reason", "format", "duration_ms", "speech_ms",
		"pre_roll_ms", "frames", "frames_lost", "loss_pct", "rms_dbfs", "peak_dbfs",
		"clip_pct", "noise_dbfs", "snr_db", "thd_db", "thd_n_db"}
	if err := w.Write(header); err != nil {
		return err
	}
	for _, r := range rows {
		rec := []string{
			strconv.FormatUint(uint64(r.SegmentID), 10),
			r.Label, r.Reason, r.Format,
			f2(r.DurationMS), f2(r.SpeechMS), f2(r.PreRollMS),
			strconv.Itoa(r.Frames), strconv.Itoa(r.Lost), f2(r.LossPct),
			f2(r.RMSDBFS), f2(r.PeakDBFS), f2(r.ClipPct), f2(r.NoiseDBFS), f2(r.SNRDB),
			f2(r.THDDb), f2(r.THDNDb),
		}
		if err := w.Write(rec); err != nil {
			return err
		}
	}
	return w.Error()
}

func f2(v float64) string { return strconv.FormatFloat(v, 'f', 2, 64) }

var htmlTemplate = template.Must(template.New("report").Parse(`<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>{{.Title}}</title>
<style>
body{font-family:system-ui,sans-serif;margin:2rem;color:#222;background:#fafafa}
h1{margin-bottom:.2rem} table{border-collapse:collapse;margin:1rem 0;background:#fff}
th,td{border:1px solid #ddd;padding:.35rem .55rem;font-size:.9rem;text-align:right}
th{background:#f0f0f0} td:first-child,th:first-child{text-align:left}
.meta{display:grid;grid-template-columns:max-content 1fr;gap:.2rem 1rem;font-size:.9rem}
.note{color:#8a6d3b;background:#fcf8e3;padding:.5rem;border-radius:4px}
a{color:#075ea8} img{max-width:100%;border:1px solid #ddd;background:#fff}
</style></head><body>
<h1>{{.Title}}</h1>
{{if .Notes}}<div class="note">{{range .Notes}}{{.}}<br>{{end}}</div>{{end}}
<h2>Capture</h2><div class="meta">{{range .Meta}}<b>{{.Key}}</b><span>{{.Value}}</span>{{end}}</div>
<h2>Segments</h2>
<table><tr><th>#</th><th>Label</th><th>Reason</th><th>Format</th><th>Duration</th>
<th>Lost</th><th>RMS</th><th>Peak</th><th>Clip%</th><th>Noise</th><th>SNR</th>
<th>THD</th><th>THD+N</th><th>Files</th></tr>
{{range .Rows}}<tr>
<td>{{.SegmentID}}</td><td>{{.Label}}</td><td>{{.Reason}}</td><td>{{.Format}}</td>
<td>{{printf "%.1f ms" .DurationMS}}</td><td>{{.Lost}}</td>
<td>{{printf "%.1f" .RMSDBFS}}</td><td>{{printf "%.1f" .PeakDBFS}}</td>
<td>{{printf "%.2f" .ClipPct}}</td><td>{{printf "%.1f" .NoiseDBFS}}</td>
<td>{{printf "%.1f" .SNRDB}}</td><td>{{printf "%.1f" .THDDb}}</td><td>{{printf "%.1f" .THDNDb}}</td>
<td><a href="segments/{{.SegmentID}}/audio.wav">wav</a>
<a href="segments/{{.SegmentID}}/metrics.json">metrics</a>
<a href="segments/{{.SegmentID}}/frames.csv">frames</a>
<a href="segments/{{.SegmentID}}/plots/waveform.png">wave</a>
<a href="segments/{{.SegmentID}}/plots/spectrum.png">spectrum</a></td>
</tr>{{end}}</table>
<p style="color:#666;font-size:.85rem">Reports are generated from server-recomputed
metrics. Opus captures are lossy; THD/THD+N and frequency response from Opus are not
chip-grade. See metrics.json for analysis parameters.</p>
</body></html>`))

type MetaRow struct {
	Key   string
	Value string
}

type htmlData struct {
	Title string
	Meta  []MetaRow
	Rows  []SegmentRow
	Notes []string
}

// WriteHTML renders report.html. `meta` values are stringified.
func WriteHTML(dir, title string, meta map[string]any, rows []SegmentRow, notes []string) error {
	data := htmlData{Title: title, Rows: rows, Notes: notes}
	keys := make([]string, 0, len(meta))
	for k := range meta {
		keys = append(keys, k)
	}
	// Stable, readable order: insertion is not available in a map, so sort.
	for i := 0; i < len(keys); i++ {
		for j := i + 1; j < len(keys); j++ {
			if keys[j] < keys[i] {
				keys[i], keys[j] = keys[j], keys[i]
			}
		}
	}
	for _, k := range keys {
		data.Meta = append(data.Meta, MetaRow{Key: k, Value: fmt.Sprint(meta[k])})
	}
	f, err := os.Create(filepath.Join(dir, "report.html"))
	if err != nil {
		return err
	}
	defer f.Close()
	return htmlTemplate.Execute(f, data)
}
