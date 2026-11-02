// SPDX-License-Identifier: GPL-2.0-or-later
// Local integration fixture ONLY. Not a production authentication server.
// HTTPS binds loopback; stdin controls synthetic membership and the start gate.
package main

import (
	"bufio"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	modsync "octr.local/modsyncv2"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

func main() {
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		panic(err)
	}
	secret := hex.EncodeToString(key)
	var mu sync.Mutex
	room := modsync.Room{ID: "demo", Round: "1", Members: []string{"us", "eu", "disabled"}}
	nonces := map[string]int64{}
	auth := func(r *http.Request) (string, error) {
		mu.Lock()
		defer mu.Unlock()
		fields := strings.Split(r.Header.Get("X-RyuNET-Proof"), ".")
		if len(fields) != 4 || fields[0] != "1" {
			return "", errors.New("proof")
		}
		ts, err := strconv.ParseInt(fields[1], 10, 64)
		now := time.Now().Unix()
		if err != nil || ts < now-30 || ts > now+30 {
			return "", errors.New("timestamp")
		}
		nonce, err := hex.DecodeString(fields[2])
		if err != nil || len(nonce) != 16 {
			return "", errors.New("nonce")
		}
		for n, expiry := range nonces {
			if expiry < now {
				delete(nonces, n)
			}
		}
		if _, exists := nonces[fields[2]]; exists {
			return "", errors.New("replay")
		}
		mac := hmac.New(sha256.New, []byte(secret))
		fmt.Fprintf(mac, "%s\n%s\n%s\n%s", fields[1], fields[2], r.Method, r.RequestURI)
		supplied, err := hex.DecodeString(fields[3])
		if err != nil || !hmac.Equal(supplied, mac.Sum(nil)) {
			return "", errors.New("signature")
		}
		nick := r.URL.Query().Get("nick")
		if nick != "us" && nick != "eu" && nick != "disabled" {
			return "", errors.New("fixture account")
		}
		nonces[fields[2]] = now + 60
		return nick, nil
	}
	lookup := func(player string) (modsync.Room, error) {
		mu.Lock()
		defer mu.Unlock()
		for _, id := range room.Members {
			if id == player {
				copy := room
				copy.Members = append([]string(nil), room.Members...)
				return copy, nil
			}
		}
		return modsync.Room{}, errors.New("not in lobby")
	}
	bridge, err := modsync.NewBridge(auth, lookup)
	if err != nil {
		panic(err)
	}
	bridge.SetEnabled(true)
	server := httptest.NewTLSServer(bridge)
	defer server.Close()
	cert := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw})
	output := json.NewEncoder(os.Stdout)
	output.Encode(map[string]string{"origin": server.URL, "fixture_key": secret, "certificate": string(cert)})
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		var command struct {
			Countdown *bool     `json:"countdown"`
			Members   *[]string `json:"members"`
			Round     string    `json:"round"`
			Enabled   *bool     `json:"enabled"`
			Offset    int       `json:"offset_seconds"`
		}
		if json.Unmarshal(scanner.Bytes(), &command) != nil {
			output.Encode(map[string]string{"error": "command"})
			continue
		}
		mu.Lock()
		if command.Countdown != nil {
			room.Countdown = *command.Countdown
		}
		if command.Members != nil {
			room.Members = *command.Members
		}
		if command.Round != "" {
			room.Round = command.Round
		}
		if command.Enabled != nil {
			bridge.SetEnabled(*command.Enabled)
		}
		hold := bridge.BeforeRaceStart(room, time.Now().Add(time.Duration(command.Offset)*time.Second))
		mu.Unlock()
		output.Encode(map[string]bool{"hold": hold})
	}
}
