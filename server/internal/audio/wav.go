// Package audio provides PCM/WAV helpers and optional Opus decoding.
package audio

import (
	"encoding/binary"
	"fmt"
	"io"
)

// WAVWriter writes a PCM S16LE RIFF/WAVE file. Close() rewrites the header
// sizes; the caller must call it even for an empty segment.
type WAVWriter struct {
	w          io.WriteSeeker
	sampleRate int
	channels   int
	dataBytes  uint32
	closed     bool
}

func NewWAVWriter(w io.WriteSeeker, sampleRate, channels int) (*WAVWriter, error) {
	if sampleRate <= 0 || channels <= 0 {
		return nil, fmt.Errorf("invalid wav format: rate=%d channels=%d", sampleRate, channels)
	}
	ww := &WAVWriter{w: w, sampleRate: sampleRate, channels: channels}
	if err := ww.writeHeader(0); err != nil {
		return nil, err
	}
	return ww, nil
}

func (w *WAVWriter) writeHeader(dataBytes uint32) error {
	byteRate := uint32(w.sampleRate * w.channels * 2)
	blockAlign := uint16(w.channels * 2)
	header := make([]byte, 44)
	copy(header[0:4], "RIFF")
	binary.LittleEndian.PutUint32(header[4:8], 36+dataBytes)
	copy(header[8:12], "WAVE")
	copy(header[12:16], "fmt ")
	binary.LittleEndian.PutUint32(header[16:20], 16)
	binary.LittleEndian.PutUint16(header[20:22], 1) // PCM
	binary.LittleEndian.PutUint16(header[22:24], uint16(w.channels))
	binary.LittleEndian.PutUint32(header[24:28], uint32(w.sampleRate))
	binary.LittleEndian.PutUint32(header[28:32], byteRate)
	binary.LittleEndian.PutUint16(header[32:34], blockAlign)
	binary.LittleEndian.PutUint16(header[34:36], 16)
	copy(header[36:40], "data")
	binary.LittleEndian.PutUint32(header[40:44], dataBytes)
	if _, err := w.w.Seek(0, io.SeekStart); err != nil {
		return err
	}
	_, err := w.w.Write(header)
	return err
}

// WriteSamples appends interleaved S16 samples.
func (w *WAVWriter) WriteSamples(samples []int16) error {
	if w.closed {
		return fmt.Errorf("wav writer closed")
	}
	if len(samples) == 0 {
		return nil
	}
	buf := make([]byte, len(samples)*2)
	for i, s := range samples {
		binary.LittleEndian.PutUint16(buf[i*2:], uint16(s))
	}
	if _, err := w.w.Write(buf); err != nil {
		return err
	}
	w.dataBytes += uint32(len(buf))
	return nil
}

func (w *WAVWriter) Close() error {
	if w.closed {
		return nil
	}
	w.closed = true
	if err := w.writeHeader(w.dataBytes); err != nil {
		return err
	}
	if seeker, ok := w.w.(io.Seeker); ok {
		_, err := seeker.Seek(0, io.SeekEnd)
		return err
	}
	return nil
}

func (w *WAVWriter) DataBytes() uint32 { return w.dataBytes }

// BytesToSamples converts little-endian S16LE bytes to int16 samples.
func BytesToSamples(b []byte) []int16 {
	out := make([]int16, len(b)/2)
	for i := range out {
		out[i] = int16(binary.LittleEndian.Uint16(b[i*2:]))
	}
	return out
}

// SamplesToBytes converts interleaved int16 samples to S16LE bytes.
func SamplesToBytes(s []int16) []byte {
	out := make([]byte, len(s)*2)
	for i, v := range s {
		binary.LittleEndian.PutUint16(out[i*2:], uint16(v))
	}
	return out
}

// OpusDecoder decodes one Opus packet to interleaved S16 samples.
type OpusDecoder interface {
	Decode(packet []byte, sampleRate, channels int) ([]int16, error)
}

// ErrNoDecoder is returned by the noopus build.
var ErrNoDecoder = fmt.Errorf("opus decoding is disabled in this build (use -tags opus)")

// NewOpusDecoder returns nil in the default build; the opus-tagged build
// replaces this via decode_opus.go's init hook.
var NewOpusDecoder func() (OpusDecoder, error) = func() (OpusDecoder, error) {
	return nil, ErrNoDecoder
}
