# Unity groups for an arch sub-library: see "Unity build" in the top-level
# Makefile.  Include after OBJS is complete and before the rule that archives
# it, with UNITY_LIB set to the library's name in source/unity/unity.mk and
# UNITY_SRC_DIR to this directory relative to $(TOP).  UNITY comes from the
# top-level make; run on its own this Makefile builds one TU per file.

UNITY ?= no
-include $(TOP)/source/unity/unity.mk
# The rules below must not become the including Makefile's default goal.
UNITY_SAVED_GOAL := $(.DEFAULT_GOAL)

UNITY_HERE = $(foreach g,$(UNITY_GROUPS_$(UNITY_LIB)),$(UNITY_MEMBERS_$g))

ifeq ($(UNITY),yes)
UNITY_STALE = $(filter-out $(addprefix $(UNITY_SRC_DIR)/,$(SRCS)),$(UNITY_HERE))
$(if $(UNITY_STALE),$(error source/unity/unity.mk lists $(UNITY_STALE) under '$(UNITY_LIB)', which no longer builds it; run scripts/gen_unity.py))
OBJS := $(filter-out $(addprefix $(BUILD_DIR)/,$(notdir $(UNITY_HERE:.c=.o))),$(OBJS)) \
	$(foreach g,$(UNITY_GROUPS_$(UNITY_LIB)),$(BUILD_DIR)/unity_$g.o)
endif

$(BUILD_DIR)/unity_%.o: $(TOP)/source/unity/%.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -o $@ -c $< $(CFLAGS) $(DEFINES) -I$(TOP) -I$(TOP)/source/ir
$(foreach g,$(UNITY_GROUPS_$(UNITY_LIB)),$(eval $(BUILD_DIR)/unity_$g.o : $(addprefix $(TOP)/,$(UNITY_MEMBERS_$g))))

# For scripts/gen_unity.py.
unity-info:
	@echo 'LIB $(UNITY_LIB) $(addprefix $(UNITY_SRC_DIR)/,$(SRCS))'
.PHONY: unity-info
.DEFAULT_GOAL := $(UNITY_SAVED_GOAL)
