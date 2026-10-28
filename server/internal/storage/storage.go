// Package storage owns the on-disk capture layout.
package storage

import (
	"crypto/rand"
	"encoding/base32"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Layout is the capture root (data/captures/<capture_id>/...).
type Layout struct {
	Root string
}

func New(root string) (*Layout, error) {
	if root == "" {
		root = "./data"
	}
	if err := os.MkdirAll(filepath.Join(root, "captures"), 0o755); err != nil {
		return nil, err
	}
	return &Layout{Root: root}, nil
}

func validID(id string) bool {
	if id == "" || len(id) > 64 {
		return false
	}
	for _, r := range id {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') || r == '-' || r == '_' {
			continue
		}
		return false
	}
	return true
}

func (l *Layout) CaptureDir(id string) (string, error) {
	if !validID(id) {
		return "", fmt.Errorf("invalid capture id")
	}
	return filepath.Join(l.Root, "captures", id), nil
}

func (l *Layout) EnsureCapture(id string) (string, error) {
	dir, err := l.CaptureDir(id)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Join(dir, "segments"), 0o755); err != nil {
		return "", err
	}
	return dir, nil
}

func (l *Layout) SegmentDir(captureID string, segmentID uint32) (string, error) {
	base, err := l.CaptureDir(captureID)
	if err != nil {
		return "", err
	}
	return filepath.Join(base, "segments", strconv.FormatUint(uint64(segmentID), 10)), nil
}

func (l *Layout) EnsureSegment(captureID string, segmentID uint32) (string, error) {
	dir, err := l.SegmentDir(captureID, segmentID)
	if err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Join(dir, "plots"), 0o755); err != nil {
		return "", err
	}
	return dir, nil
}

// WriteJSON writes atomically (temp file + rename).
func WriteJSON(path string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

type CaptureSummary struct {
	ID        string    `json:"id"`
	Path      string    `json:"path"`
	StartedAt time.Time `json:"started_at,omitempty"`
	DeviceID  string    `json:"device_id,omitempty"`
	Format    string    `json:"format,omitempty"`
	Segments  int       `json:"segments"`
}

func (l *Layout) ListCaptures() ([]CaptureSummary, error) {
	dir := filepath.Join(l.Root, "captures")
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []CaptureSummary
	for _, e := range entries {
		if !e.IsDir() || !validID(e.Name()) {
			continue
		}
		cs := CaptureSummary{ID: e.Name(), Path: filepath.Join(dir, e.Name())}
		if data, err := os.ReadFile(filepath.Join(cs.Path, "manifest.json")); err == nil {
			var m struct {
				DeviceID  string    `json:"device_id"`
				Format    string    `json:"format"`
				StartedAt time.Time `json:"started_at"`
				Segments  []any     `json:"segments"`
			}
			if json.Unmarshal(data, &m) == nil {
				cs.DeviceID = m.DeviceID
				cs.Format = m.Format
				cs.StartedAt = m.StartedAt
				cs.Segments = len(m.Segments)
			}
		}
		// The manifest is finalized when the session closes; while a device is
		// still connected, count the segment directories so the list is live.
		if segs, err := os.ReadDir(filepath.Join(cs.Path, "segments")); err == nil {
			count := 0
			for _, s := range segs {
				if s.IsDir() {
					count++
				}
			}
			if count > cs.Segments {
				cs.Segments = count
			}
		}
		out = append(out, cs)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID > out[j].ID })
	return out, nil
}

// NewID returns a time-ordered, filesystem-safe capture/session id.
func NewID(prefix string) string {
	var b [10]byte
	_, _ = rand.Read(b[:])
	ts := time.Now().UTC().Format("20060102T150405")
	enc := base32.StdEncoding.WithPadding(base32.NoPadding)
	tail := strings.ToLower(enc.EncodeToString(b[:]))
	return prefix + "-" + ts + "-" + tail[:10]
}
