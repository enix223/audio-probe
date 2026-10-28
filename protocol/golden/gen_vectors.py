#!/usr/bin/env python3
"""Generate the shared protocol golden vectors.

Run from the repository root:

    python3 protocol/golden/gen_vectors.py

The generated files are committed.  C++ (tests/test_protocol.cc) and Go
(server/internal/protocol/golden_test.go) decode the same bytes and compare
against audio_header_expected.json / malformed_expected.json.
"""

import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

HEADER = struct.Struct(">BBBBIIIHHhHhBB")


def header(
    version=1,
    mtype=1,
    flags=0,
    fmt=1,
    segment_id=0,
    seq=0,
    timestamp_ms=0,
    samples=0,
    payload_len=0,
    rms=-32768,
    peak=0xFFFF,
    dc=-32768,
    vad=255,
    reserved=0,
):
    return HEADER.pack(
        version,
        mtype,
        flags,
        fmt,
        segment_id,
        seq,
        timestamp_ms,
        samples,
        payload_len,
        rms,
        peak,
        dc,
        vad,
        reserved,
    )


def pattern(n, seed=0):
    return bytes(((i * 7 + seed) & 0xFF) for i in range(n))


def write(name, data):
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(data)


def main():
    expected = {}

    pcm_payload = pattern(640)
    pcm = header(
        flags=0x21,
        fmt=1,
        segment_id=1,
        seq=0,
        timestamp_ms=1000,
        samples=320,
        payload_len=640,
        rms=-6120,
        peak=1200,
        dc=3,
        vad=5,
    ) + pcm_payload
    write("audio_header_pcm_s16le.bin", pcm)
    expected["audio_header_pcm_s16le.bin"] = {
        "version": 1,
        "type": 1,
        "flags": 0x21,
        "format": 1,
        "segment_id": 1,
        "seq": 0,
        "timestamp_ms": 1000,
        "samples": 320,
        "payload_len": 640,
        "rms_dbfs_x100": -6120,
        "peak": 1200,
        "dc_offset": 3,
        "vad_score": 5,
        "reserved": 0,
        "payload_len_actual": 640,
    }

    opus_payload = bytes([0x48]) + pattern(73, seed=3)
    opus = header(
        flags=0x02,
        fmt=2,
        segment_id=2,
        seq=5,
        timestamp_ms=1000,
        samples=320,
        payload_len=74,
        rms=-32768,
        peak=0xFFFF,
        dc=-32768,
        vad=255,
    ) + opus_payload
    write("audio_header_opus.bin", opus)
    expected["audio_header_opus.bin"] = {
        "version": 1,
        "type": 1,
        "flags": 0x02,
        "format": 2,
        "segment_id": 2,
        "seq": 5,
        "timestamp_ms": 1000,
        "samples": 320,
        "payload_len": 74,
        "rms_dbfs_x100": -32768,
        "peak": 0xFFFF,
        "dc_offset": -32768,
        "vad_score": 255,
        "reserved": 0,
        "payload_len_actual": 74,
    }

    all_flags_payload = pattern(320)
    all_flags = header(
        flags=0x3F,
        fmt=1,
        segment_id=0xFFFFFFFF,
        seq=0xFFFFFFFE,
        timestamp_ms=0xFFFFFFFF,
        samples=160,
        payload_len=320,
        rms=-100,
        peak=32768,
        dc=-1234,
        vad=100,
    ) + all_flags_payload
    write("audio_header_flags_all.bin", all_flags)
    expected["audio_header_flags_all.bin"] = {
        "version": 1,
        "type": 1,
        "flags": 0x3F,
        "format": 1,
        "segment_id": 0xFFFFFFFF,
        "seq": 0xFFFFFFFE,
        "timestamp_ms": 0xFFFFFFFF,
        "samples": 160,
        "payload_len": 320,
        "rms_dbfs_x100": -100,
        "peak": 32768,
        "dc_offset": -1234,
        "vad_score": 100,
        "reserved": 0,
        "payload_len_actual": 320,
    }

    sentinels_payload = pattern(320)
    sentinels = header(
        flags=0x00,
        fmt=1,
        segment_id=7,
        seq=9,
        timestamp_ms=42,
        samples=160,
        payload_len=320,
        rms=-32768,
        peak=0xFFFF,
        dc=-32768,
        vad=255,
    ) + sentinels_payload
    write("audio_header_sentinels.bin", sentinels)
    expected["audio_header_sentinels.bin"] = {
        "version": 1,
        "type": 1,
        "flags": 0x00,
        "format": 1,
        "segment_id": 7,
        "seq": 9,
        "timestamp_ms": 42,
        "samples": 160,
        "payload_len": 320,
        "rms_dbfs_x100": -32768,
        "peak": 0xFFFF,
        "dc_offset": -32768,
        "vad_score": 255,
        "reserved": 0,
        "payload_len_actual": 320,
    }

    with open(os.path.join(HERE, "audio_header_expected.json"), "w") as f:
        json.dump(expected, f, indent=2, sort_keys=True)
        f.write("\n")

    # --- malformed vectors -------------------------------------------------
    malformed = {}

    def mal(name, data, error):
        write(name, data)
        malformed[name] = error

    mal("malformed_truncated_header.bin", header()[:20], "truncated_header")
    mal(
        "malformed_bad_version.bin",
        header(version=2, samples=1, payload_len=2, fmt=1) + b"\x00\x00",
        "unsupported_version",
    )
    mal(
        "malformed_bad_type.bin",
        header(mtype=2, samples=1, payload_len=2, fmt=1) + b"\x00\x00",
        "bad_type",
    )
    mal(
        "malformed_bad_format.bin",
        header(fmt=3, samples=1, payload_len=2) + b"\x00\x00",
        "bad_format",
    )
    mal(
        "malformed_reserved_flags.bin",
        header(flags=0x40, samples=1, payload_len=2, fmt=1) + b"\x00\x00",
        "reserved_bits",
    )
    mal(
        "malformed_length_mismatch.bin",
        header(samples=1, payload_len=3, fmt=1) + b"\x00\x00",
        "length_mismatch",
    )
    # payload_len 3000 => message 3028 > max_frame_bytes (2048 used by tests)
    mal(
        "malformed_payload_too_large.bin",
        header(samples=1500, payload_len=3000, fmt=1) + pattern(3000),
        "payload_too_large",
    )
    mal(
        "malformed_opus_oversize.bin",
        header(fmt=2, samples=320, payload_len=256) + bytes([0x48]) + pattern(255),
        "opus_payload_too_large",
    )
    mal(
        "malformed_opus_empty.bin",
        header(fmt=2, samples=320, payload_len=0),
        "opus_empty_packet",
    )
    mal(
        "malformed_opus_bad_toc.bin",
        header(fmt=2, samples=320, payload_len=2) + bytes([0x03, 0x00]),
        "opus_empty_packet",
    )

    with open(os.path.join(HERE, "malformed_expected.json"), "w") as f:
        json.dump(malformed, f, indent=2, sort_keys=True)
        f.write("\n")

    print(f"generated {len(expected) + len(malformed)} vectors in {HERE}")


if __name__ == "__main__":
    main()
