# Hot code in RAM (`--hot-text`)

On the RP2350 a program runs from flash behind a 16 KB cache, and a miss costs about
122 cycles against 2.2 per instruction. At -O0 the compiler runs about 300 KB of code,
so it spends more time waiting for instructions than executing them.

`-Wl,--hot-text=FILE` names functions the loader runs from a copy in SRAM. `FILE` has
one function name per line; `#` starts a comment; a name that does not exist is ignored.
A name also matches the functions it names once Zig's numbers are dropped (`fn__1234`,
`fn__func_12__345` -> `fn`): the C backend renumbers them on every change of the source,
so the device zig's list (`apps/zig/hot_functions.txt` in yasos) names them without.
A function moves only if it has a section of its own (`-ffunction-sections`).

## Link

The listed functions become the last block of `.text`. Within the block and within the
rest, a `BL` reaches as usual. From one to the other the distance is known only at load
time, so such a call goes through a 20-byte veneer on the caller's side:

    ldr.w ip, [r9, #slot]    ; the base, Thumb bit included
    addw  ip, ip, #bits 0-11
    add.w ip, ip, #bits 12-19
    add.w ip, ip, #bits 20-27
    bx    ip

There are two reserved GOT slots: index 4 holds flash `.text`, index 5 where the block
runs. A call from the block to an import does what the PLT stub does, without fetching
it from flash. Veneers use only IP. There is no slot per function and no thunk.

Any other PC-relative relocation across the block's edge is a link error.

## Image

`YaffHotTextArchSection` extends the architecture section through its `size` field with
the block's offset and length in the code section. Other images keep the short section
and stay byte-identical.

Shared libraries take a list too (libc: `libs/libc/hot_functions.txt`), and the kernel its
own through `--ram-text=FILE`, which moves listed functions to `.time_critical` for the
linker script to run from RAM; names leave out the number Zig's C backend appends.

## Load

`module.zig` copies the block once per image, shared by every process using it, and
`address_in_section` sends addresses in the block to the copy: GOT entries, thunks and
data relocations follow without knowing about it. Relocated-image templates have a base
of their own for it (`template.self_hot`).

The flash copy stays valid code. A loader that ignores the section, or finds no memory,
runs the block from flash.

The block takes fast memory before the compiler's IR arena, which gets what is left.

## The list

`source/driver/hot_functions.txt` comes from a profile of the self-host on the board
model, on an image without a list:

    scripts/selfhost_profile.py run IMAGE OUT --nest 2
    scripts/selfhost_profile.py hotlist IMAGE OUT --kb N > source/driver/hot_functions.txt

A name with a leading `=` stays in the block, but a caller outside it calls the flash copy
directly, with no veneer. For tiny accessors called constantly from flash, the veneer
costs more than the fetch: the nine marked here are -1.3% at -O2.

It picks by fetch stall per byte. `TCC_HOT_TEXT_FILE` makes `build_rootfs.sh` use another
list; empty means none.
