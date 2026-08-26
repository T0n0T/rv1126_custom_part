package sipua

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/emiago/sipgo/sip"
)

// parseInvite extracts what the device needs from the platform's INVITE SDP:
// the RTP destination (c= / m= lines) and the session SSRC (y=, decimal).
func parseInvite(req *sip.Request) (InviteInfo, error) {
	var info InviteInfo
	info.ChannelID = channelIDFromSubject(req)

	body := strings.ReplaceAll(string(req.Body()), "\r\n", "\n")
	for _, line := range strings.Split(body, "\n") {
		line = strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(line, "c=IN IP4 "):
			info.DestIP = strings.TrimPrefix(line, "c=IN IP4 ")
		case strings.HasPrefix(line, "m=video "):
			fields := strings.Fields(strings.TrimPrefix(line, "m=video "))
			if len(fields) > 0 {
				p, err := strconv.Atoi(fields[0])
				if err != nil {
					return info, fmt.Errorf("bad media port %q", fields[0])
				}
				info.DestPort = p
			}
		case strings.HasPrefix(line, "y="):
			info.SSRC = strings.TrimSpace(strings.TrimPrefix(line, "y="))
		}
	}
	if info.DestIP == "" || info.DestPort == 0 || info.SSRC == "" {
		return info, fmt.Errorf("incomplete SDP: ip=%q port=%d ssrc=%q",
			info.DestIP, info.DestPort, info.SSRC)
	}
	return info, nil
}

// channelIDFromSubject reads the GB channel id from the SIP Subject header
// ("<channelId>:<ssrc>,<platformId>:0" as sent by WVP).
func channelIDFromSubject(req *sip.Request) string {
	h := req.GetHeader("Subject")
	if h == nil {
		return ""
	}
	subject := h.Value()
	if i := strings.IndexByte(subject, ':'); i > 0 {
		return subject[:i]
	}
	return strings.TrimSpace(subject)
}

// buildAnswerSDP builds the device's 200 OK SDP. WVP only needs a valid SDP
// and reads the o= username to construct the ACK request-URI.
func buildAnswerSDP(deviceID, localIP, ssrc string) string {
	return fmt.Sprintf(
		"v=0\r\n"+
			"o=%s 0 0 IN IP4 %s\r\n"+
			"s=Play\r\n"+
			"c=IN IP4 %s\r\n"+
			"t=0 0\r\n"+
			"m=video 5060 RTP/AVP 98\r\n"+
			"a=sendonly\r\n"+
			"a=rtpmap:98 H264/90000\r\n"+
			"y=%s\r\n",
		deviceID, localIP, localIP, ssrc)
}
