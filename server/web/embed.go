// Package web embeds the production React build served by the Go API.
//
// Source lives in this directory; `scripts/build_web.sh` (or `make web`)
// regenerates dist/, which is embedded so `go build` produces a self-contained
// server binary.
package web

import "embed"

//go:embed all:dist
var Dist embed.FS
