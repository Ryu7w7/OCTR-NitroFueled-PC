// SPDX-License-Identifier: GPL-2.0-or-later
package modsync

import (
	"fmt"
	"strings"
	"testing"
	"time"
)

func TestExpiredRoomCapacityRecovers(t *testing.T) {
	c := New()
	room, req, now := fixture()
	for i := 0; i < 4096; i++ {
		c.rooms[fmt.Sprint(i)] = &roomState{touched: now.Add(-11 * time.Minute)}
	}
	if _, err := c.Exchange(room, "owner", req, now); err != nil {
		t.Fatal(err)
	}
	if len(c.rooms) != 1 {
		t.Fatal("expired room capacity retained")
	}
}

func TestDuplicateOrEmptyMembershipRejected(t *testing.T) {
	for _, members := range [][]string{{"owner", "owner"}, {"owner", ""}} {
		c := New()
		room, req, now := fixture()
		room.Members = members
		if _, err := c.Exchange(room, "owner", req, now); err == nil {
			t.Fatal("malformed roster accepted")
		}
		if c.ShouldHold(room, now) {
			t.Fatal("invalid roster blocked start")
		}
	}
}

func TestAmbiguousJSONRejected(t *testing.T) {
	for _, raw := range []string{
		`{"protocol":2,"protocol":2,"offers":[],"report":{"phase":"fallback"}}`,
		`{"protocol":2,"offers":[],"report":{"phase":"ready","phase":"fallback"}}`,
		`{"protocol":2,"offers":[{"file_id":1,"file_id":2}],"report":{"phase":"fallback"}}`,
		`{"protocol":2,"offers":[],"report":{}} {}`,
		strings.Repeat("[", 34) + "0" + strings.Repeat("]", 34),
	} {
		if _, err := decodeRequest([]byte(raw)); err == nil {
			t.Fatal("ambiguous request accepted:", raw)
		}
	}
	if _, err := decodeRequest([]byte(`{"protocol":2,"offers":[],"report":{"phase":"fallback","session":"","generation":"","loaded":[]}}`)); err != nil {
		t.Fatal(err)
	}
}
