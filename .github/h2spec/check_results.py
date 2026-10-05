#!/usr/bin/env python3
"""Fails if h2spec reports a failure that is not on the allowlist.

Allowlisted cases are nghttp2 behaviour, not Orbit's (see the reasons).
Everything else must pass, so regressions in Orbit's HTTP/2 layer fail CI.
"""
import sys
import xml.etree.ElementTree as ET

KNOWN = {
    # RFC 9113 says frames on closed streams are minimally processed and
    # discarded; h2spec expects the RFC 7540 stream error. nghttp2 follows 9113.
    ("http2/5.1", "closed: Sends a DATA frame after sending RST_STREAM frame"):
        "RFC 9113: frames on closed streams are discarded",
    ("http2/5.1", "closed: Sends a HEADERS frame after sending RST_STREAM frame"):
        "RFC 9113: frames on closed streams are discarded",
    ("http2/5.1", "closed: Sends a DATA frame"):
        "RFC 9113: frames on closed streams are discarded",
    ("http2/5.1", "closed: Sends a HEADERS frame"):
        "RFC 9113: frames on closed streams are discarded",
    # nghttp2 answers with GOAWAY, but the response to the earlier stream
    # is already on the wire, and h2spec expects the GOAWAY first.
    ("http2/5.1.1", "Sends stream identifier that is numerically smaller than previous"):
        "GOAWAY follows an already-sent response",
    # Timing-dependent: h2spec sends the second HEADERS in a separate write.
    # When the response to stream 1 is already sent, the stream is closed and
    # the frame is discarded as above; otherwise nghttp2 sends RST_STREAM.
    ("http2/5.1", "half closed (remote): Sends a HEADERS frame"):
        "races the response; closed streams discard frames (RFC 9113)",
    # Same race: DATA sent on a stream whose request has ended.
    ("http2/6.1", 'Sends a DATA frame on the stream that is not in "open" or "half-closed (local)" state'):
        "races the response; closed streams discard frames (RFC 9113)",
    # RFC 9113 deprecated the RFC 7540 priority scheme.
    ("http2/5.3.1", "Sends PRIORITY frame that depend on itself"):
        "RFC 9113 deprecated RFC 7540 priorities",
    # nghttp2 does not reject PRIORITY on stream 0 (nghttp2/nghttp2#2840).
    ("http2/6.3", "Sends a PRIORITY frame with 0x0 stream identifier"):
        "nghttp2/nghttp2#2840",
    # nghttp2 treats a stream window overflow as a connection error
    # (connection closed) where h2spec expects RST_STREAM.
    ("http2/6.9.1", "Sends multiple WINDOW_UPDATE frames increasing the flow control window to above 2^31-1 on a stream"):
        "nghttp2 closes the connection instead of resetting the stream",
}

root = ET.parse(sys.argv[1]).getroot()
passed = 0
known = []
unexpected = []
for suite in root.iter("testsuite"):
    package = suite.get("package") or suite.get("name")
    for case in suite.iter("testcase"):
        failed = case.find("failure") is not None or case.find("error") is not None
        if not failed:
            passed += 1
            continue
        key = (package, case.get("classname"))
        (known if key in KNOWN else unexpected).append(key)

print(f"{passed} passed, {len(known)} known nghttp2 deviations, {len(unexpected)} unexpected failures")
for key in known:
    print(f"  known: {key[0]} {key[1]} ({KNOWN[key]})")
if unexpected:
    print("Unexpected failures:")
    for key in unexpected:
        print(f"  {key[0]} {key[1]}")
    sys.exit(1)
