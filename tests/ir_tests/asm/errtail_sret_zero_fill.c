/* Zig's `return .{ .error = X, .payload = undefined }` lowers to a zero fill
 * of the result through the sret pointer followed by the real fields, every
 * byte of the fill overwritten before anything can read it.  The fill is
 * dead: each result byte is written exactly once on the error path. */
typedef struct { unsigned long long lo, hi; } u128;
struct eu { u128 payload; unsigned short error; };

extern unsigned char step(int);

struct eu first_error(int k)
{
  if (step(k))
    return (struct eu){.error = 111, .payload = {0xaaaaaaaaaaaaaaaaull, 0x2aaaaaaaaaaull}};
  struct eu r;
  r.error = 0;
  r.payload.lo = (unsigned long long)k;
  r.payload.hi = 0;
  return r;
}
