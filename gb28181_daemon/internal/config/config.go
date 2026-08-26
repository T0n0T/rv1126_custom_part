package config

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"time"
)

// Config is the top-level daemon configuration. Unknown JSON fields are
// ignored so the file can grow without breaking older binaries.
type Config struct {
	SIP      SIPConfig    `json:"sip"`
	Media    MediaConfig  `json:"media"`
	Stream   StreamConfig `json:"stream"`
	Channels []Channel    `json:"channels"`
	Log      LogConfig    `json:"log"`
}

type SIPConfig struct {
	DeviceID          string `json:"device_id"`
	LocalIP           string `json:"local_ip"`
	ServerAddr        string `json:"server_addr"`
	PlatformID        string `json:"platform_id"`
	PlatformDomain    string `json:"platform_domain"`
	Password          string `json:"password"`
	Expires           int    `json:"expires"`
	KeepaliveInterval int    `json:"keepalive_interval"`
	RegisterRefresh   int    `json:"register_refresh"`
}

type MediaConfig struct {
	Mode      string `json:"mode"` // "none" | "rpc"
	Socket    string `json:"socket"`
	TimeoutMS int    `json:"timeout_ms"`
	Simulate  bool   `json:"simulate"`
}

type StreamConfig struct {
	Codec       string `json:"codec"`
	PayloadType int    `json:"payload_type"`
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	FPS         int    `json:"fps"`
	Bitrate     int    `json:"bitrate"`
}

type Channel struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	PTZType int    `json:"ptz_type"`
	Status  string `json:"status"`
}

type LogConfig struct {
	Level string `json:"level"`
}

// Load reads, merges with defaults and validates the config file.
func Load(path string) (*Config, error) {
	cfg := defaults()
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read config %s: %w", path, err)
	}
	if err := json.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config %s: %w", path, err)
	}
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	return cfg, nil
}

func defaults() *Config {
	return &Config{
		SIP: SIPConfig{
			Expires:           3600,
			KeepaliveInterval: 60,
			RegisterRefresh:   300,
		},
		Media: MediaConfig{
			Mode:      "none",
			Socket:    "/var/run/gb28181_media.sock",
			TimeoutMS: 3000,
		},
		Stream: StreamConfig{
			Codec:       "h264",
			PayloadType: 98,
			Width:       3840,
			Height:      2160,
			FPS:         30,
			Bitrate:     8192,
		},
		Log: LogConfig{Level: "info"},
	}
}

func (c *Config) MediaTimeout() time.Duration {
	if c.Media.TimeoutMS <= 0 {
		return 3 * time.Second
	}
	return time.Duration(c.Media.TimeoutMS) * time.Millisecond
}

func (c *Config) HasChannel(id string) bool {
	return c.Channel(id) != nil
}

func (c *Config) Channel(id string) *Channel {
	if id == "" {
		return nil
	}
	for _, ch := range c.Channels {
		if ch.ID == id {
			ch := ch
			return &ch
		}
	}
	return nil
}

func (c *Config) validate() error {
	if len(c.SIP.DeviceID) != 20 {
		return errors.New("sip.device_id must be 20 digits")
	}
	if len(c.SIP.PlatformID) != 20 {
		return errors.New("sip.platform_id must be 20 digits")
	}
	if c.SIP.PlatformDomain == "" {
		return errors.New("sip.platform_domain is required")
	}
	if net.ParseIP(c.SIP.LocalIP) == nil {
		return fmt.Errorf("sip.local_ip is not a valid IP: %q", c.SIP.LocalIP)
	}
	if _, _, err := net.SplitHostPort(c.SIP.ServerAddr); err != nil {
		return fmt.Errorf("sip.server_addr must be host:port: %w", err)
	}
	if c.SIP.Password == "" {
		return errors.New("sip.password is required")
	}
	switch c.Media.Mode {
	case "none", "rpc":
	default:
		return fmt.Errorf("media.mode must be \"none\" or \"rpc\", got %q", c.Media.Mode)
	}
	for _, ch := range c.Channels {
		if len(ch.ID) != 20 {
			return fmt.Errorf("channel id must be 20 digits, got %q", ch.ID)
		}
	}
	return nil
}
