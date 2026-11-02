// SPDX-License-Identifier: GPL-2.0-or-later
package modsync

import (
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"
)

func testBridge(t *testing.T) *Bridge {
	t.Helper()
	b, err := NewBridge(func(*http.Request) (string, error) { return "owner", nil }, func(string) (Room, error) { room, _, _ := fixture(); return room, nil })
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestBridgeDisabledDefaultAndKillSwitch(t *testing.T) {
	b := testBridge(t)
	room, req, now := fixture()
	w := httptest.NewRecorder()
	b.ServeHTTP(w, httptest.NewRequest("GET", "/ryunet/modsync/v2/capabilities", nil))
	if w.Code != 503 || b.BeforeRaceStart(room, now) {
		t.Fatal("disabled bridge active")
	}
	b.SetEnabled(true)
	first := b.active.Load()
	reply, err := first.Exchange(room, "owner", req, now)
	if err != nil || !reply.Hold {
		t.Fatal(reply, err)
	}
	b.SetEnabled(true)
	if b.active.Load() != first {
		t.Fatal("repeated enable reset deadline")
	}
	b.SetEnabled(false)
	if b.BeforeRaceStart(room, now) {
		t.Fatal("kill switch retained hold")
	}
	b.SetEnabled(true)
	if b.active.Load() == first || b.BeforeRaceStart(room, now) {
		t.Fatal("old readiness leaked into new instance")
	}
}

func TestBridgeCloseHookDoesNotReacquireMembershipLock(t *testing.T) {
	var matchmaker sync.Mutex
	room, req, now := fixture()
	b, err := NewBridge(func(*http.Request) (string, error) { return "owner", nil }, func(string) (Room, error) { matchmaker.Lock(); defer matchmaker.Unlock(); return room, nil })
	if err != nil {
		t.Fatal(err)
	}
	b.SetEnabled(true)
	_, _ = b.active.Load().Exchange(room, "owner", req, now)
	done := make(chan bool, 1)
	go func() { matchmaker.Lock(); defer matchmaker.Unlock(); done <- b.BeforeRaceStart(room, now) }()
	select {
	case held := <-done:
		if !held {
			t.Fatal("expected hold")
		}
	case <-time.After(time.Second):
		t.Fatal("recursive matchmaker lock")
	}
	b.RoomDestroyed(room.ID)
	if b.BeforeRaceStart(room, now) {
		t.Fatal("destroyed room retained hold")
	}
}

func TestBridgeConcurrentConfigurationAndRequests(t *testing.T) {
	b := testBridge(t)
	var group sync.WaitGroup
	for worker := 0; worker < 8; worker++ {
		group.Add(1)
		go func() {
			defer group.Done()
			for i := 0; i < 100; i++ {
				b.SetEnabled(i%2 == 0)
				w := httptest.NewRecorder()
				b.ServeHTTP(w, httptest.NewRequest("GET", "/ryunet/modsync/v2/capabilities", nil))
				if w.Code != 200 && w.Code != 503 {
					t.Errorf("unexpected code %d", w.Code)
				}
			}
		}()
	}
	group.Wait()
}

func TestBridgeRejectsMissingAdapters(t *testing.T) {
	if _, err := NewBridge(nil, nil); err == nil {
		t.Fatal("unconfigured bridge accepted")
	}
}
