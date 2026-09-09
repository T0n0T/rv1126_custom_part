package config

import "testing"

func validConfigForTest() *Config {
	cfg := defaults()
	cfg.SIP.DeviceID = "35020000001320000001"
	cfg.SIP.PlatformID = "35020000002000000001"
	cfg.SIP.PlatformDomain = "3502000000"
	cfg.SIP.LocalIP = "192.168.1.88"
	cfg.SIP.ServerAddr = "192.168.1.88:8160"
	cfg.SIP.Password = "secret"
	cfg.Media.Mode = "rpc"
	return cfg
}

func TestEventsDefaults(t *testing.T) {
	cfg := defaults()
	if cfg.Events.Enabled {
		t.Fatal("events are enabled by default")
	}
	if cfg.Events.OutboxPath == "" || cfg.Events.MaxRecords <= 0 ||
		cfg.Events.MaxBytes <= 0 || cfg.Events.AlarmPriority != 4 ||
		cfg.Events.AlarmMethod != 5 || cfg.Events.MaxAttempts != 10 ||
		cfg.Events.RetryMaxMS <= cfg.Events.RetryBaseMS ||
		cfg.Events.AlarmTypes["occupancy"] != 9 {
		t.Fatalf("unexpected event defaults: %+v", cfg.Events)
	}
}

func TestEnabledEventsValidate(t *testing.T) {
	cfg := validConfigForTest()
	cfg.Events.Enabled = true
	if err := cfg.validate(); err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name string
		edit func(*Config)
	}{
		{"missing outbox", func(c *Config) { c.Events.OutboxPath = "" }},
		{"invalid priority", func(c *Config) { c.Events.AlarmPriority = 5 }},
		{"invalid method", func(c *Config) { c.Events.AlarmMethod = 0 }},
		{"invalid attempts", func(c *Config) { c.Events.MaxAttempts = 0 }},
		{"invalid retry", func(c *Config) { c.Events.RetryBaseMS = 0 }},
		{"invalid retry max", func(c *Config) { c.Events.RetryMaxMS = 0 }},
		{"invalid max records", func(c *Config) { c.Events.MaxRecords = 0 }},
		{"invalid max bytes", func(c *Config) { c.Events.MaxBytes = 0 }},
		{"invalid alarm mapping", func(c *Config) { c.Events.AlarmTypes["occupancy"] = 0 }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			candidate := *cfg
			test.edit(&candidate)
			if err := candidate.validate(); err == nil {
				t.Fatal("validate accepted invalid events config")
			}
		})
	}
}

func TestEnabledEvidenceRequiresHTTPEndpoint(t *testing.T) {
	cfg := validConfigForTest()
	cfg.Events.Enabled = true
	cfg.Events.EvidenceEnabled = true
	cfg.Events.EvidenceDir = "/tmp/media-evidence"
	cfg.Events.EvidenceURL = "http://127.0.0.1:18080/evidence"
	cfg.Events.EvidenceToken = "secret"
	if err := cfg.validate(); err != nil {
		t.Fatal(err)
	}
	for _, endpoint := range []string{"", "/relative", "ftp://127.0.0.1/evidence"} {
		candidate := *cfg
		candidate.Events.EvidenceURL = endpoint
		if err := candidate.validate(); err == nil {
			t.Fatalf("validate accepted evidence endpoint %q", endpoint)
		}
	}
	candidate := *cfg
	candidate.Events.EvidenceToken = ""
	if err := candidate.validate(); err == nil {
		t.Fatal("validate accepted evidence without bearer token")
	}
	disabled := validConfigForTest()
	disabled.Events.EvidenceEnabled = true
	if err := disabled.validate(); err == nil {
		t.Fatal("validate accepted evidence while events are disabled")
	}
}
