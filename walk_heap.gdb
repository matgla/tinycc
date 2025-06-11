# Post-mortem heap walk: find the small-pool freelist node whose block_size was
# corrupted by the wild store (that's what free()->freelist_insert coalescing
# trips on -> HardFault).  PSRAM survives `reset halt`, so the crashed heap is
# still readable here.
set pagination off
set confirm off
set height 0
set width 0

define walk_pool
  set $p = $arg0
  printf "== pool %p: refs=%d size=0x%x freelist=%p ==\n", $p, $p->refs, $p->size, $p->freelist
  set $n = (struct free_node *)$p->freelist
  set $i = 0
  while $n != 0 && $i < 4000
    set $bs = ((struct free_node *)$n)->block_size
    set $nx = ((struct free_node *)$n)->next
    if $bs <= 0 || $bs > 0x8000 || ((unsigned)(unsigned long)$n & 7) != 0
      printf "  node[%d] @%p bs=0x%x next=%p   <<<<< CORRUPT\n", $i, $n, $bs, $nx
    else
      printf "  node[%d] @%p bs=0x%x next=%p\n", $i, $n, $bs, $nx
    end
    set $n = $nx
    set $i = $i + 1
  end
  printf "  (walked %d nodes)\n", $i
end

printf "\n[WALK] ===== small-heap freelist walk =====\n"
printf "[WALK] pool=%p pool1=%p old_pools=%p\n", pool, pool1, old_pools
if pool != 0
  walk_pool pool
end
if pool1 != 0
  walk_pool pool1
end
set $op = old_pools
set $k = 0
while $op != 0 && $k < 32
  walk_pool $op
  set $op = $op->next_pool
  set $k = $k + 1
end
printf "[WALK] ===== done =====\n"
quit
