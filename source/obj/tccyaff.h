#pragma once

#include <stdint.h>

/* On-disk format version, written to YaffHeader.yaff_version.
 *
 * Bumped whenever the layout changes in a way an older loader cannot read.
 * A loader must refuse a version it does not know rather than guess.
 *
 *   1 -- initial layout.
 *   2 -- architecture section (YaffArchSection) between the module name and
 *        the imported-library table; YaffHeader.arch carries a real YaffArch
 *        instead of a constant 1.
 *   3 -- the table offsets and the relocation/symbol counts are 32-bit. They
 *        were 16-bit, which silently capped everything ahead of the code at
 *        64 KiB: a module with more than ~8k relocations wrapped text_offset
 *        and the loader read its "code" out of the middle of the relocation
 *        table. Nothing reported it -- the link succeeded and the image was
 *        garbage. Found on a 5 MB module (the Zig compiler) whose pre-text
 *        tables came to 105,440 bytes and whose text_offset read back 39,904.
 *   4 -- data_alignment and data_alignment_offset: where the per-process data
 *        region has to start. The loader put it wherever its allocator
 *        returned, so an object aligned past that -- `static int buf[4]
 *        __attribute__((aligned(64)))` -- was aligned only against a base the
 *        linker assumed and nothing reproduced. */
#define YAFF_VERSION 4

typedef struct {
  uint32_t nbucket;
  uint32_t nchain;
  uint32_t *bucket;
  uint32_t *chain;
} YaffHashTable;

void tcc_free_hash_table(YaffHashTable *ht);

/* Scope-owned hash table; must be zero-initialized at declaration. */
#define scoped_yaff_hash_table YaffHashTable __attribute__((cleanup(tcc_free_hash_table)))

/* What kind of module this is, written to YaffHeader.module_type. */
typedef enum YaffModuleType {
  YAFF_MODULE_TYPE_UNKNOWN = 0,
  YAFF_MODULE_TYPE_EXECUTABLE = 1,
  YAFF_MODULE_TYPE_SHARED_LIBRARY = 2,
} YaffModuleType;

/* Instruction set the image was generated for, written to YaffHeader.arch.
 * A loader refuses an image whose architecture it cannot execute.
 *
 * Values are permanent: a finer distinction (say ARMv8-M baseline vs mainline,
 * which today share YAFF_ARCH_ARMV8_M because every yasos target is mainline)
 * appends a new value rather than reinterpreting an old one. */
typedef enum YaffArch {
  YAFF_ARCH_UNKNOWN = 0,
  YAFF_ARCH_ARMV6_M = 1, /* Cortex-M0/M0+/M1 */
  YAFF_ARCH_ARMV7_M = 2, /* Cortex-M3 -- reserved, no toolchain target yet */
  YAFF_ARCH_ARMV7E_M = 3, /* Cortex-M4/M7 -- reserved, no toolchain target yet */
  YAFF_ARCH_ARMV8_M = 4,  /* Cortex-M23/M33/M55 */
} YaffArch;

/* The floating point unit the code was generated for. Names the intent (it is
 * what -mfpu selected); what a loader actually has to check is the feature
 * bitmask below, which stays decidable as the FPU list grows. */
typedef enum YaffFpu {
  YAFF_FPU_NONE = 0, /* software floating point only, no FP instructions */
  YAFF_FPU_FPV4_SP_D16 = 1,
  YAFF_FPU_FPV5_SP_D16 = 2,
  YAFF_FPU_FPV5_D16 = 3,
  YAFF_FPU_RP2350 = 4, /* FPv5-SP plus the RP2350 DCP double coprocessor */
  YAFF_FPU_VFP = 5,
  YAFF_FPU_VFPV3 = 6,
  YAFF_FPU_VFPV4 = 7,
  YAFF_FPU_NEON = 8,
  YAFF_FPU_NEON_VFPV4 = 9,
  YAFF_FPU_NEON_FP_ARMV8 = 10,
} YaffFpu;

/* How floating point arguments and results are passed. Images that disagree
 * cannot call each other, so a loader refuses an image whose ABI is not the
 * one the rest of the system was built with. */
typedef enum YaffFloatAbi {
  YAFF_FLOAT_ABI_SOFT = 0,   /* no FP instructions at all, FP args in GPRs */
  YAFF_FLOAT_ABI_SOFTFP = 1, /* FP instructions allowed, FP args in GPRs */
  YAFF_FLOAT_ABI_HARD = 2,   /* FP args and results in FP registers */
} YaffFloatAbi;

/* Hardware the image needs to be present and enabled to execute correctly.
 * This is the machine-checkable half of the architecture section: a loader
 * rejects the image when required_features names anything the part does not
 * provide, without having to know every FPU name.
 *
 * The bits describe the whole module, including the FP runtime it links
 * against -- e.g. an image built with -mfpu=rp2350 requires the DCP even if
 * no DCP sequence was inlined into its own code, because librp2350fp runs
 * them. Each module (executable or shared library) declares its own. */
typedef enum YaffArchFeature {
  YAFF_ARCH_FEATURE_FPU_SP = 1u << 0, /* single-precision VFP instructions */
  YAFF_ARCH_FEATURE_FPU_DP = 1u << 1, /* double-precision VFP instructions */
  YAFF_ARCH_FEATURE_DCP = 1u << 2,    /* RP2350 double coprocessor on CP4 */
} YaffArchFeature;

/* Architecture section, at YaffHeader.arch_section_offset (never 0 from
 * YAFF_VERSION 2 on -- offset 0 lands inside the header, so it doubles as the
 * "absent" sentinel).
 *
 * `size` makes the section forward compatible: a newer writer appends fields
 * and grows it, an older loader reads the prefix it understands and ignores
 * the rest. A loader must reject a section shorter than the prefix it needs. */
typedef struct __attribute__((packed)) YaffArchSection {
  uint16_t size;     /* bytes in this section, including this field */
  uint8_t arch;      /* YaffArch; must equal YaffHeader.arch */
  uint8_t fpu;       /* YaffFpu the code was generated for */
  uint8_t float_abi; /* YaffFloatAbi */
  uint8_t reserved_[3];
  uint32_t required_features; /* bitmask of YaffArchFeature */
} YaffArchSection;

typedef struct __attribute__((packed)) YaffHeader {
  uint8_t magic[4];
  uint8_t module_type;
  uint16_t arch;
  uint8_t yaff_version;
  uint32_t code_length;
  uint32_t init_length;
  uint32_t data_length;
  uint32_t bss_length;
  uint32_t entry;
  uint16_t external_libraries_amount;
  uint8_t alignment;
  uint8_t text_and_data_separation;
  uint16_t version_major;
  uint16_t version_minor;
  /* 32-bit since YAFF_VERSION 3: a multi-megabyte module has far more than
   * 65,535 data relocations, and the old 16-bit counts wrapped in silence. */
  uint32_t symbol_table_relocations_amount;
  uint32_t local_relocations_amount;
  uint32_t data_relocations_amount;
  uint32_t copy_relocations_amount;
  uint32_t exported_symbols_amount;
  uint32_t imported_symbols_amount;
  uint32_t got_length;
  uint32_t got_plt_length;
  uint32_t plt_length;
  /* Offset of the YaffArchSection, which sits between the module name and the
   * imported-library table. Never 0 from YAFF_VERSION 2 on. */
  uint32_t arch_section_offset;
  uint32_t imported_libraries_offset;
  uint32_t relocations_offset;
  uint32_t imported_symbols_offset;
  uint32_t exported_symbols_offset;
  /* Where the code starts in the file. This is the field the 16-bit layout
   * broke on first: it sits behind every table above, so it is the largest
   * offset in the header and the first one to pass 64 KiB. */
  uint32_t text_offset;
  uint32_t imported_symbols_lookup_offset;
  uint32_t exported_symbols_lookup_offset;
  uint32_t imported_symbols_hash_table_offset;
  uint32_t exported_symbols_hash_table_offset;
  /* Per-image stack/heap profile in bytes. 0xFFFFFFFF = use the OS default
   * (kernel-driven stack size; heap free to grow in the shared paged pool).
   * A concrete value lets a program declare its footprint (e.g. shell applets
   * want far less stack than the 32 KiB default that tcc needs) so the kernel
   * can bound the process to fixed limits — the basis for MPU-guarded,
   * profile-limited processes. */
  uint32_t stack_size;
  uint32_t heap_size;
  /* RELRO: size in bytes of the pure-const .rodata sub-region that lives in the
   * SHARED/XIP image (after plt) instead of the per-process writable data
   * segment. 0 = no shared rodata (all rodata stays per-process, legacy
   * behaviour). When >0, the loader maps it once (XIP, ref-counted) and code
   * reaches it via the rodata anchor GOT slot + R_ARM_RODATA_OFF offsets. */
  uint32_t const_rodata_length;
  /* Placement of the per-process data region ([data][bss][got], and .rodata
   * too when it is not shared): the loader starts it at an address A with
   * A % data_alignment == data_alignment_offset.  data_alignment is the
   * largest alignment any section in the region asked for (a power of two);
   * the offset is the region's linked start modulo it, so every object keeps
   * the alignment it had at link time. */
  uint32_t data_alignment;
  uint32_t data_alignment_offset;
} YaffHeader;

typedef enum YaffSectionCode {
  YAFF_SECTION_CODE = 0,
  YAFF_SECTION_DATA = 1,
  YAFF_SECTION_INIT = 2,
  YAFF_SECTION_UNKNOWN = 3,
  YAFF_SECTION_BSS = 4,    /* matches loader Section.Bss (writer maps bss->data) */
  YAFF_SECTION_RODATA = 5, /* RELRO: shared XIP .rodata. Needs the 3-bit field. */
} YaffSectionCode;

typedef struct __attribute__((packed)) YaffSymbolTableRelocationEntry {
  uint32_t is_exported_symbol : 1;
  uint32_t index : 31;
  uint32_t function_pointer : 1;
  uint32_t plt_call : 1;
  uint32_t symbol_index : 30;
} YaffSymbolTableRelocationEntry;

typedef struct __attribute__((packed)) YaffDataRelocationEntry {
  uint32_t to;
  uint32_t section : 2;
  uint32_t from : 30;
} YaffDataRelocationEntry;

typedef struct __attribute__((packed)) YaffLocalRelocationEntry {
  uint32_t section : 2;
  uint32_t index : 30;
  uint32_t target_offset;
} YaffLocalRelocationEntry;

typedef struct __attribute__((packed)) YaffCopyRelocationEntry {
  uint32_t bss_offset;
  uint32_t symbol_index;
  uint32_t size;
} YaffCopyRelocationEntry;

typedef struct __attribute__((packed)) YaffLookupEntry {
  uint16_t symbol_offset;
} YaffLookupEntry;

typedef struct __attribute__((packed)) YaffSymbolEntry {
  uint32_t section : 2;
  uint32_t weak : 1;
  uint32_t offset : 29;
  char name[0];
} YaffSymbolEntry;