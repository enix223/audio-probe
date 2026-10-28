// Package metrics computes the audio-quality measurements defined in
// docs/audio-quality-metrics.md from received PCM. It never trusts device
// header statistics for quality claims.
package metrics

import (
	"math"
	"sort"

	"audio-probe/server/internal/analysis"
	"audio-probe/server/internal/protocol"
)

// FrameStats is one frame's server-computed levels.
type FrameStats struct {
	Samples    int     `json:"samples"`
	RMSDBFS    float64 `json:"rms_dbfs"`
	PeakDBFS   float64 `json:"peak_dbfs"`
	PeakLinear float64 `json:"peak_linear"`
	DCLSB      float64 `json:"dc_offset_lsb"`
	ClipPct    float64 `json:"clip_pct"`
	ZCR        float64 `json:"zcr"`
}

// FrameRow is the per-frame sidecar row written to frames.csv.
type FrameRow struct {
	Seq        uint32
	DeviceTSMS uint32
	ArrivalMS  int64
	Bytes      int
	Flags      uint8
	Samples    int
	RMSDBFS    float64
	PeakDBFS   float64
	DCLSB      float64
	ClipPct    float64
	ZCR        float64
	DeviceRMS  int16
	DevicePeak uint16
	DeviceDC   int16
	DeviceVAD  uint8
}

// AnalyzeFrame computes levels over all channels.
func AnalyzeFrame(pcm []int16, channels int) FrameStats {
	var out FrameStats
	if len(pcm) == 0 || channels <= 0 {
		out.RMSDBFS = -200
		out.PeakDBFS = -200
		return out
	}
	n := len(pcm)
	var sum, sumSq float64
	peak := 0
	clipped := 0
	crossings := 0
	prev := pcm[0]
	for i, v := range pcm {
		f := float64(v)
		sum += f
		sumSq += f * f
		a := v
		if a < 0 {
			a = -a
		}
		if int(a) > peak {
			peak = int(a)
		}
		if a >= 32760 {
			clipped++
		}
		if i > 0 && (v < 0) != (prev < 0) {
			crossings++
		}
		prev = v
	}
	rms := math.Sqrt(sumSq/float64(n)) / 32768.0
	out.Samples = n / channels
	out.RMSDBFS = dbfs(rms)
	out.PeakLinear = float64(peak) / 32768.0
	out.PeakDBFS = dbfs(out.PeakLinear)
	out.DCLSB = sum / float64(n)
	if n > 0 {
		out.ClipPct = 100 * float64(clipped) / float64(n)
		out.ZCR = float64(crossings) / float64(n-1)
	}
	return out
}

func dbfs(linear float64) float64 {
	return 20 * math.Log10(linear+1e-12)
}

// Utterance is a server-VAD speech span inside a capture.
type Utterance struct {
	Index       int     `json:"index"`
	StartMS     float64 `json:"start_ms"`
	EndMS       float64 `json:"end_ms"`
	SpeechMS    float64 `json:"speech_ms"`
	PreRollMS   float64 `json:"pre_roll_ms"`
	RMSDBFS     float64 `json:"rms_dbfs"`
	PeakDBFS    float64 `json:"peak_dbfs"`
	NoiseDBFS   float64 `json:"noise_dbfs"`
	SNRDB       float64 `json:"snr_db"`
	StartSample int64   `json:"start_sample"`
	EndSample   int64   `json:"end_sample"`
}

// VADConfig controls the server-side energy VAD used for continuous captures.
type VADConfig struct {
	FrameMS     int
	OnsetMS     int
	SilenceMS   int
	MinSpeechMS int
	PreRollMS   int
	ThresholdDB float64
}

func DefaultVADConfig(sampleRate int) VADConfig {
	return VADConfig{FrameMS: 20, OnsetMS: 120, SilenceMS: 700, MinSpeechMS: 150,
		PreRollMS: 300, ThresholdDB: 6}
}

// DetectUtterances runs an energy VAD over decoded PCM and returns speech
// spans. This is the server-side pre-roll/segmentation path for Opus-only
// devices (docs/protocol.md §7.4).
func DetectUtterances(pcm []int16, sampleRate, channels int, cfg VADConfig) []Utterance {
	if len(pcm) == 0 || sampleRate <= 0 || channels <= 0 {
		return nil
	}
	if cfg.FrameMS <= 0 {
		cfg.FrameMS = 20
	}
	frameSamples := sampleRate * cfg.FrameMS / 1000 * channels
	if frameSamples <= 0 {
		return nil
	}
	nFrames := len(pcm) / frameSamples
	if nFrames == 0 {
		return nil
	}
	rms := make([]float64, nFrames)
	for i := 0; i < nFrames; i++ {
		st := AnalyzeFrame(pcm[i*frameSamples:(i+1)*frameSamples], channels)
		rms[i] = st.RMSDBFS
	}
	noise := Percentile(rms, 10)
	noise = math.Max(-120, math.Min(0, noise))
	onsetFrames := maxInt(1, cfg.OnsetMS/cfg.FrameMS)
	silenceFrames := maxInt(1, cfg.SilenceMS/cfg.FrameMS)
	minSpeechFrames := maxInt(1, cfg.MinSpeechMS/cfg.FrameMS)
	preRollFrames := maxInt(0, cfg.PreRollMS/cfg.FrameMS)

	var out []Utterance
	speechRun := 0
	silenceRun := 0
	inSpeech := false
	startFrame := 0
	lastSpeech := 0
	for i, r := range rms {
		speech := r > noise+cfg.ThresholdDB
		if speech {
			speechRun++
			silenceRun = 0
			if !inSpeech && speechRun >= onsetFrames {
				inSpeech = true
				startFrame = i - speechRun + 1
				if startFrame < 0 {
					startFrame = 0
				}
				startFrame -= preRollFrames
				if startFrame < 0 {
					startFrame = 0
				}
			}
			if inSpeech {
				lastSpeech = i
			}
		} else {
			speechRun = 0
			if inSpeech {
				silenceRun++
				if silenceRun >= silenceFrames {
					if lastSpeech-startFrame+1 >= minSpeechFrames {
						out = append(out, utteranceFromFrames(pcm, rms, sampleRate, channels,
							cfg, startFrame, lastSpeech+1, len(out)))
					}
					inSpeech = false
					silenceRun = 0
				}
			}
		}
	}
	if inSpeech && lastSpeech-startFrame+1 >= minSpeechFrames {
		out = append(out, utteranceFromFrames(pcm, rms, sampleRate, channels, cfg, startFrame,
			lastSpeech+1, len(out)))
	}
	return out
}

func utteranceFromFrames(pcm []int16, rms []float64, sampleRate, channels int, cfg VADConfig,
	startFrame, endFrame, index int) Utterance {
	frameSamples := sampleRate * cfg.FrameMS / 1000 * channels
	if startFrame < 0 {
		startFrame = 0
	}
	if endFrame > len(rms) {
		endFrame = len(rms)
	}
	startSample := int64(startFrame * frameSamples)
	endSample := int64(endFrame * frameSamples)
	if endSample > int64(len(pcm)) {
		endSample = int64(len(pcm))
	}
	seg := pcm[startSample:endSample]
	levels := AnalyzeFrame(seg, channels)
	noise := Percentile(rms, 10)
	u := Utterance{
		Index:       index,
		StartMS:     float64(startSample) / float64(channels) / float64(sampleRate) * 1000,
		EndMS:       float64(endSample) / float64(channels) / float64(sampleRate) * 1000,
		RMSDBFS:     levels.RMSDBFS,
		PeakDBFS:    levels.PeakDBFS,
		NoiseDBFS:   noise,
		SNRDB:       levels.RMSDBFS - noise,
		StartSample: startSample,
		EndSample:   endSample,
	}
	u.SpeechMS = u.EndMS - u.StartMS
	return u
}

// SegmentMetrics is written to segments/<id>/metrics.json.
type SegmentMetrics struct {
	SegmentID          uint32               `json:"segment_id"`
	CaptureID          string               `json:"capture_id"`
	Format             string               `json:"format"`
	SampleRate         int                  `json:"sample_rate"`
	Channels           int                  `json:"channels"`
	GainDB             *float64             `json:"gain_db"`
	Path               string               `json:"path"`
	DurationMS         float64              `json:"duration_ms"`
	SpeechMS           float64              `json:"speech_ms"`
	PreRollMS          float64              `json:"pre_roll_ms"`
	FramesExpected     int                  `json:"frames_expected"`
	FramesReceived     int                  `json:"frames_received"`
	FramesLost         int                  `json:"frames_lost"`
	LossPct            float64              `json:"loss_pct"`
	JitterMS           float64              `json:"jitter_ms"`
	RMSDBFS            float64              `json:"rms_dbfs"`
	PeakDBFS           float64              `json:"peak_dbfs"`
	CrestDB            float64              `json:"crest_db"`
	DCOffsetLSB        float64              `json:"dc_offset_lsb"`
	DCOffsetPctFS      float64              `json:"dc_offset_pct_fs"`
	ClipPct            float64              `json:"clip_pct"`
	ZCR                float64              `json:"zcr"`
	SpeechRMSDBFS      float64              `json:"speech_rms_dbfs"`
	NoiseFloorDBFS     float64              `json:"noise_floor_dbfs"`
	SNRDB              float64              `json:"snr_db"`
	NoiseMethod        string               `json:"noise_method"`
	SpectralCentroidHz float64              `json:"spectral_centroid_hz"`
	SpectralFlatness   float64              `json:"spectral_flatness"`
	Rolloff85Hz        float64              `json:"rolloff_85_hz"`
	Hum50DB            float64              `json:"hum_50_db"`
	Hum60DB            float64              `json:"hum_60_db"`
	HighFreqRatio      float64              `json:"high_freq_ratio"`
	Dropouts           int                  `json:"dropouts"`
	DeviceDrops        int                  `json:"device_drops"`
	Tone               *analysis.ToneResult `json:"tone,omitempty"`
	Utterances         []Utterance          `json:"utterances,omitempty"`
	Notes              []string             `json:"notes"`
}

// ComputeSegmentMetrics aggregates received frames and their decoded PCM.
// rows are in arrival order; pcm is the decoded/raw audio for those frames.
func ComputeSegmentMetrics(captureID string, format string, sampleRate, channels, frameMS int,
	rows []FrameRow, pcm []int16, preRollFrames int, deviceDrops int) SegmentMetrics {
	m := SegmentMetrics{
		CaptureID:   captureID,
		Format:      format,
		SampleRate:  sampleRate,
		Channels:    channels,
		DeviceDrops: deviceDrops,
		Notes:       []string{},
	}
	if len(rows) == 0 {
		m.RMSDBFS, m.PeakDBFS, m.SpeechRMSDBFS, m.NoiseFloorDBFS = -200, -200, -200, -200
		m.NoiseMethod = "none"
		return m
	}
	m.SegmentID = rows[0].Seq // overwritten by caller with the real segment id
	m.FramesReceived = len(rows)

	firstSeq := rows[0].Seq
	lastSeq := rows[len(rows)-1].Seq
	lost := 0
	deviceGap := 0
	for i := 1; i < len(rows); i++ {
		prev := rows[i-1].Seq
		cur := rows[i].Seq
		if cur <= prev {
			continue
		}
		gap := int(cur-prev) - 1
		if gap <= 0 {
			continue
		}
		if rows[i].Flags&(protocol.FlagDiscontinuity|protocol.FlagDTXPLC) != 0 {
			deviceGap += gap
		} else {
			lost += gap
		}
	}
	m.FramesExpected = int(lastSeq-firstSeq+1) + deviceGap
	m.FramesLost = lost
	if m.FramesExpected > 0 {
		m.LossPct = 100 * float64(lost) / float64(m.FramesExpected)
	}

	// Aggregate levels from the decoded PCM.
	total := AnalyzeFrame(pcm, channels)
	m.RMSDBFS = total.RMSDBFS
	m.PeakDBFS = total.PeakDBFS
	m.CrestDB = m.PeakDBFS - m.RMSDBFS
	m.DCOffsetLSB = total.DCLSB
	m.DCOffsetPctFS = total.DCLSB / 32768 * 100
	m.ClipPct = total.ClipPct
	m.ZCR = total.ZCR
	if len(pcm) > 0 {
		m.DurationMS = float64(len(pcm)/channels) / float64(sampleRate) * 1000
	}

	// Map frame rows to PCM chunks and group speech/silence levels.
	frameSamples := 0
	for _, r := range rows {
		if r.Samples > 0 {
			frameSamples = r.Samples
			break
		}
	}
	offset := 0
	var speechRMS, silenceRMS []float64
	speechFrames := 0
	for _, r := range rows {
		n := r.Samples * channels
		if n <= 0 {
			n = frameSamples * channels
		}
		if offset+n > len(pcm) {
			break
		}
		st := AnalyzeFrame(pcm[offset:offset+n], channels)
		offset += n
		if r.Flags&protocol.FlagSpeech != 0 {
			speechRMS = append(speechRMS, st.RMSDBFS)
			speechFrames++
		} else {
			silenceRMS = append(silenceRMS, st.RMSDBFS)
		}
	}
	if len(speechRMS) == 0 {
		// No device speech flags (chip Opus / server VAD): use server VAD.
		utts := DetectUtterances(pcm, sampleRate, channels, DefaultVADConfig(sampleRate))
		m.Utterances = utts
		for _, u := range utts {
			speechRMS = append(speechRMS, u.RMSDBFS)
		}
		m.NoiseMethod = "server_vad"
	} else {
		m.NoiseMethod = "device_vad_silence"
	}
	if frameMS <= 0 {
		frameMS = 20
	}
	if len(speechRMS) > 0 {
		m.SpeechRMSDBFS = Percentile(speechRMS, 50)
		m.SpeechMS = float64(speechFrames) * float64(frameMS)
		if m.SpeechMS == 0 {
			m.SpeechMS = float64(len(speechRMS)) * float64(frameMS)
		}
	} else {
		m.SpeechRMSDBFS = m.RMSDBFS
	}
	if len(silenceRMS) == 0 {
		silenceRMS = frameRMS(rows, pcm, channels)
	}
	if len(silenceRMS) > 0 {
		m.NoiseFloorDBFS = Percentile(silenceRMS, 10)
	} else {
		m.NoiseFloorDBFS = m.RMSDBFS - 20
		m.Notes = append(m.Notes, "noise floor estimated from segment RMS")
	}
	m.SNRDB = m.SpeechRMSDBFS - m.NoiseFloorDBFS
	m.PreRollMS = float64(preRollFrames * frameMS)
	m.JitterMS = jitterMS(rows)

	sp := SpectralMetrics(pcm, sampleRate)
	m.SpectralCentroidHz = sp.CentroidHz
	m.SpectralFlatness = sp.Flatness
	m.Rolloff85Hz = sp.Rolloff85Hz
	m.Hum50DB = sp.Hum50DB
	m.Hum60DB = sp.Hum60DB
	m.HighFreqRatio = sp.HighFreqRatio
	m.Dropouts = countDropouts(rows, pcm, channels,
		func(frames []float64) float64 { return Percentile(frames, 10) })
	if m.SNRDB < 20 {
		m.Notes = append(m.Notes, "low SNR (<20 dB)")
	}
	if m.ClipPct > 0.1 {
		m.Notes = append(m.Notes, "clipping >0.1%")
	}
	if m.LossPct > 0.5 {
		m.Notes = append(m.Notes, "packet loss >0.5%")
	}
	return m
}

func frameRMS(rows []FrameRow, pcm []int16, channels int) []float64 {
	var out []float64
	offset := 0
	for _, r := range rows {
		n := r.Samples * channels
		if n <= 0 || offset+n > len(pcm) {
			break
		}
		out = append(out, AnalyzeFrame(pcm[offset:offset+n], channels).RMSDBFS)
		offset += n
	}
	return out
}

func jitterMS(rows []FrameRow) float64 {
	if len(rows) < 2 {
		return 0
	}
	baseArrival := rows[0].ArrivalMS
	baseTS := int64(rows[0].DeviceTSMS)
	var sum, sumSq float64
	n := 0
	for _, r := range rows {
		if r.Flags&(protocol.FlagPreRoll|protocol.FlagDTXPLC) != 0 {
			continue
		}
		d := float64((r.ArrivalMS - baseArrival) - (int64(r.DeviceTSMS) - baseTS))
		sum += d
		sumSq += d * d
		n++
	}
	if n < 2 {
		return 0
	}
	mean := sum / float64(n)
	variance := sumSq/float64(n) - mean*mean
	if variance < 0 {
		variance = 0
	}
	return math.Sqrt(variance)
}

func countDropouts(rows []FrameRow, pcm []int16, channels int, noiseFn func([]float64) float64) int {
	levels := frameRMS(rows, pcm, channels)
	if len(levels) == 0 {
		return 0
	}
	threshold := math.Max(-80, noiseFn(levels)-6)
	run := 0
	dropouts := 0
	for i, r := range rows {
		if i >= len(levels) {
			break
		}
		if r.Flags&protocol.FlagSpeech != 0 && levels[i] < threshold {
			run++
			if run == 2 {
				dropouts++
			}
		} else {
			run = 0
		}
	}
	return dropouts
}

// Percentile returns the p-th percentile (0..100) of values. It sorts a copy.
func Percentile(values []float64, p float64) float64 {
	if len(values) == 0 {
		return -200
	}
	cp := append([]float64(nil), values...)
	sort.Float64s(cp)
	if p <= 0 {
		return cp[0]
	}
	if p >= 100 {
		return cp[len(cp)-1]
	}
	idx := int(math.Round(p / 100 * float64(len(cp)-1)))
	if idx < 0 {
		idx = 0
	}
	if idx >= len(cp) {
		idx = len(cp) - 1
	}
	return cp[idx]
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
