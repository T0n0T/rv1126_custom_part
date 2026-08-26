package sipua

import (
	"strings"
	"testing"

	"github.com/emiago/sipgo/sip"
)

func TestParseInvite(t *testing.T) {
	recipient := sip.Uri{}
	if err := sip.ParseUri("sip:35020000001310000001@192.168.1.88:5060", &recipient); err != nil {
		t.Fatal(err)
	}
	req := sip.NewRequest(sip.INVITE, recipient)
	req.AppendHeader(sip.NewHeader("Subject", "35020000001310000001:0200004568,35020000002000000001:0"))
	req.SetBody([]byte(`v=0
o=35020000001320000001 0 0 IN IP4 192.168.1.88
s=Play
c=IN IP4 192.168.1.88
t=0 0
m=video 10003 RTP/AVP 96 97 98 99
a=recvonly
a=rtpmap:96 PS/90000
y=0200004568
`))

	info, err := parseInvite(req)
	if err != nil {
		t.Fatalf("parseInvite: %v", err)
	}
	if info.ChannelID != "35020000001310000001" {
		t.Fatalf("channel = %q", info.ChannelID)
	}
	if info.DestIP != "192.168.1.88" || info.DestPort != 10003 {
		t.Fatalf("dest = %s:%d", info.DestIP, info.DestPort)
	}
	if info.SSRC != "0200004568" {
		t.Fatalf("ssrc = %q", info.SSRC)
	}
}

func TestBuildAnswerSDP(t *testing.T) {
	out := buildAnswerSDP("35020000001320000001", "192.168.1.88", "0200004568")
	for _, want := range []string{"o=35020000001320000001", "m=video 5060 RTP/AVP 98",
		"a=rtpmap:98 H264/90000", "y=0200004568"} {
		if !strings.Contains(out, want) {
			t.Fatalf("answer SDP missing %q:\n%s", want, out)
		}
	}
}
