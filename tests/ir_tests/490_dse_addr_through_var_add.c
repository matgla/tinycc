/* dse's dead-stackloc scan follows an address taken of a frame object through
   copies and pointer arithmetic, and counts a deref through any of them as a
   read of the object.  It followed ADD/SUB only from a TEMP into a TEMP, yet
   treats every ADD of an address as safe arithmetic -- so `V <-- T ADD #8`
   (the address into a VAR) was neither followed nor counted, and the stores
   filling the object died although it was read through V.  This is Zig's
   Io.Threaded.dirCreateDirPath as the Zig C backend emits it: `t11 =
   &t4.path; t12 = *t11` read a path whose stores were gone, and the tcc -O1
   Zig compiler failed "failed to create path 'h' in local cache directory".
   The path iterator and the filesystem are simple stand-ins. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
enum { zig_error_PathAlreadyExists = 39u, zig_error_FileNotFound = 42u, zig_error_NotDir = 43u, zig_error_BadPathName = 47u };
typedef uint32_t enum__Io_File_Permissions__enum_3216_3216;
typedef uint8_t enum__Io_File_Kind_3902;
typedef uint8_t enum__Io_Dir_CreatePathStatus_3243;
struct slice_u8_49 { uint8_t const *ptr; uintptr_t len; };
struct Io_Threaded_1733;
struct errunion_Io_Dir_CreatePathStatus_3243 { enum__Io_Dir_CreatePathStatus_3243 payload; uint16_t error; };
struct Io_Dir_3194 { int32_t handle; };
struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 { struct slice_u8_49 path; uintptr_t root_len; uintptr_t root_end_index; uintptr_t start_index; uintptr_t end_index; };
struct fs_path_ComponentIterator_28_posix_2cu8_29_Component_12739 { struct slice_u8_49 name; struct slice_u8_49 path; };
struct opt_fs_path_ComponentIterator_28_posix_2cu8_29_Component_12741 { struct fs_path_ComponentIterator_28_posix_2cu8_29_Component_12739 payload; bool is_null; };
struct errunion_void_34 { uint16_t error; };
struct errunion_Io_File_Kind_3902 { enum__Io_File_Kind_3902 payload; uint16_t error; };
struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 fs_path_componentIterator__7103(struct slice_u8_49 a0);
struct opt_fs_path_ComponentIterator_28_posix_2cu8_29_Component_12741 fs_path_ComponentIterator_28_posix_2cu8_29_last__8209(struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 *a0);
struct opt_fs_path_ComponentIterator_28_posix_2cu8_29_Component_12741 fs_path_ComponentIterator_28_posix_2cu8_29_previous__8212(struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 *a0);
struct opt_fs_path_ComponentIterator_28_posix_2cu8_29_Component_12741 fs_path_ComponentIterator_28_posix_2cu8_29_next__8210(struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 *a0);
struct errunion_void_34 Io_Threaded_dirCreateDirPosix__2914(void *a0, struct Io_Dir_3194 a1, struct slice_u8_49 a2, enum__Io_File_Permissions__enum_3216_3216 a3);
struct errunion_Io_File_Kind_3902 Io_Threaded_filePathKind__2929(struct Io_Threaded_1733 *a0, struct Io_Dir_3194 a1, struct slice_u8_49 a2);
struct errunion_Io_Dir_CreatePathStatus_3243 Io_Threaded_dirCreateDirPath__2917(void *const a0, struct Io_Dir_3194 const a1, struct slice_u8_49 const a2, enum__Io_File_Permissions__enum_3216_3216 const a3);
struct errunion_Io_Dir_CreatePathStatus_3243 Io_Threaded_dirCreateDirPath__2917(void *const a0, struct Io_Dir_3194 const a1, struct slice_u8_49 const a2, enum__Io_File_Permissions__enum_3216_3216 const a3) {
 struct Io_Threaded_1733 *t0;
 struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 t2;
 struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 t1;
 struct fs_path_ComponentIterator_28_posix_2cu8_29_Component_12739 t5;
 struct fs_path_ComponentIterator_28_posix_2cu8_29_Component_12739 t8;
 struct fs_path_ComponentIterator_28_posix_2cu8_29_Component_12739 t4;
 struct opt_fs_path_ComponentIterator_28_posix_2cu8_29_Component_12741 t6;
 void *t9;
 void *t10;
 struct slice_u8_49 *t11;
 struct slice_u8_49 t12;
 struct errunion_void_34 t13;
 uint16_t t14;
 struct errunion_Io_File_Kind_3902 t15;
 uint16_t t16;
 uint16_t t17;
 struct errunion_Io_Dir_CreatePathStatus_3243 t18;
 uint16_t t20;
 bool t7;
 enum__Io_File_Kind_3902 t19;
 enum__Io_Dir_CreatePathStatus_3243 t21;
 enum__Io_Dir_CreatePathStatus_3243 t3;
 t0 = (struct Io_Threaded_1733 *)a0;
 t2 = fs_path_componentIterator__7103(a2);
 t1 = t2;
 t3 = UINT8_C(0);
 t6 = fs_path_ComponentIterator_28_posix_2cu8_29_last__8209(&t1);
 t7 = !t6.is_null;
 if (t7) {
  t8 = t6.payload;
  t5 = t8;
  goto zig_block_0;
 }
 return (struct errunion_Io_Dir_CreatePathStatus_3243){ .error = zig_error_BadPathName, .payload = UINT8_C(0x0) };

zig_block_0:;
 t4 = t5;
 zig_loop_22:
 t9 = (void *)t0;
 t10 = t9;
 t11 = (struct slice_u8_49 *)&t4.path;
 t12 = (*t11);
 t13 = Io_Threaded_dirCreateDirPosix__2914(t10, a1, t12, a3);
 t7 = t13.error == UINT16_C(0);
 if (t7) {
  t3 = UINT8_C(1);
  goto zig_block_2;
 }
 t14 = t13.error;
 switch (t14) {
  case zig_error_PathAlreadyExists: {
   t11 = (struct slice_u8_49 *)&t4.path;
   t12 = (*t11);
   t15 = Io_Threaded_filePathKind__2929(t0, a1, t12);
   if (t15.error) {
    t16 = t15.error;
    t17 = t16;
    t18.payload = UINT8_C(0x0);
    t18.error = t17;
    return t18;
   }
   t19 = t15.payload;
   t7 = t19 != UINT8_C(2);
   if (t7) {
    return (struct errunion_Io_Dir_CreatePathStatus_3243){ .error = zig_error_NotDir, .payload = UINT8_C(0x0) };
   }
   goto zig_block_3;

zig_block_3:;
   goto zig_block_2;
  }
  case zig_error_FileNotFound: {
   t6 = fs_path_ComponentIterator_28_posix_2cu8_29_previous__8212(&t1);
   t7 = !t6.is_null;
   if (t7) {
    t8 = t6.payload;
    t5 = t8;
    goto zig_block_4;
   }
   return (struct errunion_Io_Dir_CreatePathStatus_3243){ .error = zig_error_FileNotFound, .payload = UINT8_C(0x0) };

zig_block_4:;
   t4 = t5;
   goto zig_block_1;
  }
  default: {
   t20 = t14;
   t17 = t20;
   t18.payload = UINT8_C(0x0);
   t18.error = t17;
   return t18;
  }
 }

zig_block_2:;
 t6 = fs_path_ComponentIterator_28_posix_2cu8_29_next__8210(&t1);
 t7 = !t6.is_null;
 if (t7) {
  t8 = t6.payload;
  t5 = t8;
  goto zig_block_5;
 }
 t21 = t3;
 t18.payload = t21;
 t18.error = UINT16_C(0);
 return t18;

zig_block_5:;
 t4 = t5;
 goto zig_block_1;

zig_block_1:;
 goto zig_loop_22;
}
typedef struct fs_path_ComponentIterator_28_posix_2cu8_29_12730 It;
typedef struct opt_fs_path_ComponentIterator_28_posix_2cu8_29_Component_12741 Opt;
/* end_index = end of the current component; start_index = its start */
It fs_path_componentIterator__7103(struct slice_u8_49 a0) { It it = {a0, 0, 0, 0, 0}; return it; }
static Opt cur(It *it) {
  Opt o; o.is_null = false;
  o.payload.name.ptr = it->path.ptr + it->start_index; o.payload.name.len = it->end_index - it->start_index;
  o.payload.path.ptr = it->path.ptr; o.payload.path.len = it->end_index; return o;
}
Opt fs_path_ComponentIterator_28_posix_2cu8_29_last__8209(It *it) {
  if (!it->path.len) { Opt o; memset(&o, 0, sizeof o); o.is_null = true; return o; }
  it->end_index = it->path.len; uintptr_t s = it->end_index; while (s && it->path.ptr[s - 1] != '/') s--; it->start_index = s; return cur(it);
}
Opt fs_path_ComponentIterator_28_posix_2cu8_29_previous__8212(It *it) {
  if (it->start_index == 0) { Opt o; memset(&o, 0, sizeof o); o.is_null = true; return o; }
  it->end_index = it->start_index - 1; uintptr_t s = it->end_index; while (s && it->path.ptr[s - 1] != '/') s--; it->start_index = s; return cur(it);
}
Opt fs_path_ComponentIterator_28_posix_2cu8_29_next__8210(It *it) {
  if (it->end_index >= it->path.len) { Opt o; memset(&o, 0, sizeof o); o.is_null = true; return o; }
  it->start_index = it->end_index + 1; uintptr_t e = it->start_index; while (e < it->path.len && it->path.ptr[e] != '/') e++; it->end_index = e; return cur(it);
}
static char dirs[16][32]; static int ndirs;
static int exists(struct slice_u8_49 p) { for (int i = 0; i < ndirs; i++) if (strlen(dirs[i]) == p.len && !memcmp(dirs[i], p.ptr, p.len)) return 1; return 0; }
struct errunion_void_34 Io_Threaded_dirCreateDirPosix__2914(void *a0, struct Io_Dir_3194 a1, struct slice_u8_49 a2, enum__Io_File_Permissions__enum_3216_3216 a3) {
  struct errunion_void_34 r = {0};
  printf("mkdir %.*s (h=%d p=%o)\n", (int)a2.len, (const char *)a2.ptr, (int)a1.handle, (unsigned)a3);
  if (exists(a2)) { r.error = zig_error_PathAlreadyExists; return r; }
  uintptr_t s = a2.len; while (s && a2.ptr[s - 1] != '/') s--;
  if (s) { struct slice_u8_49 par = {a2.ptr, s - 1}; if (!exists(par)) { r.error = zig_error_FileNotFound; return r; } }
  memcpy(dirs[ndirs], a2.ptr, a2.len); dirs[ndirs++][a2.len] = 0; return r;
}
struct errunion_Io_File_Kind_3902 Io_Threaded_filePathKind__2929(struct Io_Threaded_1733 *a0, struct Io_Dir_3194 a1, struct slice_u8_49 a2) {
  struct errunion_Io_File_Kind_3902 r = {2, 0}; return r;
}
int main(void) {
  strcpy(dirs[ndirs++], "a");
  static const char p[] = "a/b/c";
  struct slice_u8_49 s = {(const uint8_t *)p, 5};
  struct errunion_Io_Dir_CreatePathStatus_3243 r = Io_Threaded_dirCreateDirPath__2917((void *)0x1234, (struct Io_Dir_3194){-100}, s, 0755);
  printf("-> payload=%d error=%d\n", r.payload, r.error);
  static const char q[] = "h";
  s.ptr = (const uint8_t *)q; s.len = 1;
  r = Io_Threaded_dirCreateDirPath__2917((void *)0x1234, (struct Io_Dir_3194){-100}, s, 0755);
  printf("-> payload=%d error=%d\n", r.payload, r.error);
  return 0;
}
