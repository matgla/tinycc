/* slots.h -- the yasc side of slots.c (chapter 10).
 *
 * slots.c is regular C: a table of numbered "files" handed out as int
 * handles, as open() hands out file descriptors.  This header is written in
 * yasc (#pragma yasc), so the handles carry ownership: slot_open's result
 * owns a slot until slot_close takes it back, and may be invalid (< 0) when
 * the table is full.  YasOS's libc declares open, close, read and write the
 * same way (`int owned(close) maybe open(...)`). */
#pragma yasc
#ifndef SLOTS_H
#define SLOTS_H

/* takes the handle back; an invalid one (< 0) is fine, as free(NULL) is */
int slot_close(int drop ?owned(slot_close) h);
/* a handle on file number `which`, or -1 when every slot is taken */
int ?owned(slot_close) slot_open(int which);
/* reads through a valid handle: a view borrows it */
int slot_read(int view h);
/* how many handles are open: the examples print it to show none leaked */
int slot_live(void);

#endif
