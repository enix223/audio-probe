// recorderd is the recorder data-plane server: WebSocket ingest, capture
// storage, metrics, reports, REST API, and dashboard.
package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"audio-probe/server/internal/httpapi"
	"audio-probe/server/internal/hub"
	"audio-probe/server/internal/metrics"
	"audio-probe/server/internal/storage"
	"audio-probe/server/internal/wsapi"
)

func main() {
	var (
		addr          = flag.String("addr", ":8080", "WebSocket listen address")
		httpAddr      = flag.String("http", ":8081", "HTTP API/dashboard listen address")
		out           = flag.String("out", "./data", "capture output directory")
		token         = flag.String("token", "", "device bearer token (empty = no auth; lab only)")
		cert          = flag.String("cert", "", "PEM certificate for WSS (empty = plaintext ws://, lab only)")
		key           = flag.String("key", "", "PEM private key for WSS")
		httpCert      = flag.String("http-cert", "", "optional PEM certificate for HTTPS API")
		httpKey       = flag.String("http-key", "", "optional PEM private key for HTTPS API")
		format        = flag.String("format", "auto", "audio format: auto, pcm_s16le, opus")
		rate          = flag.Int("rate", 16000, "preferred sample rate")
		channels      = flag.Int("channels", 1, "preferred channel count")
		frameMS       = flag.Int("frame-ms", 20, "preferred frame duration")
		mode          = flag.String("mode", "auto", "capture mode: auto, vad, continuous")
		preRollMS     = flag.Int("pre-roll-ms", 300, "server-requested device pre-roll")
		maxSegmentMS  = flag.Int("max-segment-ms", 15000, "maximum segment duration")
		maxFrameBytes = flag.Int("max-frame-bytes", 8192, "maximum binary message size")
		onsetMS       = flag.Int("vad-onset-ms", 120, "VAD onset debounce")
		silenceMS     = flag.Int("vad-silence-ms", 700, "VAD trailing silence")
		minSpeechMS   = flag.Int("vad-min-speech-ms", 150, "VAD minimum speech")
		thresholdDB   = flag.Float64("vad-threshold-db", 6, "energy VAD margin above the noise floor")
		levelMS       = flag.Int("level-interval-ms", 500, "telemetry level interval")
		statsMS       = flag.Int("stats-interval-ms", 5000, "telemetry stats interval")
	)
	flag.Parse()

	logger := log.New(os.Stderr, "recorderd ", log.LstdFlags|log.Lmicroseconds)
	layout, err := storage.New(*out)
	if err != nil {
		logger.Fatalf("storage: %v", err)
	}
	h := hub.New()

	opts := wsapi.DefaultOptions()
	opts.Token = *token
	opts.Format = *format
	opts.SampleRate = *rate
	opts.Channels = *channels
	opts.FrameMS = *frameMS
	opts.Mode = *mode
	opts.PreRollMS = *preRollMS
	opts.MaxSegmentMS = *maxSegmentMS
	opts.MaxFrameBytes = *maxFrameBytes
	opts.LevelInterval = *levelMS
	opts.StatsInterval = *statsMS
	opts.VAD = metrics.VADConfig{
		FrameMS:     *frameMS,
		OnsetMS:     *onsetMS,
		SilenceMS:   *silenceMS,
		MinSpeechMS: *minSpeechMS,
		PreRollMS:   *preRollMS,
		ThresholdDB: *thresholdDB,
	}

	wsHandler := wsapi.New(opts, layout, h, logger)
	apiHandler := httpapi.New(h, layout, logger).Handler()

	wsSrv := &http.Server{Addr: *addr, Handler: wsHandler, ReadHeaderTimeout: 10 * time.Second}
	apiSrv := &http.Server{Addr: *httpAddr, Handler: apiHandler, ReadHeaderTimeout: 10 * time.Second}

	if *token == "" {
		logger.Printf("WARNING: WebSocket token authentication is disabled (develop/lab only)")
	}
	if *cert == "" {
		logger.Printf("WARNING: WSS is disabled; the data plane uses plaintext ws:// (develop/lab only)")
	}

	errCh := make(chan error, 2)
	go func() {
		scheme := "ws"
		if *cert != "" {
			scheme = "wss"
		}
		logger.Printf("WebSocket data plane (%s) on %s", scheme, *addr)
		var err error
		if *cert != "" {
			err = wsSrv.ListenAndServeTLS(*cert, *key)
		} else {
			err = wsSrv.ListenAndServe()
		}
		if err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()
	go func() {
		scheme := "http"
		if *httpCert != "" {
			scheme = "https"
		}
		logger.Printf("HTTP API/dashboard (%s) on %s", scheme, *httpAddr)
		var err error
		if *httpCert != "" {
			err = apiSrv.ListenAndServeTLS(*httpCert, *httpKey)
		} else {
			err = apiSrv.ListenAndServe()
		}
		if err != nil && err != http.ErrServerClosed {
			errCh <- err
		}
	}()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	select {
	case sig := <-sigCh:
		logger.Printf("shutting down (%s)", sig)
	case err := <-errCh:
		logger.Printf("server error: %v", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = wsSrv.Shutdown(ctx)
	_ = apiSrv.Shutdown(ctx)
}
