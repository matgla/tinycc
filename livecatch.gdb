# Live catch of the PCH heap corruptor via DWT data watchpoints (FPB is dead;
# tcc code is XIP-flash so no software bp).  Chain: tcc_state write (tcc_new) ->
# capture s1 -> watch load_index building auto_pch_entries -> dump entries+pool.
set pagination off
set confirm off
set height 0
set width 0

python
import subprocess
_sender_src = r'''
import sys, time
import serial, serial.tools.list_ports
dev = ""
for p in serial.tools.list_ports.comports(include_links=False):
    dev = p.device; break
print("[LC] sender using " + str(dev), file=sys.stderr)
s = serial.Serial(dev, 921600, timeout=1)
buf = b""; t0 = time.time()
while time.time() - t0 < 50:
    c = s.read(1)
    if c:
        buf += c
        if buf.endswith(b"$ "):
            break
time.sleep(0.4)
print("[LC] sending tcc command", file=sys.stderr)
s.write(b"tcc /root/ci/sources/tests2/00_assignment.c -o /tmp/x\n")
s.flush(); time.sleep(3); s.close()
'''
subprocess.Popen(["python3", "-c", _sender_src])
print("[LC] serial sender launched")
end

printf "[LC] arming DWT watch on tcc_state...\n"
watch -location tcc_state
printf "[LC] continuing (board boots, serial triggers tcc)...\n"
continue
printf "[LC] caught tcc_state write in frame:\n"
frame 0
printf "[LC] s1 = "
print s1
# Capture addresses while s1 is in scope (avoid frame-scope loss after continue).
set $S1 = s1
set $eptr = (unsigned long)&s1->auto_pch_entries
set $nbptr = (unsigned long)&s1->nb_auto_pch_entries
printf "[LC] s1=0x%x  &auto_pch_entries=0x%x  &nb=0x%x\n", (unsigned long)$S1, $eptr, $nbptr
delete

printf "[LC] arming DWT watch on the auto_pch_entries pointer field...\n"
watch -location *(unsigned long *)$eptr
set $j = 0
while $j < 6
  continue
  set $arr = *(unsigned long *)$eptr
  set $nb  = *(int *)$nbptr
  printf "[LC]   realloc#%d entries=0x%x nb=%d e0.pn=0x%x pc=%p\n", $j, $arr, $nb, *(unsigned long*)($arr+4), $pc
  set $j = $j + 1
  if $nb >= 2
    loop_break
  end
end
delete

# entry[0].pn is still CORRECT right after realloc#1 (nb=2) and becomes corrupt by
# realloc#2 -> watch it NOW (in the nb=2 array) and catch the wild store.
set $arr = *(unsigned long *)$eptr
set $pn0 = $arr + 4
printf "[LC] nb=2 entries=0x%x  watching entry[0].pn @0x%x (cur=0x%x)\n", $arr, $pn0, *(unsigned long*)$pn0
watch -location *(unsigned long *)$pn0
printf "[LC] continuing to catch the wild store...\n"
continue
printf "\n[LC] ===== WILD STORE CAUGHT =====\n"
printf "[LC] entry[0].pn @0x%x now = 0x%x\n", $pn0, *(unsigned long*)$pn0
info registers pc lr r0 r1 r2 r3
printf "[LC] frame:\n"
frame 0
printf "[LC] disas around pc:\n"
x/6i $pc-8
quit
