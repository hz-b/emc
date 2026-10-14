TOP = .
include $(TOP)/configure/CONFIG

.DEFAULT_GOAL := all

DIRS += configure
DIRS += elderberryMCTestApp
DIRS += iocBoot

elderberryMCTestApp_DEPEND_DIRS += configure
iocBoot_DEPEND_DIRS += elderberryMCTestApp

.PHONY: run
run: install
	cd iocBoot/iocelderberryMCTest && ../../bin/$(EPICS_HOST_ARCH)/elderberryMCTestIoc st.cmd

include $(TOP)/configure/RULES_TOP
