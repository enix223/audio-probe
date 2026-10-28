// Package protocol implements the normative recorder v1 wire contract.
//
// The authoritative description is docs/protocol.md.  protocol/golden/ holds
// byte-exact vectors shared with the C++ device; this package must agree with
// them and with protocol/cl_recorder_protocol.h.
package protocol

import (
	"encoding/binary"
	"fmt"
)

const (
	Version          = 1
	MessageTypeAudio = 1
	AudioHeaderSize  = 28

	MaxOpusPayload  = 255
	MaxPCMPayload   = 4096
	MaxAudioMessage = AudioHeaderSize + MaxPCMPayload

	StatUnavailableI16 = -32768
	StatUnavailableU16 = 0xFFFF
	VadScoreUnknown    = 255
)

// AudioFormat is the binary header format byte.
type AudioFormat uint8

const (
	FormatPCMS16LE AudioFormat = 1
	FormatOpus     AudioFormat = 2
)

func (f AudioFormat) String() string {
	switch f {
	case FormatPCMS16LE:
		return "pcm_s16le"
	case FormatOpus:
		return "opus"
	default:
		return "unknown"
	}
}

// Audio flags in the binary header.
const (
	FlagPreRoll       uint8 = 0x01
	FlagSpeech        uint8 = 0x02
	FlagClipped       uint8 = 0x04
	FlagDiscontinuity uint8 = 0x08
	FlagDTXPLC        uint8 = 0x10
	FlagStatsValid    uint8 = 0x20
	FlagReservedMask  uint8 = 0xC0
)

// AudioHeader is the decoded 28-byte binary header.
type AudioHeader struct {
	Version     uint8
	Type        uint8
	Flags       uint8
	Format      AudioFormat
	SegmentID   uint32
	Seq         uint32
	TimestampMS uint32
	Samples     uint16
	PayloadLen  uint16
	RMSDBFSx100 int16
	Peak        uint16
	DCOffset    int16
	VADScore    uint8
	Reserved    uint8
}

func (h AudioHeader) HasFlag(flag uint8) bool { return h.Flags&flag != 0 }

// HeaderError identifies a rejected frame.
type HeaderError struct {
	Code string
	Msg  string
}

func (e *HeaderError) Error() string {
	if e.Msg == "" {
		return e.Code
	}
	return e.Code + ": " + e.Msg
}

func hdrErr(code, msg string) error { return &HeaderError{Code: code, Msg: msg} }

// DecodeAudioHeader decodes a complete binary WebSocket message.
//
// maxFrameBytes is the effective negotiated limit (the smaller of the device
// and server limits).  On success, PayloadOffset returns 28.
func DecodeAudioHeader(data []byte, maxFrameBytes int) (AudioHeader, error) {
	var h AudioHeader
	if len(data) < AudioHeaderSize {
		return h, hdrErr("truncated_header", fmt.Sprintf("%d bytes", len(data)))
	}

	h.Version = data[0]
	h.Type = data[1]
	h.Flags = data[2]
	h.Format = AudioFormat(data[3])
	h.SegmentID = binary.BigEndian.Uint32(data[4:8])
	h.Seq = binary.BigEndian.Uint32(data[8:12])
	h.TimestampMS = binary.BigEndian.Uint32(data[12:16])
	h.Samples = binary.BigEndian.Uint16(data[16:18])
	h.PayloadLen = binary.BigEndian.Uint16(data[18:20])
	h.RMSDBFSx100 = int16(binary.BigEndian.Uint16(data[20:22]))
	h.Peak = binary.BigEndian.Uint16(data[22:24])
	h.DCOffset = int16(binary.BigEndian.Uint16(data[24:26]))
	h.VADScore = data[26]
	h.Reserved = data[27]

	if h.Version != Version {
		return h, hdrErr("unsupported_version", fmt.Sprintf("version %d", h.Version))
	}
	if h.Type != MessageTypeAudio {
		return h, hdrErr("bad_type", fmt.Sprintf("type %d", h.Type))
	}
	if h.Format != FormatPCMS16LE && h.Format != FormatOpus {
		return h, hdrErr("bad_format", fmt.Sprintf("format %d", h.Format))
	}
	if h.Flags&FlagReservedMask != 0 || h.Reserved != 0 {
		return h, hdrErr("reserved_bits", "reserved flag/byte non-zero")
	}
	if int(h.PayloadLen) != len(data)-AudioHeaderSize {
		return h, hdrErr("length_mismatch",
			fmt.Sprintf("payload_len %d, message carries %d", h.PayloadLen, len(data)-AudioHeaderSize))
	}
	if len(data) > maxFrameBytes {
		return h, hdrErr("payload_too_large",
			fmt.Sprintf("message %d > max %d", len(data), maxFrameBytes))
	}

	if h.Format == FormatPCMS16LE {
		if h.PayloadLen > MaxPCMPayload {
			return h, hdrErr("payload_too_large", fmt.Sprintf("pcm payload %d", h.PayloadLen))
		}
	} else {
		if h.PayloadLen > MaxOpusPayload {
			return h, hdrErr("opus_payload_too_large", fmt.Sprintf("opus payload %d", h.PayloadLen))
		}
		if h.PayloadLen == 0 {
			return h, hdrErr("opus_empty_packet", "empty packet")
		}
		if _, ok := ParseOpusTOC(data[AudioHeaderSize:]); !ok {
			return h, hdrErr("opus_empty_packet", "unparseable TOC")
		}
	}
	return h, nil
}

// ValidatePCM checks the payload/sample/channel relationship.  The binary
// header does not carry the channel count, so this uses the negotiated one.
func ValidatePCM(h AudioHeader, channels int) error {
	if h.Format != FormatPCMS16LE || channels <= 0 {
		return hdrErr("bad_format", "not pcm")
	}
	want := int(h.Samples) * channels * 2
	if int(h.PayloadLen) != want {
		return hdrErr("pcm_length_mismatch",
			fmt.Sprintf("payload_len %d, samples*channels*2 = %d", h.PayloadLen, want))
	}
	return nil
}

// EncodeAudioHeader produces the 28-byte wire header (used by tests and tools).
func EncodeAudioHeader(h AudioHeader) [AudioHeaderSize]byte {
	var out [AudioHeaderSize]byte
	out[0] = h.Version
	out[1] = h.Type
	out[2] = h.Flags
	out[3] = byte(h.Format)
	binary.BigEndian.PutUint32(out[4:8], h.SegmentID)
	binary.BigEndian.PutUint32(out[8:12], h.Seq)
	binary.BigEndian.PutUint32(out[12:16], h.TimestampMS)
	binary.BigEndian.PutUint16(out[16:18], h.Samples)
	binary.BigEndian.PutUint16(out[18:20], h.PayloadLen)
	binary.BigEndian.PutUint16(out[20:22], uint16(h.RMSDBFSx100))
	binary.BigEndian.PutUint16(out[22:24], h.Peak)
	binary.BigEndian.PutUint16(out[24:26], uint16(h.DCOffset))
	out[26] = h.VADScore
	out[27] = h.Reserved
	return out
}

// OpusTOCInfo is the structural description of an Opus packet from its TOC.
type OpusTOCInfo struct {
	OK         bool
	FrameCount int
	TotalUS    int
}

// ParseOpusTOC mirrors opus_packet_get_samples_per_frame /
// opus_packet_get_nb_frames.  It validates the structural TOC only.
func ParseOpusTOC(data []byte) (OpusTOCInfo, bool) {
	var info OpusTOCInfo
	if len(data) == 0 {
		return info, false
	}
	toc := data[0]
	var spf48 int
	switch {
	case toc&0x80 != 0:
		spf48 = 120 << ((toc >> 3) & 0x3)
	case toc&0x60 == 0x60:
		if toc&0x08 != 0 {
			spf48 = 960
		} else {
			spf48 = 480
		}
	default:
		k := (toc >> 3) & 0x3
		if k == 3 {
			spf48 = 2880
		} else {
			spf48 = 480 << k
		}
	}

	var count int
	switch toc & 0x3 {
	case 0:
		count = 1
	case 1, 2:
		count = 2
	default:
		if len(data) < 2 {
			return info, false
		}
		count = int(data[1] & 0x3F)
		if count <= 0 {
			return info, false
		}
	}
	samples48 := spf48 * count
	if samples48 > 5760 {
		return info, false
	}
	info.OK = true
	info.FrameCount = count
	info.TotalUS = samples48 * 1000 / 48
	return info, true
}
