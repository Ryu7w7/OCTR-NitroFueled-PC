// SPDX-License-Identifier: GPL-2.0-or-later
// Package modsync implements optional, bounded lobby readiness. It never loads
// files or modifies character packets; OCTR's missing-racer fallback remains on.
package modsync

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"sync"
	"time"
)

const MaxHold = 20 * time.Second
const HeartbeatTTL = 7 * time.Second
const MaxOffers = 32

var pagePattern = regexp.MustCompile(`^https://gamebanana\.com/mods/[1-9][0-9]{0,9}$`)
var digestPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

type Offer struct {
	PageURL string `json:"page_url"`
	FileID  int64  `json:"file_id"`
	Bytes   int64  `json:"bytes"`
	SHA256  string `json:"sha256"`
}

func (o Offer) valid() bool {
	return pagePattern.MatchString(o.PageURL) && o.FileID > 0 && o.FileID < 10000000000 &&
		o.Bytes > 0 && o.Bytes <= 512*1024*1024 && digestPattern.MatchString(o.SHA256)
}

type Report struct {
	Phase      string   `json:"phase"`
	Session    string   `json:"session"`
	Generation string   `json:"generation"`
	Loaded     []string `json:"loaded"`
}

type Request struct {
	Protocol int     `json:"protocol"`
	Offers   []Offer `json:"offers"`
	Report   Report  `json:"report"`
}

type Reply struct {
	Protocol   int     `json:"protocol"`
	Session    string  `json:"session"`
	Generation string  `json:"generation"`
	ExpiresAt  int64   `json:"expires_at"`
	Offers     []Offer `json:"offers"`
	Hold       bool    `json:"hold"`
}

// Room comes ONLY from the existing server's authoritative membership/countdown.
// Round is a unique, server-generated attempt ID, renewed for each next race.
type Room struct {
	ID, Round string
	Members   []string
	Countdown bool
}
type participant struct {
	offers []Offer
	report Report
	seen   time.Time
}
type roomState struct {
	round    string
	members  map[string]participant
	deadline time.Time
	released bool
	touched  time.Time
}

type Coordinator struct {
	mu    sync.Mutex
	rooms map[string]*roomState
}

func New() *Coordinator { return &Coordinator{rooms: make(map[string]*roomState)} }

func contains(values []string, wanted string) bool {
	for _, value := range values {
		if value == wanted {
			return true
		}
	}
	return false
}

func validateRoom(room Room) error {
	if room.ID == "" || room.Round == "" || len(room.ID) > 128 || len(room.Round) > 128 || len(room.Members) > 8 {
		return errors.New("invalid authoritative room")
	}
	return nil
}

func (c *Coordinator) state(room Room, now time.Time) *roomState {
	for id, state := range c.rooms {
		if now.Sub(state.touched) > 10*time.Minute {
			delete(c.rooms, id)
		}
	}
	state := c.rooms[room.ID]
	if state == nil || state.round != room.Round {
		state = &roomState{round: room.Round, members: make(map[string]participant)}
		c.rooms[room.ID] = state
	}
	state.touched = now
	for id := range state.members {
		if !contains(room.Members, id) {
			delete(state.members, id)
		}
	}
	if room.Countdown && state.deadline.IsZero() {
		state.deadline = now.Add(MaxHold)
	}
	return state
}

func snapshot(room Room, state *roomState, now time.Time) Reply {
	// Exact upload descriptors participate in generation identity, not just hashes.
	byHash := make(map[string]Offer)
	conflicts := make(map[string]bool)
	for _, p := range state.members {
		if now.Sub(p.seen) > HeartbeatTTL {
			continue
		}
		for _, offer := range p.offers {
			if old, ok := byHash[offer.SHA256]; ok && old != offer {
				conflicts[offer.SHA256] = true
			}
			byHash[offer.SHA256] = offer
		}
	}
	offers := make([]Offer, 0)
	for hash, offer := range byHash {
		if !conflicts[hash] {
			offers = append(offers, offer)
		}
	}
	sort.Slice(offers, func(i, j int) bool { return offers[i].SHA256 < offers[j].SHA256 })
	if len(offers) > MaxOffers {
		offers = offers[:MaxOffers]
	}
	members := append([]string(nil), room.Members...)
	sort.Strings(members)
	raw, _ := json.Marshal(struct {
		Round   string
		Members []string
		Offers  []Offer
	}{room.Round, members, offers})
	digest := sha256.Sum256(raw)
	generation := hex.EncodeToString(digest[:])
	hold := false
	for _, p := range state.members {
		if now.Sub(p.seen) > HeartbeatTTL || p.report.Phase == "fallback" {
			continue
		}
		ready := p.report.Phase == "ready" && p.report.Session == room.ID && p.report.Generation == generation
		if len(p.report.Loaded) != len(offers) {
			ready = false
		}
		for _, offer := range offers {
			if !contains(p.report.Loaded, offer.SHA256) {
				ready = false
			}
		}
		if !ready && len(offers) > 0 {
			hold = true
		}
	}
	if !state.deadline.IsZero() && !now.Before(state.deadline) {
		state.released = true
	}
	hold = hold && room.Countdown && !state.released
	// Once released, later progress packets cannot start the hold again this race.
	if room.Countdown && !hold {
		state.released = true
	}
	return Reply{2, room.ID, generation, now.Add(15 * time.Second).Unix(), offers, hold}
}

func (c *Coordinator) Exchange(room Room, player string, req Request, now time.Time) (Reply, error) {
	if err := validateRoom(room); err != nil {
		return Reply{}, err
	}
	if !contains(room.Members, player) {
		return Reply{}, errors.New("not a member of this lobby")
	}
	if req.Protocol != 2 || len(req.Offers) > MaxOffers || len(req.Report.Loaded) > MaxOffers ||
		(req.Report.Phase != "preparing" && req.Report.Phase != "fallback" && req.Report.Phase != "ready") ||
		len(req.Report.Session) > 128 || len(req.Report.Generation) > 128 {
		return Reply{}, errors.New("invalid mod sync report")
	}
	seen := make(map[string]bool)
	for _, offer := range req.Offers {
		if !offer.valid() || seen[offer.SHA256] {
			return Reply{}, errors.New("invalid or duplicate offer")
		}
		seen[offer.SHA256] = true
	}
	seen = make(map[string]bool)
	for _, digest := range req.Report.Loaded {
		if !digestPattern.MatchString(digest) || seen[digest] {
			return Reply{}, errors.New("invalid loaded proof")
		}
		seen[digest] = true
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.rooms) >= 4096 && c.rooms[room.ID] == nil {
		return Reply{}, fmt.Errorf("sync capacity reached")
	}
	state := c.state(room, now)
	// Copy caller-owned slices so the state cannot be changed outside the mutex.
	req.Report.Loaded = append([]string(nil), req.Report.Loaded...)
	state.members[player] = participant{append([]Offer(nil), req.Offers...), req.Report, now}
	return snapshot(room, state, now), nil
}

// ShouldHold must be called by the REAL server match-start check, not just a UI timer.
// A failed adapter call should preserve normal OCTR behaviour, never lock the room.
func (c *Coordinator) ShouldHold(room Room, now time.Time) bool {
	if validateRoom(room) != nil {
		return false
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.rooms[room.ID] == nil || c.rooms[room.ID].round != room.Round {
		return false
	}
	return snapshot(room, c.state(room, now), now).Hold
}

func (c *Coordinator) RemoveRoom(id string) { c.mu.Lock(); defer c.mu.Unlock(); delete(c.rooms, id) }
