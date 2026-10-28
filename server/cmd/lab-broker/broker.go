package main

// A deliberately small MQTT 3.1.1/5 broker for the recorder lab. It supports
// CONNECT, SUBSCRIBE/UNSUBSCRIBE, PUBLISH (QoS 0/1/2), PING, and DISCONNECT.
// Unlike some general-purpose brokers it does not reserve the "$sys" prefix,
// because the OneNET-compatible mapping uses "$sys/<product>/<device>/...".
//
// It is not a production broker: no persistence, no retained messages, no
// will messages, and no authentication beyond "anonymous".

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net"
	"strings"
	"sync"
	"sync/atomic"
)

const (
	pktConnect     = 1
	pktConnack     = 2
	pktPublish     = 3
	pktPuback      = 4
	pktPubrec      = 5
	pktPubrel      = 6
	pktPubcomp     = 7
	pktSubscribe   = 8
	pktSuback      = 9
	pktUnsubscribe = 10
	pktUnsuback    = 11
	pktPingreq     = 12
	pktPingresp    = 13
	pktDisconnect  = 14
)

type broker struct {
	mu      sync.RWMutex
	clients map[string]*client
	subs    map[string]map[*client]byte
	debug   bool
}

type client struct {
	b      *broker
	conn   net.Conn
	id     string
	level  byte
	wmu    sync.Mutex
	closed atomic.Bool
	nextID uint16
}

func newBroker(debug bool) *broker {
	return &broker{clients: map[string]*client{}, subs: map[string]map[*client]byte{}, debug: debug}
}

func (b *broker) serve(ln net.Listener) error {
	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		go b.handle(conn)
	}
}

func (b *broker) logf(format string, args ...any) {
	if b.debug {
		log.Printf(format, args...)
	}
}

func (b *broker) handle(conn net.Conn) {
	c := &client{b: b, conn: conn, nextID: 1}
	defer func() {
		c.close()
		b.remove(c)
	}()
	r := bufio.NewReaderSize(conn, 64*1024)
	for {
		header, payload, err := readPacket(r)
		if err != nil {
			return
		}
		if err := b.dispatch(c, header, payload); err != nil {
			b.logf("mqtt client %s: %v", c.id, err)
			return
		}
		if c.closed.Load() {
			return
		}
	}
}

func (b *broker) dispatch(c *client, header byte, payload []byte) error {
	switch header >> 4 {
	case pktConnect:
		return b.handleConnect(c, payload)
	case pktSubscribe:
		return b.handleSubscribe(c, header, payload)
	case pktUnsubscribe:
		return b.handleUnsubscribe(c, payload)
	case pktPublish:
		return b.handlePublish(c, header, payload)
	case pktPingreq:
		return c.write(pktPingresp<<4, nil)
	case pktDisconnect:
		c.close()
		return nil
	case pktPubrel:
		if len(payload) >= 2 {
			return c.write(pktPubcomp<<4, payload[:2])
		}
		return nil
	case pktPuback, pktPubrec, pktPubcomp:
		return nil
	default:
		return fmt.Errorf("unsupported packet type %d", header>>4)
	}
}

func (b *broker) handleConnect(c *client, payload []byte) error {
	b.logf("CONNECT payload: % x", payload)
	proto, rest, ok := takeString(payload)
	if !ok {
		return fmt.Errorf("bad connect")
	}
	if len(rest) < 4 {
		return fmt.Errorf("bad connect flags")
	}
	c.level = rest[0]
	rest = rest[4:] // level + connect flags + keepalive
	if c.level == 5 {
		props, nrest, ok := takeVarint(rest)
		if !ok || props > len(nrest) {
			return fmt.Errorf("bad connect properties")
		}
		rest = nrest[props:]
	}
	id, _, ok := takeString(rest)
	if !ok {
		return fmt.Errorf("bad client id")
	}
	if id == "" {
		id = fmt.Sprintf("anon-%p", c)
	}
	c.id = id
	b.logf("mqtt CONNECT id=%s proto=%s level=%d", id, proto, c.level)

	b.mu.Lock()
	if old := b.clients[id]; old != nil && old != c {
		old.close()
	}
	b.clients[id] = c
	b.mu.Unlock()

	// CONNACK: session-present=0, reason=0, (MQTT 5) properties length=0.
	body := []byte{0, 0}
	if c.level == 5 {
		body = []byte{0, 0, 0}
	}
	return c.write(pktConnack<<4, body)
}

func (b *broker) handleSubscribe(c *client, header byte, payload []byte) error {
	if len(payload) < 2 {
		return fmt.Errorf("bad subscribe")
	}
	pid := binary.BigEndian.Uint16(payload[:2])
	rest := payload[2:]
	if c.level == 5 {
		n, nrest, ok := takeVarint(rest)
		if !ok || n > len(nrest) {
			return fmt.Errorf("bad subscribe properties")
		}
		rest = nrest[n:]
	}
	var granted []byte
	for len(rest) > 0 {
		filter, nrest, ok := takeString(rest)
		if !ok || len(nrest) < 1 {
			return fmt.Errorf("bad subscribe filter")
		}
		qos := nrest[0] & 0x03
		if qos > 1 {
			qos = 1
		}
		rest = nrest[1:]
		granted = append(granted, qos)
		b.mu.Lock()
		if b.subs[filter] == nil {
			b.subs[filter] = map[*client]byte{}
		}
		b.subs[filter][c] = qos
		b.mu.Unlock()
		b.logf("mqtt SUBSCRIBE id=%s topic=%s qos=%d", c.id, filter, qos)
	}
	body := []byte{byte(pid >> 8), byte(pid)}
	if c.level == 5 {
		body = append(body, 0) // properties length
	}
	body = append(body, granted...)
	return c.write(pktSuback<<4, body)
}

func (b *broker) handleUnsubscribe(c *client, payload []byte) error {
	if len(payload) < 2 {
		return fmt.Errorf("bad unsubscribe")
	}
	pid := binary.BigEndian.Uint16(payload[:2])
	rest := payload[2:]
	if c.level == 5 {
		n, nrest, ok := takeVarint(rest)
		if !ok || n > len(nrest) {
			return fmt.Errorf("bad unsubscribe properties")
		}
		rest = nrest[n:]
	}
	count := 0
	for len(rest) > 0 {
		filter, nrest, ok := takeString(rest)
		if !ok {
			return fmt.Errorf("bad unsubscribe filter")
		}
		rest = nrest
		count++
		b.mu.Lock()
		if m := b.subs[filter]; m != nil {
			delete(m, c)
			if len(m) == 0 {
				delete(b.subs, filter)
			}
		}
		b.mu.Unlock()
	}
	body := []byte{byte(pid >> 8), byte(pid)}
	if c.level == 5 {
		body = append(body, 0)
		for i := 0; i < count; i++ {
			body = append(body, 0) // success reason code
		}
	}
	return c.write(pktUnsuback<<4, body)
}

func (b *broker) handlePublish(c *client, header byte, payload []byte) error {
	qos := (header >> 1) & 0x03
	topic, rest, ok := takeString(payload)
	if !ok {
		return fmt.Errorf("bad publish topic")
	}
	var pid uint16
	if qos > 0 {
		if len(rest) < 2 {
			return fmt.Errorf("bad publish packet id")
		}
		pid = binary.BigEndian.Uint16(rest[:2])
		rest = rest[2:]
	}
	if c.level == 5 {
		n, nrest, ok := takeVarint(rest)
		if !ok || n > len(nrest) {
			return fmt.Errorf("bad publish properties")
		}
		rest = nrest[n:]
	}
	if strings.HasPrefix(topic, "$SYS") {
		// Only block the true reserved namespace; $sys is application data in
		// this lab mapping.
		return nil
	}
	b.route(topic, rest)
	if qos == 1 {
		return c.write(pktPuback<<4, []byte{byte(pid >> 8), byte(pid)})
	}
	if qos == 2 {
		return c.write(pktPubrec<<4, []byte{byte(pid >> 8), byte(pid)})
	}
	return nil
}

func (b *broker) route(topic string, payload []byte) {
	b.mu.RLock()
	type target struct {
		c   *client
		qos byte
	}
	var targets []target
	for filter, subs := range b.subs {
		if !topicMatches(filter, topic) {
			continue
		}
		for c, qos := range subs {
			targets = append(targets, target{c: c, qos: qos})
		}
	}
	b.mu.RUnlock()
	for _, t := range targets {
		t.c.publish(topic, payload, t.qos)
	}
}

func (c *client) publish(topic string, payload []byte, qos byte) {
	body := make([]byte, 0, 2+len(topic)+2+len(payload))
	body = append(body, byte(len(topic)>>8), byte(len(topic)))
	body = append(body, topic...)
	header := byte(pktPublish<<4) | (qos << 1)
	if qos > 0 {
		pid := c.allocPacketID()
		body = append(body, byte(pid>>8), byte(pid))
	}
	if c.level == 5 {
		body = append(body, 0) // properties length 0
	}
	body = append(body, payload...)
	_ = c.write(header, body)
}

func (c *client) allocPacketID() uint16 {
	if c.nextID == 0 {
		c.nextID = 1
	}
	id := c.nextID
	c.nextID++
	return id
}

func (c *client) write(header byte, body []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.closed.Load() {
		return fmt.Errorf("closed")
	}
	buf := make([]byte, 0, 5+len(body))
	buf = append(buf, header)
	buf = append(buf, encodeRemaining(len(body))...)
	buf = append(buf, body...)
	_, err := c.conn.Write(buf)
	if err != nil {
		c.close()
	}
	return err
}

func (c *client) close() {
	if c.closed.Swap(true) {
		return
	}
	_ = c.conn.Close()
}

func (b *broker) remove(c *client) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.clients[c.id] == c {
		delete(b.clients, c.id)
	}
	for filter, subs := range b.subs {
		delete(subs, c)
		if len(subs) == 0 {
			delete(b.subs, filter)
		}
	}
	b.logf("mqtt DISCONNECT id=%s", c.id)
}

func readPacket(r *bufio.Reader) (byte, []byte, error) {
	header, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	length, err := readRemaining(r)
	if err != nil {
		return 0, nil, err
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	return header, payload, nil
}

func readRemaining(r *bufio.Reader) (int, error) {
	mult := 1
	value := 0
	for i := 0; i < 4; i++ {
		b, err := r.ReadByte()
		if err != nil {
			return 0, err
		}
		value += int(b&0x7F) * mult
		if b&0x80 == 0 {
			return value, nil
		}
		mult *= 128
	}
	return 0, fmt.Errorf("malformed remaining length")
}

func encodeRemaining(n int) []byte {
	var out []byte
	for {
		d := byte(n % 128)
		n /= 128
		if n > 0 {
			d |= 0x80
		}
		out = append(out, d)
		if n == 0 {
			return out
		}
	}
}

func takeString(p []byte) (string, []byte, bool) {
	if len(p) < 2 {
		return "", p, false
	}
	n := int(binary.BigEndian.Uint16(p[:2]))
	if len(p) < 2+n {
		return "", p, false
	}
	return string(p[2 : 2+n]), p[2+n:], true
}

func takeVarint(p []byte) (int, []byte, bool) {
	mult := 1
	value := 0
	for i := 0; i < 4 && i < len(p); i++ {
		b := p[i]
		value += int(b&0x7F) * mult
		if b&0x80 == 0 {
			return value, p[i+1:], true
		}
		mult *= 128
	}
	return 0, p, false
}

func topicMatches(filter, topic string) bool {
	if filter == topic {
		return true
	}
	fp := strings.Split(filter, "/")
	tp := strings.Split(topic, "/")
	if len(tp) > 0 && strings.HasPrefix(tp[0], "$") && (fp[0] == "#" || fp[0] == "+") {
		return false
	}
	for i, f := range fp {
		if f == "#" {
			return true
		}
		if i >= len(tp) {
			return false
		}
		if f == "+" {
			continue
		}
		if f != tp[i] {
			return false
		}
	}
	return len(fp) == len(tp)
}
