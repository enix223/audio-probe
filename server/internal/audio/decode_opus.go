//go:build opus

package audio

import (
	"gopkg.in/hraban/opus.v2"
)

type libopusDecoder struct {
	decoder *opus.Decoder
	rate    int
	ch      int
}

func (d *libopusDecoder) Decode(packet []byte, sampleRate, channels int) ([]int16, error) {
	if d.decoder == nil || d.rate != sampleRate || d.ch != channels {
		dec, err := opus.NewDecoder(sampleRate, channels)
		if err != nil {
			return nil, err
		}
		d.decoder = dec
		d.rate = sampleRate
		d.ch = channels
	}
	// 120 ms is the maximum Opus packet duration.
	pcm := make([]int16, sampleRate*channels*120/1000)
	n, err := d.decoder.Decode(packet, pcm)
	if err != nil {
		return nil, err
	}
	return pcm[:n], nil
}

func init() {
	NewOpusDecoder = func() (OpusDecoder, error) {
		return &libopusDecoder{}, nil
	}
}
