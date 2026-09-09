package gbxml

import (
	"encoding/xml"
	"fmt"
	"time"
)

// AlarmInput contains only standard Alarm fields plus a compact description.
// Frame IDs, PTS values and event sequences belong in the description; they
// never replace the independent GB28181 SN.
type AlarmInput struct {
	SN            uint64
	DeviceID      string
	Priority      int
	Method        int
	EventTimeUS   int64
	AlarmType     int
	Description   string
	AlarmTypeInfo string
}

type alarmNotify struct {
	XMLName          xml.Name `xml:"Notify"`
	CmdType          string   `xml:"CmdType"`
	SN               uint64   `xml:"SN"`
	DeviceID         string   `xml:"DeviceID"`
	AlarmPriority    int      `xml:"AlarmPriority"`
	AlarmMethod      int      `xml:"AlarmMethod"`
	AlarmTime        string   `xml:"AlarmTime"`
	AlarmDescription string   `xml:"AlarmDescription"`
	AlarmType        int      `xml:"AlarmType"`
	AlarmTypeParam   string   `xml:"AlarmTypeParam,omitempty"`
}

// Alarm builds a standard GB28181 Alarm MESSAGE body. An unavailable event
// clock stays empty instead of being replaced with send time.
func Alarm(input AlarmInput) string {
	message := alarmNotify{
		CmdType:          "Alarm",
		SN:               input.SN,
		DeviceID:         input.DeviceID,
		AlarmPriority:    input.Priority,
		AlarmMethod:      input.Method,
		AlarmTime:        formatAlarmTime(input.EventTimeUS),
		AlarmDescription: input.Description,
		AlarmType:        input.AlarmType,
		AlarmTypeParam:   input.AlarmTypeInfo,
	}
	data, err := xml.Marshal(message)
	if err != nil {
		return fmt.Sprintf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Notify><CmdType>Alarm</CmdType><SN>%d</SN><DeviceID></DeviceID><AlarmPriority>0</AlarmPriority><AlarmMethod>0</AlarmMethod><AlarmTime></AlarmTime><AlarmDescription></AlarmDescription><AlarmType>0</AlarmType></Notify>", input.SN)
	}
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + string(data)
}

func formatAlarmTime(eventTimeUS int64) string {
	if eventTimeUS <= 0 {
		return ""
	}
	return time.UnixMicro(eventTimeUS).Local().Format("2006-01-02T15:04:05")
}

// DefaultAlarmTypes returns the people-flow mapping from the specification.
// Callers may override individual entries for a target platform.
func DefaultAlarmTypes() map[string]int {
	return map[string]int{
		"line_cross": 5,
		"intrusion":  6,
		"occupancy":  9,
	}
}
