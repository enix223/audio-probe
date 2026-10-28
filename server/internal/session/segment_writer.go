package session

import (
	"bufio"
	"encoding/binary"
	"encoding/csv"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"

	"audio-probe/server/internal/analysis"
	"audio-probe/server/internal/audio"
	"audio-probe/server/internal/metrics"
	"audio-probe/server/internal/protocol"
	"audio-probe/server/internal/report"
	"audio-probe/server/internal/storage"
)

type segmentWriter struct {
	session   *Session
	captureID string
	dir       string
	start     protocol.SegmentStart
	audio     protocol.AudioConfig
	tone      *protocol.ToneInfo

	wav        *audio.WAVWriter
	wavFile    *os.File
	packets    *os.File
	frames     *csv.Writer
	framesFile *os.File
	decoder    audio.OpusDecoder
	decoderErr error
	notes      []string

	rows           []metrics.FrameRow
	pcm            []int16
	lastSeq        uint32
	haveSeq        bool
	packetsWritten int
}

type segmentMeta struct {
	Start          protocol.SegmentStart `json:"segment_start"`
	End            *protocol.SegmentEnd  `json:"segment_end,omitempty"`
	Audio          protocol.AudioConfig  `json:"audio"`
	Tone           *protocol.ToneInfo    `json:"tone,omitempty"`
	Notes          []string              `json:"notes"`
	UtteranceFiles []string              `json:"utterance_files,omitempty"`
}

func newSegmentWriter(s *Session, start protocol.SegmentStart) (*segmentWriter, error) {
	dir, err := s.layout.EnsureSegment(s.cfg.CaptureID, start.SegmentID)
	if err != nil {
		return nil, err
	}
	seg := &segmentWriter{
		session:   s,
		captureID: s.cfg.CaptureID,
		dir:       dir,
		start:     start,
		audio:     s.cfg.Audio,
		tone:      start.Tone,
	}
	if seg.tone == nil {
		s.mu.Lock()
		seg.tone = s.tone
		s.mu.Unlock()
	}
	if seg.start.Audio.Format == "" {
		seg.start.Audio = s.cfg.Audio
	}

	seg.framesFile, err = os.Create(filepath.Join(dir, "frames.csv"))
	if err != nil {
		return nil, err
	}
	seg.frames = csv.NewWriter(bufio.NewWriter(seg.framesFile))
	_ = seg.frames.Write([]string{"seq", "device_ts_ms", "arrival_ms", "bytes", "flags",
		"samples", "rms_dbfs", "peak_dbfs", "dc_lsb", "clip_pct", "zcr",
		"device_rms_x100", "device_peak", "device_dc", "vad_score"})

	if s.cfg.Audio.Format == "pcm_s16le" {
		seg.wavFile, err = os.Create(filepath.Join(dir, "audio.wav"))
		if err != nil {
			return nil, err
		}
		seg.wav, err = audio.NewWAVWriter(seg.wavFile, s.cfg.Audio.SampleRate, s.cfg.Audio.Channels)
		if err != nil {
			return nil, err
		}
	} else {
		seg.packets, err = os.Create(filepath.Join(dir, "packets.bin"))
		if err != nil {
			return nil, err
		}
		seg.decoder, seg.decoderErr = audio.NewOpusDecoder()
		if seg.decoderErr == nil && seg.decoder != nil {
			seg.wavFile, err = os.Create(filepath.Join(dir, "audio.wav"))
			if err != nil {
				return nil, err
			}
			seg.wav, err = audio.NewWAVWriter(seg.wavFile, s.cfg.Audio.SampleRate, s.cfg.Audio.Channels)
			if err != nil {
				return nil, err
			}
		} else {
			seg.notes = append(seg.notes, "opus_decode_unavailable: raw packets preserved")
		}
	}
	return seg, nil
}

func (w *segmentWriter) addFrame(f *frameJob) error {
	h := f.header
	row := metrics.FrameRow{
		Seq:        h.Seq,
		DeviceTSMS: h.TimestampMS,
		ArrivalMS:  f.arrival.UnixMilli(),
		Bytes:      len(f.payload),
		Flags:      h.Flags,
		Samples:    int(h.Samples),
		DeviceRMS:  h.RMSDBFSx100,
		DevicePeak: h.Peak,
		DeviceDC:   h.DCOffset,
		DeviceVAD:  h.VADScore,
	}

	if w.audio.Format == "pcm_s16le" {
		pcm := audio.BytesToSamples(f.payload)
		st := metrics.AnalyzeFrame(pcm, w.audio.Channels)
		row.RMSDBFS, row.PeakDBFS, row.DCLSB, row.ClipPct, row.ZCR =
			st.RMSDBFS, st.PeakDBFS, st.DCLSB, st.ClipPct, st.ZCR
		w.pcm = append(w.pcm, pcm...)
		if w.wav != nil {
			if err := w.wav.WriteSamples(pcm); err != nil {
				return err
			}
		}
	} else {
		if w.packets != nil {
			var lenBuf [4]byte
			binary.BigEndian.PutUint32(lenBuf[:], uint32(len(f.payload)))
			if _, err := w.packets.Write(lenBuf[:]); err != nil {
				return err
			}
			if _, err := w.packets.Write(f.payload); err != nil {
				return err
			}
			w.packetsWritten++
		}
		if w.decoder != nil {
			pcm, err := w.decoder.Decode(f.payload, w.audio.SampleRate, w.audio.Channels)
			if err != nil {
				return fmt.Errorf("opus decode: %w", err)
			}
			st := metrics.AnalyzeFrame(pcm, w.audio.Channels)
			row.RMSDBFS, row.PeakDBFS, row.DCLSB, row.ClipPct, row.ZCR =
				st.RMSDBFS, st.PeakDBFS, st.DCLSB, st.ClipPct, st.ZCR
			w.pcm = append(w.pcm, pcm...)
			if w.wav != nil {
				if err := w.wav.WriteSamples(pcm); err != nil {
					return err
				}
			}
		} else if h.Flags&protocol.FlagStatsValid != 0 {
			row.RMSDBFS = float64(h.RMSDBFSx100) / 100
			row.PeakDBFS = 20 * math.Log10(float64(h.Peak)/32768+1e-12)
			row.DCLSB = float64(h.DCOffset)
		}
	}

	w.rows = append(w.rows, row)
	_ = w.frames.Write([]string{
		strconv.FormatUint(uint64(row.Seq), 10),
		strconv.FormatUint(uint64(row.DeviceTSMS), 10),
		strconv.FormatInt(row.ArrivalMS, 10),
		strconv.Itoa(row.Bytes),
		strconv.Itoa(int(row.Flags)),
		strconv.Itoa(row.Samples),
		f2(row.RMSDBFS), f2(row.PeakDBFS), f2(row.DCLSB), f2(row.ClipPct), f4(row.ZCR),
		strconv.Itoa(int(row.DeviceRMS)), strconv.Itoa(int(row.DevicePeak)),
		strconv.Itoa(int(row.DeviceDC)), strconv.Itoa(int(row.DeviceVAD)),
	})
	w.lastSeq = h.Seq
	w.haveSeq = true
	return nil
}

func (w *segmentWriter) finalize(reason string, end *protocol.SegmentEnd) (report.SegmentRow, string) {
	if end == nil {
		end = &protocol.SegmentEnd{
			Type:       "segment_end",
			CaptureID:  w.captureID,
			SegmentID:  w.start.SegmentID,
			Reason:     reason,
			TMS:        w.start.TMS + uint32(len(w.pcm)/max(1, w.audio.Channels)*1000/max(1, w.audio.SampleRate)),
			Frames:     len(w.rows),
			DurationMS: len(w.pcm) / max(1, w.audio.Channels) * 1000 / max(1, w.audio.SampleRate),
		}
	}
	if end.Reason == "" {
		end.Reason = reason
	}
	if w.frames != nil {
		w.frames.Flush()
	}
	if w.framesFile != nil {
		_ = w.framesFile.Close()
	}
	if w.wav != nil {
		_ = w.wav.Close()
	}
	if w.wavFile != nil {
		_ = w.wavFile.Close()
	}
	if w.packets != nil {
		_ = w.packets.Close()
	}

	preRollFrames := end.PreRollFrames
	m := metrics.ComputeSegmentMetrics(w.captureID, w.audio.Format, w.audio.SampleRate,
		w.audio.Channels, w.audio.FrameMS, w.rows, w.pcm, preRollFrames, end.FramesDropped)
	m.SegmentID = w.start.SegmentID
	m.GainDB = nil
	if w.session != nil {
		m.Path = w.session.cfg.Path
	}
	m.Notes = append(m.Notes, w.notes...)
	if w.tone == nil && w.session != nil {
		w.session.mu.Lock()
		w.tone = w.session.tone
		w.session.mu.Unlock()
	}
	if w.decoderErr != nil && w.audio.Format == "opus" {
		m.Notes = append(m.Notes, "opus packets are preserved but not decoded")
	}
	if w.tone != nil && len(w.pcm) > 0 {
		expected := w.tone.FrequencyHz
		if expected == 0 {
			expected = w.tone.StartHz
		}
		tr := analysis.AnalyzeTone(w.pcm, w.audio.SampleRate, expected)
		tr.ToneID = w.tone.ToneID
		tr.Waveform = w.tone.Waveform
		m.Tone = &tr
	}

	meta := segmentMeta{
		Start: w.start,
		End:   end,
		Audio: w.audio,
		Tone:  w.tone,
		Notes: append([]string(nil), w.notes...),
	}

	// Server-VAD utterance WAVs inside the segment (Opus continuous path).
	if len(m.Utterances) > 0 && len(w.pcm) > 0 {
		files, note := writeUtteranceWAVs(w.dir, w.pcm, w.audio, m.Utterances)
		if note != "" {
			meta.Notes = append(meta.Notes, note)
		}
		meta.UtteranceFiles = files
	}

	_ = storage.WriteJSON(filepath.Join(w.dir, "meta.json"), meta)
	_ = storage.WriteJSON(filepath.Join(w.dir, "metrics.json"), m)
	_ = report.WriteSegmentPlots(w.dir, w.pcm, w.audio.SampleRate, w.audio.Channels)

	row := report.SegmentRow{
		SegmentID:  w.start.SegmentID,
		Label:      w.start.Label,
		Reason:     end.Reason,
		Format:     w.audio.Format,
		DurationMS: m.DurationMS,
		SpeechMS:   m.SpeechMS,
		PreRollMS:  m.PreRollMS,
		Frames:     m.FramesReceived,
		Lost:       m.FramesLost,
		LossPct:    m.LossPct,
		RMSDBFS:    m.RMSDBFS,
		PeakDBFS:   m.PeakDBFS,
		ClipPct:    m.ClipPct,
		NoiseDBFS:  m.NoiseFloorDBFS,
		SNRDB:      m.SNRDB,
	}
	if m.Tone != nil {
		row.THDDb = m.Tone.THDDb
		row.THDNDb = m.Tone.THDNDb
	}
	note := ""
	if w.decoderErr != nil && w.audio.Format == "opus" {
		note = "opus capture preserved as packets; decoded WAV unavailable"
	}
	return row, note
}

func writeUtteranceWAVs(dir string, pcm []int16, cfg protocol.AudioConfig, utts []metrics.Utterance) ([]string, string) {
	udir := filepath.Join(dir, "utterances")
	if err := os.MkdirAll(udir, 0o755); err != nil {
		return nil, "failed to create utterances dir: " + err.Error()
	}
	var files []string
	for i, u := range utts {
		start := u.StartSample
		end := u.EndSample
		if start < 0 {
			start = 0
		}
		if end > int64(len(pcm)) {
			end = int64(len(pcm))
		}
		if start >= end {
			continue
		}
		name := fmt.Sprintf("utt-%03d.wav", i)
		path := filepath.Join(udir, name)
		f, err := os.Create(path)
		if err != nil {
			return files, "failed to create utterance wav: " + err.Error()
		}
		w, err := audio.NewWAVWriter(f, cfg.SampleRate, cfg.Channels)
		if err != nil {
			_ = f.Close()
			return files, "failed to write utterance wav: " + err.Error()
		}
		_ = w.WriteSamples(pcm[start:end])
		_ = w.Close()
		_ = f.Close()
		files = append(files, filepath.Join("utterances", name))
	}
	return files, ""
}

func f2(v float64) string { return strconv.FormatFloat(v, 'f', 2, 64) }
func f4(v float64) string { return strconv.FormatFloat(v, 'f', 4, 64) }

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}
