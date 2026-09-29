package sipua

import (
	"fmt"
	"testing"

	"github.com/emiago/sipgo/sip"
)

func TestBuildFailedInviteByeKeepsDialogIdentity(t *testing.T) {
	const wire = "INVITE sip:35020000001310000001@192.168.1.63 SIP/2.0\r\n" +
		"Via: SIP/2.0/UDP 192.168.1.88:8116;branch=z9hG4bK-test\r\n" +
		"From: <sip:44030600002000000002@192.168.1.88>;tag=platform-tag\r\n" +
		"To: <sip:35020000001320000001@192.168.1.63>\r\n" +
		"Contact: <sip:44030600002000000002@192.168.1.88:8116>\r\n" +
		"Call-ID: invite-123@192.168.1.88\r\n" +
		"CSeq: 42 INVITE\r\n" +
		"Content-Length: 0\r\n\r\n"
	msg, err := sip.ParseMessage([]byte(wire))
	if err != nil {
		t.Fatal(err)
	}
	req := msg.(*sip.Request)
	req.SetSource("192.168.1.88:8116")
	res := sip.NewResponseFromRequest(req, sip.StatusOK, "OK", nil)
	bye, err := buildFailedInviteBye(req, res)
	if err != nil {
		t.Fatal(err)
	}
	if bye.Method != sip.BYE || bye.CallID().Value() != req.CallID().Value() ||
		bye.CSeq().SeqNo != 43 || bye.CSeq().MethodName != sip.BYE {
		t.Fatalf("wrong BYE identity or sequence: %s", bye.String())
	}
	if bye.From().Address.String() != res.To().Address.String() ||
		bye.To().Address.String() != req.From().Address.String() {
		t.Fatalf("BYE To/From reversed incorrectly: %s", bye.String())
	}
	if got, _ := bye.From().Params.Get("tag"); got == "" {
		t.Fatal("BYE lost local dialog tag")
	}
	if got, _ := bye.To().Params.Get("tag"); got != "platform-tag" {
		t.Fatalf("BYE lost remote dialog tag: %q", got)
	}
	if bye.Recipient.String() != req.Contact().Address.String() ||
		bye.Destination() != req.Source() {
		t.Fatalf("BYE target incorrect: %s", bye.String())
	}
}

func TestBusyErrorClassification(t *testing.T) {
	req := sip.NewRequest(sip.INVITE, sip.Uri{Host: "192.168.1.63"})
	if got := inviteFailureResponse(req, fmt.Errorf("%w: active session", ErrBusy)).StatusCode; got != sip.StatusBusyHere {
		t.Fatalf("busy INVITE response = %d, want 486", got)
	}
	if got := inviteFailureResponse(req, fmt.Errorf("engine unavailable")).StatusCode; got != sip.StatusTemporarilyUnavailable {
		t.Fatalf("unavailable INVITE response = %d, want 480", got)
	}
}
