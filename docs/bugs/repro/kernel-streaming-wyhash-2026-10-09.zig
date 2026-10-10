// Performance reproducer for streaming Wyhash's eight-byte local copies and assembly.
const std = @import("std");

export fn stream_update(state: *std.hash.Wyhash, bytes: [*]const u8, len: usize) void {
    state.update(bytes[0..len]);
}
