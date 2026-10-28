// lab-broker runs a small MQTT 3.1.1/5 broker for local recorder testing.
// It deliberately allows the "$sys/<product>/<device>/..." topic namespace
// used by the OneNET-compatible recorderCmd/recorderStatus mapping.
package main

import (
	"crypto/tls"
	"flag"
	"log"
	"net"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	addr := flag.String("addr", ":1883", "MQTT listen address")
	tlsCert := flag.String("tls-cert", "", "PEM server certificate (enables TLS)")
	tlsKey := flag.String("tls-key", "", "PEM server private key")
	debug := flag.Bool("debug", false, "log MQTT connect/subscribe events")
	flag.Parse()

	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	scheme := "mqtt"
	if *tlsCert != "" {
		cert, err := tls.LoadX509KeyPair(*tlsCert, *tlsKey)
		if err != nil {
			log.Fatalf("tls keypair: %v", err)
		}
		ln = tls.NewListener(ln, &tls.Config{
			Certificates: []tls.Certificate{cert},
			MinVersion:   tls.VersionTLS12,
		})
		scheme = "mqtts"
	}

	b := newBroker(*debug)
	go func() {
		if err := b.serve(ln); err != nil {
			log.Printf("broker stopped: %v", err)
		}
	}()
	log.Printf("lab %s broker listening on %s (anonymous)", scheme, *addr)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	<-sigCh
	_ = ln.Close()
}
