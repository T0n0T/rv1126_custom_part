// Package gbxml owns GB/T 28181 XML messages: parsing inbound queries and
// building the response bodies the device must send back to the platform.
package gbxml

import (
	"encoding/xml"
	"fmt"
	"time"
)

const (
	manufacturer = "custom"
	model        = "RV1126B"
	firmware     = "0.1.0"
	owner        = "owner"
	civilCode    = "350200"
)

// Query is the common envelope of a platform -> device MESSAGE body.
type Query struct {
	CmdType  string `xml:"CmdType"`
	SN       string `xml:"SN"`
	DeviceID string `xml:"DeviceID"`
}

func ParseQuery(body []byte) (Query, error) {
	var q Query
	if err := xml.Unmarshal(body, &q); err != nil {
		return q, fmt.Errorf("parse gb xml: %w", err)
	}
	return q, nil
}

// Channel is the device-side description of one video channel.
type Channel struct {
	ID      string
	Name    string
	PTZType int
	Status  string // "ON" or "OFF"
}

// CatalogResponse builds the <Response> MESSAGE body for a Catalog query.
func CatalogResponse(sn, deviceID string, channels []Channel) string {
	items := ""
	for _, ch := range channels {
		status := ch.Status
		if status == "" {
			status = "ON"
		}
		items += fmt.Sprintf(`<Item>
<DeviceID>%s</DeviceID>
<Name>%s</Name>
<Manufacturer>%s</Manufacturer>
<Model>%s</Model>
<Owner>%s</Owner>
<CivilCode>%s</CivilCode>
<Block></Block>
<Address>rv1126b</Address>
<Parental>0</Parental>
<ParentID>%s</ParentID>
<SafetyWay>0</SafetyWay>
<RegisterWay>1</RegisterWay>
<CertNum></CertNum>
<Certifiable>0</Certifiable>
<ErrCode>0</ErrCode>
<EndTime></EndTime>
<Secrecy>0</Secrecy>
<IPAddress></IPAddress>
<Port>0</Port>
<Password></Password>
<Status>%s</Status>
<Longitude>0</Longitude>
<Latitude>0</Latitude>
<Info><PTZType>%d</PTZType></Info>
</Item>
`, ch.ID, ch.Name, manufacturer, model, owner, civilCode, deviceID, status, ch.PTZType)
	}

	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<Response>
<CmdType>Catalog</CmdType>
<SN>%s</SN>
<DeviceID>%s</DeviceID>
<SumNum>%d</SumNum>
<DeviceList Num="%d">
%s</DeviceList>
</Response>`, sn, deviceID, len(channels), len(channels), items)
}

// DeviceInfoResponse builds the <Response> MESSAGE body for a DeviceInfo query.
func DeviceInfoResponse(sn, deviceID string) string {
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<Response>
<CmdType>DeviceInfo</CmdType>
<SN>%s</SN>
<DeviceID>%s</DeviceID>
<DeviceName>rv1126b-ipc</DeviceName>
<Manufacturer>%s</Manufacturer>
<Model>%s</Model>
<Firmware>%s</Firmware>
<Result>OK</Result>
</Response>`, sn, deviceID, manufacturer, model, firmware)
}

// Keepalive builds the periodic heartbeat MESSAGE body. WVP dispatches
// MESSAGEs by root element, so the root must be <Notify> (CmdType Keepalive),
// not <Keepalive>.
func Keepalive(deviceID string) string {
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<Notify>
<CmdType>Keepalive</CmdType>
<SN>%06d</SN>
<DeviceID>%s</DeviceID>
<Status>OK</Status>
</Notify>`, time.Now().UnixNano()%1000000, deviceID)
}
