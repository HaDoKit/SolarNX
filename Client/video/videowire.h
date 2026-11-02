#pragma once

// SolarNX's video framing - mirrors SolarNX/Capture/VideoSender.cs on the PC (its doc comment is
// the full description). Every datagram is VIDEO_WIRE_HEADER_SIZE + VIDEO_WIRE_SHARD_SIZE bytes:
//   0  u8  magic 0x53 ('S')         8  u32 frame number (starts random, +1 per frame)
//   1  u8  version (2)             12  u32 frame size in bytes (Annex B, before padding)
//   2  u8  flags (bit 0: keyframe, bit 1: H.265 - else H.264)
//   3  u8  block index
//   4  u8  block count
//   5  u8  shard index in the block (data shards first, then parity)
//   6  u8  parity shards in this block
//   7  u8  reserved
// A frame's N = ceil(size / shard size) data shards (the last zero-padded) are split into
// blockCount Reed-Solomon blocks (common/rs) as evenly as possible: block b has
// N / blockCount data shards, plus one when b < N % blockCount.

#define VIDEO_WIRE_HEADER_SIZE 16
#define VIDEO_WIRE_SHARD_SIZE  1280
#define VIDEO_WIRE_DATAGRAM_SIZE (VIDEO_WIRE_HEADER_SIZE + VIDEO_WIRE_SHARD_SIZE)
#define VIDEO_WIRE_MAGIC   0x53
#define VIDEO_WIRE_VERSION 2 // 2 since the codec flag (0.2.4)
#define VIDEO_WIRE_FLAG_KEYFRAME 0x01
#define VIDEO_WIRE_FLAG_HEVC     0x02
