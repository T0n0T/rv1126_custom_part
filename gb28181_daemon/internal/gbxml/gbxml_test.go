package gbxml

import (
	"strings"
	"testing"
)

func TestParseQuery(t *testing.T) {
	body := []byte(`<?xml version="1.0" encoding="UTF-8"?>
<Query><CmdType>Catalog</CmdType><SN>123456</SN><DeviceID>35020000001320000001</DeviceID></Query>`)
	q, err := ParseQuery(body)
	if err != nil {
		t.Fatalf("ParseQuery: %v", err)
	}
	if q.CmdType != "Catalog" || q.SN != "123456" || q.DeviceID != "35020000001320000001" {
		t.Fatalf("unexpected query: %+v", q)
	}
}

func TestCatalogResponse(t *testing.T) {
	out := CatalogResponse("42", "35020000001320000001", []Channel{
		{ID: "35020000001310000001", Name: "cam1", PTZType: 0},
	})
	for _, want := range []string{"<CmdType>Catalog</CmdType>", "<SN>42</SN>", "<SumNum>1</SumNum>",
		"<DeviceID>35020000001310000001</DeviceID>", "<PTZType>0</PTZType>"} {
		if !strings.Contains(out, want) {
			t.Fatalf("CatalogResponse missing %q in:\n%s", want, out)
		}
	}
}

func TestKeepalive(t *testing.T) {
	out := Keepalive("35020000001320000001")
	if !strings.Contains(out, "<Notify>") ||
		!strings.Contains(out, "<CmdType>Keepalive</CmdType>") ||
		!strings.Contains(out, "<DeviceID>35020000001320000001</DeviceID>") ||
		!strings.Contains(out, "<Status>OK</Status>") {
		t.Fatalf("unexpected keepalive body: %s", out)
	}
}

func TestParseControl(t *testing.T) {
	body := []byte(`<?xml version="1.0" encoding="UTF-8"?>
<Control>
<CmdType>DeviceControl</CmdType>
<SN>11</SN>
<DeviceID>35020000001320000001</DeviceID>
<PTZCmd>A50F008C01000041</PTZCmd>
</Control>`)
	c, err := ParseControl(body)
	if err != nil {
		t.Fatalf("ParseControl: %v", err)
	}
	if c.CmdType != "DeviceControl" || c.SN != "11" ||
		c.DeviceID != "35020000001320000001" || c.PTZCmd != "A50F008C01000041" {
		t.Fatalf("unexpected control: %+v", c)
	}
}

func TestDecodePTZAux(t *testing.T) {
	cases := []struct {
		ptz    string
		num    int
		action string
		ok     bool
	}{
		{"A50F008C01000041", 1, "ON", true},
		{"A50F008D02000043", 2, "OFF", true},
		{"A50F008C00000040", 0, "ON", true},
		{"A50F4D1000001021", 0, "", false}, // PTZ zoom, not an aux switch
		{"not-hex", 0, "", false},
		{"A50F008C", 0, "", false}, // truncated
	}
	for _, tc := range cases {
		num, action, ok := DecodePTZAux(tc.ptz)
		if num != tc.num || action != tc.action || ok != tc.ok {
			t.Fatalf("DecodePTZAux(%q) = (%d, %q, %v), want (%d, %q, %v)",
				tc.ptz, num, action, ok, tc.num, tc.action, tc.ok)
		}
	}
}

func TestControlResponse(t *testing.T) {
	out := ControlResponse("11", "35020000001320000001", "OK")
	for _, want := range []string{"<CmdType>DeviceControl</CmdType>",
		"<SN>11</SN>", "<DeviceID>35020000001320000001</DeviceID>", "<Result>OK</Result>"} {
		if !strings.Contains(out, want) {
			t.Fatalf("ControlResponse missing %q in:\n%s", want, out)
		}
	}
}
