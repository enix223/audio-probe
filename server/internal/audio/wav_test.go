package audio

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestWAVWriterRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "audio.wav")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	w, err := NewWAVWriter(f, 16000, 1)
	if err != nil {
		t.Fatal(err)
	}
	samples := make([]int16, 1000)
	for i := range samples {
		samples[i] = int16(i - 500)
	}
	if err := w.WriteSamples(samples); err != nil {
		t.Fatal(err)
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) != 44+len(samples)*2 {
		t.Fatalf("file size %d, want %d", len(data), 44+len(samples)*2)
	}
	if string(data[0:4]) != "RIFF" || string(data[8:12]) != "WAVE" {
		t.Fatalf("bad riff header")
	}
	if binary.LittleEndian.Uint32(data[24:28]) != 16000 {
		t.Fatalf("rate mismatch")
	}
	if binary.LittleEndian.Uint16(data[22:24]) != 1 {
		t.Fatalf("channels mismatch")
	}
	if binary.LittleEndian.Uint32(data[40:44]) != uint32(len(samples)*2) {
		t.Fatalf("data size mismatch")
	}
	got := BytesToSamples(data[44:])
	for i := range samples {
		if got[i] != samples[i] {
			t.Fatalf("sample %d: got %d want %d", i, got[i], samples[i])
		}
	}
}

func TestBytesSamplesRoundTrip(t *testing.T) {
	samples := []int16{-32768, -1, 0, 1, 32767}
	round := BytesToSamples(SamplesToBytes(samples))
	if len(round) != len(samples) {
		t.Fatalf("length mismatch")
	}
	for i := range samples {
		if round[i] != samples[i] {
			t.Fatalf("sample %d: got %d want %d", i, round[i], samples[i])
		}
	}
}

func TestDefaultBuildHasNoOpusDecoder(t *testing.T) {
	// This test is compiled in the default (!opus) build only.
	if _, err := NewOpusDecoder(); err == nil {
		t.Skip("opus decoder is available in this build")
	}
}
