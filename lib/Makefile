#
# Tiny C Compiler Makefile for libtcc1.a
#

TOP = ..
include $(TOP)/Makefile
VPATH = $(TOPSRC)/lib $(TOPSRC)/win32/lib
T = $(or $(CROSS_TARGET),$(NATIVE_TARGET),unknown)
X = $(if $(CROSS_TARGET),$(CROSS_TARGET)-)
XCFG = $(or $(findstring -win,$T),-unx)
S = $(if $(findstring yes,$(SILENT)),@$(info * $@))

$(info CROSS_TARGET='$(CROSS_TARGET)')

TCC = $(TOP)/$(X)tcc$(EXESUF)
XTCC ?= $(TOP)/$(X)tcc$(EXESUF)
FP_CC := $(abspath $(TOP)/$(X)tcc$(EXESUF))
XCC = $(XTCC)
XAR = $(XTCC) -ar
XFLAGS-unx = -B$(TOPSRC)
XFLAGS-win = -B$(TOPSRC)/win32 -I$(TOPSRC)/include
XFLAGS = $(XFLAGS$(XCFG)) $(CROSS_FLAGS)
BFLAGS = -bt

# in order to use gcc, type: make <target>-libtcc1-usegcc=yes
arm-libtcc1-usegcc ?= no

# This makes bounds checking 40%..60% faster.
#x86_64-libtcc1-usegcc=yes
#i386-libtcc1-usegcc=yes

ifeq "$($(T)-libtcc1-usegcc)" "yes"
 XCC = $(CC)
 XAR = $(AR)
 XFLAGS = $(CFLAGS) -fPIC -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable -mfloat-abi=hard -mfpu=fpv4-sp-d16
 BFLAGS = $(if $(CONFIG_dwarf),-gdwarf,-gstabs)
endif

I386_O = libtcc1.o alloca.o alloca-bt.o $(COMMON_O)
X86_64_O = libtcc1.o alloca.o alloca-bt.o $(COMMON_O)
ARM_O = libtcc1.o alloca.o armflush.o $(COMMON_O) $(ARM_FP_O)
ARM64_O = lib-arm64.o $(COMMON_O)
RISCV64_O = lib-arm64.o $(COMMON_O)
COMMON_O = stdatomic.o atomic.o builtin.o
WIN_O = crt1.o crt1w.o wincrt1.o wincrt1w.o dllcrt1.o dllmain.o
LIN_O = dsohandle.o
OSX_O =

# ARM Floating Point Library Objects
# Built from lib/fp/ directory for different FPU configurations
ARM_FP_O = armeabi.o armeabi_divmod.o

# backtrace/bcheck/run only for native compiler
Nat = $(if $X,no,)
Cbt = $(Nat)$(subst yes,,$(CONFIG_backtrace))
Cbc = $(Cbt)$(subst yes,,$(CONFIG_bcheck))

$(Nat)COMMON_O += runmain.o
$(Cbt)WIN_O += bt-dll.o
$(Cbc)COMMON_O += bcheck.o

# not in libtcc1.a
EXTRA_O = runmain.o bt-exe.o bt-dll.o bt-log.o bcheck.o

OBJ-i386 = $(I386_O) $(LIN_O)
OBJ-x86_64 = $(X86_64_O) va_list.o $(LIN_O)
OBJ-x86_64-osx = $(X86_64_O) va_list.o $(OSX_O)
OBJ-i386-win32 = $(I386_O) chkstk.o $(WIN_O)
OBJ-x86_64-win32 = $(X86_64_O) chkstk.o $(WIN_O)
OBJ-arm64 = $(ARM64_O) $(LIN_O)
OBJ-arm64-osx = $(ARM64_O) $(OSX_O)
OBJ-arm = $(ARM_O) $(LIN_O)
OBJ-arm-fpa = $(OBJ-arm)
OBJ-arm-fpa-ld = $(OBJ-arm)
OBJ-arm-vfp = $(OBJ-arm)
OBJ-arm-eabi = $(OBJ-arm)
OBJ-arm-eabihf = $(OBJ-arm)
OBJ-arm-wince = $(ARM_O) $(WIN_O)
OBJ-armv8m = alloca.o armeabi.o armeabi_divmod.o
OBJ-riscv64 = $(RISCV64_O) $(LIN_O)

OBJ-extra = $(filter $(EXTRA_O),$(OBJ-$T))
OBJ-libtcc1 = $(addprefix $(X),$(filter-out $(OBJ-extra),$(OBJ-$T)))

ALL = $(addprefix $(TOP)/,$(X)libtcc1.a $(OBJ-extra))

all: $(ALL)

$(info $(TCC))
$(TOP)/$(X)libtcc1.a : $(OBJ-libtcc1) $(TCC)
	$S$(XAR) rcs $@ $(OBJ-libtcc1)

$(X)%.o : %.c $(TCC)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

$(X)%.o : %.S $(TCC)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

$(TOP)/%.o : %.c $(TCC)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

$(TOP)/bcheck.o : XFLAGS += $(BFLAGS) -I$(TOP)
# includes tccrun.c, $(TOP) for config.h  (tccrun.c prerequisite via $(TCC))
$(TOP)/bt-exe.o : XFLAGS += -I$(TOP)

$(X)crt1w.o : crt1.c
$(X)wincrt1w.o : wincrt1.c

# Build FP libraries for ARM targets
.PHONY: fp-libs
fp-libs:
	$(MAKE) -C fp FPU=soft TARGET=$(T) CC="$(FP_CC)" AR="$(FP_CC) -ar" CFLAGS="$(XFLAGS)"
	$(MAKE) -C fp FPU=vfpv4-sp TARGET=$(T) CC="$(FP_CC)" AR="$(FP_CC) -ar" CFLAGS="$(XFLAGS)"
	$(MAKE) -C fp FPU=vfpv5-dp TARGET=$(T) CC="$(FP_CC)" AR="$(FP_CC) -ar" CFLAGS="$(XFLAGS)"
	$(MAKE) -C fp FPU=rp2350 TARGET=$(T) CC="$(FP_CC)" AR="$(FP_CC) -ar" CFLAGS="$(XFLAGS)"

# Build armeabi.o which dispatches to correct FP library
$(X)armeabi.o : armeabi.c $(TCC)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

# don't try to make it
$(TCC) : ;

clean :
	rm -f *.o $(addprefix $(TOP)/,*libtcc1.a $(EXTRA_O))
	rm -rf build
	$(MAKE) -C fp clean
