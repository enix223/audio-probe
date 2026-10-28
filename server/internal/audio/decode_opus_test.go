//go:build opus

package audio

import (
	"math"
	"testing"

	"gopkg.in/hraban/opus.v2"
)

func TestOpusRoundTrip(t *testing.T) {
	enc, err := opus.NewEncoder(16000, 1, opus.AppVoIP)
	if err != nil {
		t.Fatalf("encoder: %v", err)
	}
	pcm := make([]int16, 320)
	for i := range pcm {
		pcm[i] = int16(16000 * math.Sin(2*math.Pi*440*float64(i)/16000))
	}
	packet := make([]byte, 4000)
	n, err := enc.Encode(pcm, packet)
	if err != nil || n <= 0 {
		t.Fatalf("encode: n=%d err=%v", n, err)
	}
	dec, err := NewOpusDecoder()
	if err != nil {
		t.Fatalf("decoder: %v", err)
	}
	out, err := dec.Decode(packet[:n], 16000, 1)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(out) == 0 {
		t.Fatal("decoded no samples")
	}
	var sum float64
	for _, v := range out {
		sum += float64(v) * float64(v)
	}
	rms := math.Sqrt(sum/float64(len(out))) / 32768
	if rms < 0.1 {
		t.Fatalf("decoded RMS too low: %.4f", rms)
	}
}
