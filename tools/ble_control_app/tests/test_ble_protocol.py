import json
import unittest

from ble_control_app.ble_protocol import (
    FileDescriptor,
    MessageEnvelope,
    chunk_bytes,
    crc32_hex,
    decode_line,
    encode_line,
)


class BleProtocolTests(unittest.TestCase):
    def test_encode_line_appends_newline(self) -> None:
        envelope = MessageEnvelope(kind="command", action="ping", payload={"seq": 1})

        encoded = encode_line(envelope)

        self.assertTrue(encoded.endswith(b"\n"))
        decoded = json.loads(encoded.decode("utf-8").strip())
        self.assertEqual(decoded["kind"], "command")
        self.assertEqual(decoded["action"], "ping")
        self.assertEqual(decoded["payload"]["seq"], 1)

    def test_decode_line_parses_envelope(self) -> None:
        decoded = decode_line(b'{"kind":"event","action":"status","payload":{"state":"ready"}}\n')

        self.assertEqual(decoded.kind, "event")
        self.assertEqual(decoded.action, "status")
        self.assertEqual(decoded.payload["state"], "ready")

    def test_decode_line_rejects_invalid_json(self) -> None:
        with self.assertRaises(ValueError):
            decode_line(b"not-json\n")

    def test_chunk_bytes_splits_payload_to_mtu(self) -> None:
        payload = b"abcdefghij"

        chunks = list(chunk_bytes(payload, 4))

        self.assertEqual(chunks, [b"abcd", b"efgh", b"ij"])

    def test_chunk_bytes_rejects_non_positive_size(self) -> None:
        with self.assertRaises(ValueError):
            list(chunk_bytes(b"abc", 0))

    def test_crc32_hex_matches_known_value(self) -> None:
        self.assertEqual(crc32_hex(b"123456789"), "cbf43926")

    def test_file_descriptor_roundtrip(self) -> None:
        descriptor = FileDescriptor(name="logs.txt", size=128, checksum="abcd1234")

        payload = descriptor.to_payload()
        restored = FileDescriptor.from_payload(payload)

        self.assertEqual(restored.name, "logs.txt")
        self.assertEqual(restored.size, 128)
        self.assertEqual(restored.checksum, "abcd1234")


if __name__ == "__main__":
    unittest.main()
