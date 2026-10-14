TOP = ../..
include $(TOP)/configure/CONFIG

ifeq ($(OS_CLASS),Linux)
PROD_IOC += elderberryMCTestIoc

DBD += elderberryMCTestIoc.dbd
elderberryMCTestIoc_DBD += base.dbd
elderberryMCTestIoc_DBD += elderberryMC.dbd

elderberryMCTestIoc_SRCS += elderberryMCTestIoc_registerRecordDeviceDriver.cpp
elderberryMCTestIoc_SRCS += elderberryMCTestMain.cpp

elderberryMCTestIoc_LIBS += elderberryMC
elderberryMCTestIoc_LIBS += $(EPICS_BASE_IOC_LIBS)
endif

include $(TOP)/configure/RULES
