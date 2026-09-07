package main

import "testing"

func TestRenderHelloProducesBoundedStableFrame(t *testing.T) {
	initial := renderHello(helloState{lastKey: "READY"})
	repeated := renderHello(helloState{lastKey: "READY"})
	if initial != repeated {
		t.Fatal("identical state produced a different frame")
	}
	black := 0
	for _, value := range initial {
		if value != 0 {
			black++
		}
	}
	if black < 500 {
		t.Fatalf("frame is unexpectedly sparse: %d nonzero bytes", black)
	}
}

func TestRenderHelloChangesOnInput(t *testing.T) {
	before := renderHello(helloState{lastKey: "READY"})
	after := renderHello(helloState{lastKey: "LEFT", count: 1})
	if before == after {
		t.Fatal("input state did not change the frame")
	}
}

func TestSetPixelUsesPanelStripLayout(t *testing.T) {
	var output frame
	setPixel(&output, 7, 9)
	offset := displayWidth + 7
	if output[offset] != 0x40 {
		t.Fatalf("pixel encoding = %#x, want 0x40", output[offset])
	}
}
