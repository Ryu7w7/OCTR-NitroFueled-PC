// SPDX-License-Identifier: GPL-2.0-or-later
package modsync

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func fixture() (Room, Request, time.Time) {
	room := Room{ID: "room", Round: "race-1", Members: []string{"owner", "peer", "opted-out"}, Countdown: true}
	req := Request{Protocol: 2, Offers: []Offer{{"https://gamebanana.com/mods/123456", 9999, 1000, strings.Repeat("a", 64)}}, Report: Report{Phase: "preparing"}}
	return room, req, time.Unix(1000, 0)
}

func ready(req Request, reply Reply) Request {
	req.Report = Report{Phase: "ready", Session: reply.Session, Generation: reply.Generation}
	for _, offer := range reply.Offers {
		req.Report.Loaded = append(req.Report.Loaded, offer.SHA256)
	}
	return req
}

func TestHoldUntilLoadedOptOutDoesNotBlock(t *testing.T) {
	c := New()
	room, req, now := fixture()
	reply, err := c.Exchange(room, "owner", req, now)
	if err != nil || !reply.Hold {
		t.Fatal(reply, err)
	}
	// No exchange from opted-out: this player must NEVER prevent the countdown.
	reply, err = c.Exchange(room, "owner", ready(req, reply), now.Add(time.Second))
	if err != nil || reply.Hold {
		t.Fatal(reply, err)
	}
}

func TestTimeoutCannotBeExtendedByRosterOrOfferChanges(t *testing.T) {
	c := New()
	room, req, now := fixture()
	_, _ = c.Exchange(room, "owner", req, now)
	for i := 1; i <= 25; i++ {
		req.Offers[0].FileID++
		req.Offers[0].SHA256 = strings.Repeat("b", 64)
		room.Members = append(room.Members[:2], "newcomer")
		reply, err := c.Exchange(room, "owner", req, now.Add(time.Duration(i)*time.Second))
		if err != nil {
			t.Fatal(err)
		}
		if i >= 20 && reply.Hold {
			t.Fatal("deadline was extended")
		}
	}
}

func TestFailureHeartbeatExpiryAndReleasedLatch(t *testing.T) {
	for _, scenario := range []string{"failure", "expired", "left"} {
		t.Run(scenario, func(t *testing.T) {
			c := New()
			room, req, now := fixture()
			_, _ = c.Exchange(room, "owner", req, now)
			switch scenario {
			case "failure":
				req.Report.Phase = "fallback"
				_, _ = c.Exchange(room, "owner", req, now.Add(time.Second))
			case "expired":
				now = now.Add(8 * time.Second)
			case "left":
				room.Members = []string{"peer"}
			}
			if c.ShouldHold(room, now.Add(2*time.Second)) {
				t.Fatal("fallback kept room held")
			}
			room.Members = []string{"owner", "peer"}
			req.Report.Phase = "preparing"
			reply, _ := c.Exchange(room, "owner", req, now.Add(3*time.Second))
			if reply.Hold {
				t.Fatal("released countdown was held again")
			}
		})
	}
}

func TestReadyProofRequiresCurrentGenerationAndAllLoadedHashes(t *testing.T) {
	for _, scenario := range []string{"old-generation", "old-session", "missing", "wrong-hash"} {
		t.Run(scenario, func(t *testing.T) {
			c := New()
			room, req, now := fixture()
			reply, _ := c.Exchange(room, "owner", req, now)
			req = ready(req, reply)
			switch scenario {
			case "old-generation":
				req.Report.Generation = "stale"
			case "old-session":
				req.Report.Session = "other"
			case "missing":
				req.Report.Loaded = nil
			case "wrong-hash":
				req.Report.Loaded = []string{strings.Repeat("b", 64)}
			}
			reply, err := c.Exchange(room, "owner", req, now.Add(time.Second))
			if err != nil || !reply.Hold {
				t.Fatal("bad proof released countdown", reply, err)
			}
		})
	}
}

func TestForeignPlayerAndMalformedOffersRejected(t *testing.T) {
	c := New()
	room, req, now := fixture()
	if _, err := c.Exchange(room, "intruder", req, now); err == nil {
		t.Fatal("foreign member admitted")
	}
	req.Offers[0].PageURL = "https://evil.example/mod.zip"
	if _, err := c.Exchange(room, "owner", req, now); err == nil {
		t.Fatal("arbitrary source admitted")
	}
}

func TestRoundChangeClearsPriorProofs(t *testing.T) {
	c := New()
	room, req, now := fixture()
	reply, _ := c.Exchange(room, "owner", req, now)
	req = ready(req, reply)
	_, _ = c.Exchange(room, "owner", req, now.Add(time.Second))
	room.Round = "race-2"
	reply, err := c.Exchange(room, "owner", req, now.Add(2*time.Second))
	if err != nil || !reply.Hold {
		t.Fatal("previous race readiness was reused")
	}
}

func TestEmptyOrUnsupportedLobbyNeverHeld(t *testing.T) {
	c := New()
	room, req, now := fixture()
	if c.ShouldHold(room, now) {
		t.Fatal("nonparticipant room held")
	}
	req.Offers = nil
	reply, _ := c.Exchange(room, "owner", req, now)
	if reply.Hold {
		t.Fatal("empty requirements held")
	}
}

func TestMultipleParticipantsAndFailureFallback(t *testing.T) {
	c := New()
	room, req, now := fixture()
	reply, _ := c.Exchange(room, "owner", req, now)
	_, _ = c.Exchange(room, "peer", req, now)
	reply, _ = c.Exchange(room, "owner", ready(req, reply), now.Add(time.Second))
	if !reply.Hold {
		t.Fatal("peer still preparing")
	}
	req.Report.Phase = "fallback"
	reply, _ = c.Exchange(room, "peer", req, now.Add(2*time.Second))
	if reply.Hold {
		t.Fatal("failed peer blocks race")
	}
}

func TestHTTPAuthAndSignedBodyBoundary(t *testing.T) {
	room, req, _ := fixture()
	payload, _ := json.Marshal(req)
	digest := sha256.Sum256(payload)
	target := "/ryunet/modsync/v2/exchange?nick=owner&body_sha256=" + hex.EncodeToString(digest[:])
	called := 0
	handler := Handler{New(), func(r *http.Request) (string, error) {
		called++
		if r.Header.Get("X-RyuNET-Proof") != "test-proof" {
			return "", errors.New("invalid proof")
		}
		if r.RequestURI != target {
			t.Fatal("signed target changed")
		}
		return "owner", nil
	}, func(string) (Room, error) { return room, nil }}
	request := httptest.NewRequest("POST", target, bytes.NewReader(payload))
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != 401 {
		t.Fatal(response.Code)
	}
	request = httptest.NewRequest("POST", target, bytes.NewReader(payload))
	request.Header.Set("X-RyuNET-Proof", "test-proof")
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != 200 {
		t.Fatal(response.Code, response.Body.String())
	}
	request = httptest.NewRequest("POST", target, strings.NewReader("tampered"))
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != 400 || called != 2 {
		t.Fatal("body tampering reached authenticator")
	}
}
