import path from "node:path";
import { rm } from "node:fs/promises";
import type { SpecModel } from "../models.js";
import { toJsonStable, writeTextFile } from "../util.js";

interface Vector {
  name: string;
  payloadType: string;
  encoding: string;
  hexFile: string;
  expectDecode?: Record<string, unknown>;
  expectError?: string;
}

export async function emitTestVectors(_spec: SpecModel, outDir: string): Promise<void> {
  await emitTestVectorFiles(_spec, path.join(outDir, "test_vectors"));
}

export async function emitTestVectorFiles(_spec: SpecModel, dir: string): Promise<void> {
  await rm(dir, { recursive: true, force: true });
  const vectors: Vector[] = [
    { name: "control_open", payloadType: "CONTROL", encoding: "tlv", hexFile: "control_open.hex", expectDecode: { opcode: "OPEN" } },
    { name: "rpc_audio_get_algorithm_config_request", payloadType: "RPC", encoding: "tlv", hexFile: "rpc_audio_get_algorithm_config.hex", expectDecode: { method: "audio.getAlgorithmConfig" } },
    { name: "rpc_audio_set_algorithm_config_request", payloadType: "RPC", encoding: "tlv", hexFile: "rpc_audio_set_algorithm_config.hex", expectDecode: { method: "audio.setAlgorithmConfig", field: "noiseSuppression.level" } },
    { name: "event_audio_algorithm_config_changed", payloadType: "RPC", encoding: "tlv", hexFile: "event_audio_algorithm_config_changed.hex", expectDecode: { event: "audio.algorithmConfigChanged", field: "noiseSuppression.level" } },
    { name: "stream_object_chunk", payloadType: "STREAM", encoding: "binary", hexFile: "stream_object_chunk.hex", expectDecode: { streamId: 9 } },
    { name: "compact_crc8_error", payloadType: "RPC", encoding: "tlv", hexFile: "compact_crc8_error.hex", expectError: "FRAME_CRC_ERROR" },
    { name: "compact_message_id_overflow", payloadType: "RPC", encoding: "tlv", hexFile: "compact_message_id_overflow.hex", expectError: "COMPACT_MESSAGE_ID_OVERFLOW" }
  ];

  const hexData: Record<string, string> = {
    "control_open.hex": "4158010100050110000100010100010000",
    "rpc_audio_get_algorithm_config.hex": "41580102000B0110000200010207000000010901000000",
    "rpc_audio_set_algorithm_config.hex": "41580102000E0110000300010207000000010902000001010103",
    "event_audio_algorithm_config_changed.hex": "41580102000E0110000400010206000000000901000001010101",
    "stream_object_chunk.hex": "41580103001401100005000100000009000000010000000000000001AABBCCDD",
    "compact_crc8_error.hex": "121101020701000000020500000101015000",
    "compact_message_id_overflow.hex": "1211FF01020701000000020500000101015000"
  };

  await Promise.all([
    writeTextFile(path.join(dir, "manifest.json"), toJsonStable({ vectors })),
    ...Object.entries(hexData).map(([file, content]) => writeTextFile(path.join(dir, file), content))
  ]);
}
