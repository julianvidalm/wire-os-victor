package main

import (
	"bytes"
	"testing"
)

type capturingConn struct{ written [][]byte }

func (c *capturingConn) Read() []byte      { return nil }
func (c *capturingConn) ReadBlock() []byte { return nil }
func (c *capturingConn) Close() error      { return nil }
func (c *capturingConn) Write(b []byte) (int, error) {
	c.written = append(c.written, append([]byte(nil), b...))
	return len(b), nil
}

func TestSendFakeTriggerWordWritesTheFramedGameToEngineMessage(t *testing.T) {
	conn := &capturingConn{}
	m := &EngineCladIpcManager{}
	m.conn = conn
	if err := m.SendFakeTriggerWord(); err != nil {
		t.Fatal(err)
	}
	// u16 little-endian size (1: the tag of a message without fields), then the tag.
	want := []byte{0x01, 0x00, 126}
	if len(conn.written) != 1 || !bytes.Equal(conn.written[0], want) {
		t.Fatalf("wrote %v, want %v", conn.written, want)
	}
}

func TestSendFakeTriggerWordWithoutEngineConnectionFails(t *testing.T) {
	if err := (&EngineCladIpcManager{}).SendFakeTriggerWord(); err == nil {
		t.Fatal("no error without an engine connection")
	}
}
