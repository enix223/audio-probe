// recorderctl sends recorderCmd OneJSON property sets to the lab broker and
// prints recorderStatus updates. It is the command-line operator tool for
// bring-up when the cloud console is not available.
package main

import (
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"time"

	paho "github.com/eclipse/paho.mqtt.golang"
)

func main() {
	var (
		broker  = flag.String("broker", "ssl://127.0.0.1:1883", "MQTT broker URL")
		caFile  = flag.String("ca", "", "CA bundle for MQTT TLS")
		product = flag.String("product", "recorder-lab", "IoT product id")
		device  = flag.String("device", "recorder-linux", "IoT device id")
		action  = flag.String("action", "start", "start, stop, status, configure, test_tone")
		params  = flag.String("params", "{}", "action params as a JSON object")
		id      = flag.String("id", "", "command id (default: generated)")
		wait    = flag.Duration("wait", 8*time.Second, "how long to wait for a matching status")
	)
	flag.Parse()

	if *id == "" {
		*id = randomID()
	}
	var paramsObj map[string]any
	if err := json.Unmarshal([]byte(*params), &paramsObj); err != nil {
		log.Fatalf("params: %v", err)
	}
	envelope := map[string]any{
		"v": 1, "type": "recorder_cmd", "id": *id, "action": *action, "params": paramsObj,
	}
	envBytes, err := json.Marshal(envelope)
	if err != nil {
		log.Fatal(err)
	}
	payload, _ := json.Marshal(map[string]any{
		"id":      randomID(),
		"version": "1.0",
		"params":  map[string]any{"recorderCmd": string(envBytes)},
	})

	setTopic := fmt.Sprintf("$sys/%s/%s/thing/property/set", *product, *device)
	postTopic := fmt.Sprintf("$sys/%s/%s/thing/property/post", *product, *device)

	opts := paho.NewClientOptions().
		AddBroker(*broker).
		SetClientID("recorderctl-" + randomID()).
		SetCleanSession(true).
		SetAutoReconnect(false).
		SetConnectTimeout(5 * time.Second)
	if *caFile != "" {
		pem, err := os.ReadFile(*caFile)
		if err != nil {
			log.Fatalf("read CA: %v", err)
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(pem) {
			log.Fatalf("no certificates in %s", *caFile)
		}
		opts.SetTLSConfig(&tls.Config{RootCAs: pool, MinVersion: tls.VersionTLS12})
	}
	client := paho.NewClient(opts)
	tok := client.Connect()
	if !tok.WaitTimeout(5*time.Second) || tok.Error() != nil {
		log.Fatalf("connect %s: %v", *broker, tok.Error())
	}
	defer client.Disconnect(250)

	matched := make(chan string, 4)
	if st := client.Subscribe(postTopic, 0, func(_ paho.Client, msg paho.Message) {
		text, cmdID := extractStatus(msg.Payload())
		if cmdID == *id {
			select {
			case matched <- text:
			default:
			}
		}
	}); !st.WaitTimeout(5*time.Second) || st.Error() != nil {
		log.Fatalf("subscribe %s: %v", postTopic, st.Error())
	}
	if pt := client.Publish(setTopic, 0, false, payload); !pt.WaitTimeout(5*time.Second) || pt.Error() != nil {
		log.Fatalf("publish %s: %v", setTopic, pt.Error())
	}
	log.Printf("sent id=%s action=%s", *id, *action)

	select {
	case text := <-matched:
		fmt.Println(text)
	case <-time.After(*wait):
		log.Printf("no matching recorderStatus within %s", *wait)
		os.Exit(2)
	}
}

func randomID() string {
	var b [4]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

// extractStatus pulls the recorderStatus string out of a OneJSON property post.
func extractStatus(payload []byte) (string, string) {
	var outer struct {
		Params map[string]json.RawMessage `json:"params"`
	}
	if json.Unmarshal(payload, &outer) != nil {
		return string(payload), ""
	}
	raw, ok := outer.Params["recorderStatus"]
	if !ok {
		return string(payload), ""
	}
	var wrapper struct {
		Value string `json:"value"`
	}
	text := ""
	if json.Unmarshal(raw, &wrapper) == nil && wrapper.Value != "" {
		text = wrapper.Value
	} else {
		var direct string
		if json.Unmarshal(raw, &direct) == nil {
			text = direct
		}
	}
	var status struct {
		ID string `json:"id"`
	}
	_ = json.Unmarshal([]byte(text), &status)
	return text, status.ID
}
