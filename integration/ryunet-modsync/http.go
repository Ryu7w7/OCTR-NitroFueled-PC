// SPDX-License-Identifier: GPL-2.0-or-later
package modsync

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"time"
)

// Authenticate MUST call the current server's nickname proof/replay verifier on
// method + exact RequestURI, including body_sha256. Never trust the nick query alone.
// RoomFor MUST look up current server membership, not a client-provided room ID.
type Handler struct {
	Coordinator  *Coordinator
	Authenticate func(*http.Request) (string, error)
	RoomFor      func(string) (Room, error)
}

func (h Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	if r.URL.Path == "/ryunet/modsync/v2/capabilities" && r.Method == "GET" {
		_ = json.NewEncoder(w).Encode(map[string]any{"protocol": 2, "optional_fallback": true, "max_hold_seconds": 20})
		return
	}
	if r.URL.Path != "/ryunet/modsync/v2/exchange" || r.Method != "POST" {
		http.Error(w, `{"error":"not_found"}`, http.StatusNotFound)
		return
	}
	if h.Coordinator == nil || h.Authenticate == nil || h.RoomFor == nil {
		http.Error(w, `{"error":"adapter_not_configured"}`, http.StatusServiceUnavailable)
		return
	}
	raw, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 65536))
	if err != nil {
		http.Error(w, `{"error":"request_limit"}`, http.StatusBadRequest)
		return
	}
	digest := sha256.Sum256(raw)
	if r.URL.Query().Get("body_sha256") != hex.EncodeToString(digest[:]) {
		http.Error(w, `{"error":"body_digest"}`, http.StatusBadRequest)
		return
	}
	player, err := h.Authenticate(r)
	if err != nil || player == "" {
		http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
		return
	}
	room, err := h.RoomFor(player)
	if err != nil {
		http.Error(w, `{"error":"no_lobby"}`, http.StatusConflict)
		return
	}
	var req Request
	// Decode from the already bounded bytes; strict field names prevent protocol ambiguity.
	req, err = decodeRequest(raw)
	if err != nil {
		http.Error(w, `{"error":"invalid_request"}`, http.StatusBadRequest)
		return
	}
	reply, err := h.Coordinator.Exchange(room, player, req, time.Now())
	if err != nil {
		http.Error(w, `{"error":"invalid_state"}`, http.StatusBadRequest)
		return
	}
	_ = json.NewEncoder(w).Encode(reply)
}

func decodeRequest(raw []byte) (Request, error) {
	var req Request
	strict := json.NewDecoder(bytes.NewReader(raw))
	if err := uniqueJSON(strict, 0); err != nil {
		return req, err
	}
	if _, err := strict.Token(); err != io.EOF {
		return req, errors.New("trailing JSON data")
	}
	var object map[string]json.RawMessage
	if json.Unmarshal(raw, &object) != nil || len(object) != 3 || object["protocol"] == nil || object["offers"] == nil || object["report"] == nil {
		return req, errors.New("invalid request envelope")
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	err := decoder.Decode(&req)
	return req, err
}

// encoding/json accepts duplicate keys by default. Reject them at every level
// so signed payloads have the same interpretation in client and server tooling.
func uniqueJSON(decoder *json.Decoder, depth int) error {
	if depth > 32 {
		return errors.New("JSON nesting limit")
	}
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, compound := token.(json.Delim)
	if !compound {
		return nil
	}
	if delimiter != '{' && delimiter != '[' {
		return errors.New("invalid JSON container")
	}
	keys := make(map[string]bool)
	for decoder.More() {
		if delimiter == '{' {
			keyToken, err := decoder.Token()
			if err != nil {
				return err
			}
			key, ok := keyToken.(string)
			if !ok || keys[key] {
				return errors.New("duplicate JSON key")
			}
			keys[key] = true
		}
		if err := uniqueJSON(decoder, depth+1); err != nil {
			return err
		}
	}
	_, err = decoder.Token()
	return err
}
