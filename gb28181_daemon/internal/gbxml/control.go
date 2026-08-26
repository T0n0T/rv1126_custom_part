package gbxml

import (
	"encoding/hex"
	"encoding/xml"
	"fmt"
)

// Control is the platform -> device <Control> MESSAGE body defined in
// GB/T 28181-2016 A.2.3. Only the fields needed for device-side IO control
// are modelled; the rest of the sub-commands arrive as empty strings.
type Control struct {
	XMLName   xml.Name `xml:"Control"`
	CmdType   string   `xml:"CmdType"`
	SN        string   `xml:"SN"`
	DeviceID  string   `xml:"DeviceID"`
	PTZCmd    string   `xml:"PTZCmd"`
	TeleBoot  string   `xml:"TeleBoot"`
	RecordCmd string   `xml:"RecordCmd"`
	GuardCmd  string   `xml:"GuardCmd"`
	AlarmCmd  string   `xml:"AlarmCmd"`
	IFrameCmd string   `xml:"IFrameCmd"`
}

// ParseControl parses a platform -> device <Control> MESSAGE body.
func ParseControl(body []byte) (Control, error) {
	var c Control
	if err := xml.Unmarshal(body, &c); err != nil {
		return c, fmt.Errorf("parse gb control xml: %w", err)
	}
	return c, nil
}

// DecodePTZAux extracts the auxiliary-switch action from a PTZCmd control
// code (GB/T 28181-2016 A.3.7): byte 4 is 8CH (switch on) or 8DH (switch
// off), byte 5 is the switch number (00H~FFH). It returns ok=false when the
// code is malformed or is not an auxiliary-switch command.
func DecodePTZAux(ptz string) (num int, action string, ok bool) {
	raw, err := hex.DecodeString(ptz)
	if err != nil || len(raw) != 8 {
		return 0, "", false
	}
	switch raw[3] {
	case 0x8C:
		return int(raw[4]), "ON", true
	case 0x8D:
		return int(raw[4]), "OFF", true
	}
	return 0, "", false
}

// ControlResponse builds the <Response> body for control commands that need
// an explicit result (RecordCmd/GuardCmd/AlarmCmd/HomePosition/DeviceConfig),
// per GB/T 28181-2016 A.2.6.
func ControlResponse(sn, deviceID, result string) string {
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<Response>
<CmdType>DeviceControl</CmdType>
<SN>%s</SN>
<DeviceID>%s</DeviceID>
<Result>%s</Result>
</Response>`, sn, deviceID, result)
}
