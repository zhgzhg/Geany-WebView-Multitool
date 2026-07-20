# Patch for @xterm/xterm 6.0.0 (applied by scripts/fetch-assets.sh).
#
# MacPorts webkit2-gtk JSC (macOS, seen with 2.52.4) evaluates every number in
# (2^31, 2^32) as INT32_MIN, which turns xterm's MAX_BUFFER_SIZE=4294967295
# negative and makes `new Terminal()` throw a RangeError (blank terminal).
# INT32_MAX is an equally-unreachable upper bound for the scrollback buffer,
# and is harmless on healthy engines.
s/t\.MAX_BUFFER_SIZE=4294967295/t.MAX_BUFFER_SIZE=2147483647/
