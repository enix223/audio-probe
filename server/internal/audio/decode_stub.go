//go:build !opus

package audio

// The default build preserves raw Opus packets and marks them undecoded.
// Build with -tags "opus nolibopusfile" and libopus available through
// pkg-config to produce WAVs from Opus. nolibopusfile avoids the package's
// optional libopusfile stream helper, which the recorder does not use.
