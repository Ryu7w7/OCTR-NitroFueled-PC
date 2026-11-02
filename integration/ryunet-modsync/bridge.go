// SPDX-License-Identifier: GPL-2.0-or-later
package modsync

import (
	"errors"
	"net/http"
	"sync/atomic"
	"time"
)

// Bridge is the server integration surface. Disabled until explicitly enabled.
// Authentication and membership stay owned by the host server. No bridge lock
// is held while calling these adapters: RoomFor may acquire the matchmaker lock.
type Bridge struct {
	active       atomic.Pointer[Coordinator]
	authenticate func(*http.Request) (string, error)
	roomFor      func(string) (Room, error)
}

func NewBridge(authenticate func(*http.Request) (string, error), roomFor func(string) (Room, error)) (*Bridge, error) {
	if authenticate == nil || roomFor == nil {
		return nil, errors.New("mod sync requires authentication and membership adapters")
	}
	return &Bridge{authenticate: authenticate, roomFor: roomFor}, nil
}

// SetEnabled is an administrator kill switch. Repeated enable calls preserve
// deadlines; disable/re-enable starts a fresh coordinator with no stale proofs.
func (b *Bridge) SetEnabled(enabled bool) {
	if !enabled {
		b.active.Store(nil)
		return
	}
	if b.active.Load() == nil {
		b.active.CompareAndSwap(nil, New())
	}
}

func (b *Bridge) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	coordinator := b.active.Load()
	if coordinator == nil {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = w.Write([]byte(`{"error":"modsync_disabled"}`))
		return
	}
	Handler{coordinator, b.authenticate, b.roomFor}.ServeHTTP(w, r)
}

// BeforeRaceStart receives a snapshot from the ALREADY LOCKED matchmaker.
// It never calls RoomFor, avoiding recursive lock acquisition in the close hook.
// True defers only this attempt; false continues the normal start/fallback path.
func (b *Bridge) BeforeRaceStart(room Room, now time.Time) bool {
	coordinator := b.active.Load()
	return coordinator != nil && coordinator.ShouldHold(room, now)
}

func (b *Bridge) RoomDestroyed(id string) {
	if coordinator := b.active.Load(); coordinator != nil {
		coordinator.RemoveRoom(id)
	}
}
