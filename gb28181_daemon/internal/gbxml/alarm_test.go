package gbxml

import (
	"encoding/xml"
	"strings"
	"testing"
	"time"
)

func TestAlarmBuildsStandardEscapedEnvelope(t *testing.T) {
	body := Alarm(AlarmInput{
		SN:            17,
		DeviceID:      "35020000001310000001",
		Priority:      4,
		Method:        5,
		EventTimeUS:   1760000000000000,
		AlarmType:     9,
		Description:   `event_id=e&1;rule_id=<people>`,
		AlarmTypeInfo: "occupancy",
	})
	var envelope struct {
		CmdType          string `xml:"CmdType"`
		SN               uint64 `xml:"SN"`
		DeviceID         string `xml:"DeviceID"`
		AlarmPriority    int    `xml:"AlarmPriority"`
		AlarmMethod      int    `xml:"AlarmMethod"`
		AlarmTime        string `xml:"AlarmTime"`
		AlarmDescription string `xml:"AlarmDescription"`
		AlarmType        int    `xml:"AlarmType"`
	}
	if err := xml.Unmarshal([]byte(body), &envelope); err != nil {
		t.Fatal(err)
	}
	if envelope.CmdType != "Alarm" || envelope.SN != 17 ||
		envelope.DeviceID != "35020000001310000001" ||
		envelope.AlarmPriority != 4 || envelope.AlarmMethod != 5 ||
		envelope.AlarmType != 9 {
		t.Fatalf("unexpected alarm envelope: %+v", envelope)
	}
	if envelope.AlarmDescription != `event_id=e&1;rule_id=<people>` {
		t.Fatalf("description was not escaped round-trip: %q", envelope.AlarmDescription)
	}
	wantTime := time.UnixMicro(1760000000000000).Local().Format("2006-01-02T15:04:05")
	if envelope.AlarmTime != wantTime {
		t.Fatalf("alarm time = %q, want %q", envelope.AlarmTime, wantTime)
	}
	if strings.Contains(body, "base64") || strings.Contains(body, "jpeg") {
		t.Fatal("alarm envelope unexpectedly contains evidence payload")
	}
}

func TestAlarmTypeMapping(t *testing.T) {
	mapping := DefaultAlarmTypes()
	for eventType, want := range map[string]int{
		"line_cross": 5,
		"intrusion":  6,
		"occupancy":  9,
	} {
		if got := mapping[eventType]; got != want {
			t.Fatalf("default alarm mapping for %q = %d, want %d", eventType, got, want)
		}
	}
}
