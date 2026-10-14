TOP = ..
include $(TOP)/configure/CONFIG

CHECK_RELEASE = YES
TARGETS = $(CONFIG_TARGETS)
CONFIGS += $(subst ../,,$(wildcard $(CONFIG_INSTALLS)))

include $(TOP)/configure/RULES
