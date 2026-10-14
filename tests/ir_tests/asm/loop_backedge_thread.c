/* ra:jt_backward shapes: every `continue` below used to reach the loop head
 * through a one-instruction `b head` trampoline (`bcc L ... L: b head`).
 * After post-regalloc threading the conditional branches go to the head
 * directly and no unconditional branch jumps backwards. */

struct bt_tok { const unsigned char *buf; int idx; };

int bt_ident(struct bt_tok *t)
{
  for (;;)
  {
    t->idx++;
    unsigned char c = t->buf[t->idx];
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      continue;
    return t->idx;
  }
}
