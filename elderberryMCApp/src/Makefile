TOP = ../..

include $(TOP)/configure/CONFIG

# Linux GCC/Clang
USR_CXXFLAGS_Linux += -std=c++14

LIBRARY_IOC_Linux += elderberryMC
INC += emc_server.h dispatcher.h
INC += connection_filter.h
INC += access_control.h
INC += acl_session.h
INC += echo_dispatcher.h
INC += mono_sim_dispatcher.h
INC += mono_sim_model.h
DBD += elderberryMC.dbd

elderberryMC_SRCS_Linux += emc_server.cpp
elderberryMC_SRCS_Linux += emc_iocsh.cpp
elderberryMC_SRCS_Linux += emc_iocsh_acl.cpp
SRC_DIRS += $(TOP)/elderberryMCApp/src/accessControl/
elderberryMC_SRCS_Linux += connection_filter.cpp
elderberryMC_SRCS_Linux += access_control.cpp
elderberryMC_SRCS_Linux += acl_session.cpp
SRC_DIRS += $(TOP)/elderberryMCApp/src/backend/
elderberryMC_SRCS_Linux += echo_dispatcher.cpp
elderberryMC_SRCS_Linux += mono_sim_dispatcher.cpp
elderberryMC_SRCS_Linux += mono_sim_model.cpp
elderberryMC_LIBS += Com

########################################### Unit Tests
SRC_DIRS += $(TOP)/elderberryMCApp/src/test/
# Access control tests (parser, strategies, chain, loopback server): make runtests
TESTPROD_Linux += testAccessControl
testAccessControl_SRCS += testAccessControl.cpp
testAccessControl_LIBS += elderberryMC Com
TESTS += testAccessControl

# common bind configuration, address isolation and restart behaviour.
TESTPROD_Linux += testBindAddress
testBindAddress_SRCS += testBindAddress.cpp
testBindAddress_LIBS += elderberryMC Com
TESTS += testBindAddress

# Line ending tests: reply terminator (lf, crlf, cr, none) and requests ending in LF, CR or CRLF
TESTPROD_Linux += testReplyTerminator
testReplyTerminator_SRCS += testReplyTerminator.cpp
testReplyTerminator_LIBS += elderberryMC Com
TESTS += testReplyTerminator

# Request framing security: bounded lines, absolute deadlines, other clients.
TESTPROD_Linux += testRequestLimits
testRequestLimits_SRCS += testRequestLimits.cpp
testRequestLimits_LIBS += elderberryMC Com
TESTS += testRequestLimits

# reject NUL before filtering/dispatch; no setting changes or client starvation.
TESTPROD_Linux += testRequestNul
testRequestNul_SRCS += testRequestNul.cpp
testRequestNul_LIBS += elderberryMC Com
TESTS += testRequestNul

# Connection caps and configurable idle expiry.
TESTPROD_Linux += testConnectionLimits
testConnectionLimits_SRCS += testConnectionLimits.cpp
testConnectionLimits_LIBS += elderberryMC Com
TESTS += testConnectionLimits

# token bucket, outstanding quotas, closed-client work and recovery.
TESTPROD_Linux += testRequestAdmission
testRequestAdmission_SRCS += testRequestAdmission.cpp
testRequestAdmission_LIBS += elderberryMC Com
# std::thread needs an explicit dependency; Com may not supply -lpthread.
testRequestAdmission_SYS_LIBS_Linux += pthread
TESTS += testRequestAdmission

# Deterministic simulation tests: no hardware or real-time sleeps.
TESTPROD_Linux += testMonoSimModel
testMonoSimModel_SRCS += testMonoSimModel.cpp
testMonoSimModel_LIBS += elderberryMC Com
TESTS += testMonoSimModel

TESTPROD_Linux += testMonoSimDispatcher
testMonoSimDispatcher_SRCS += testMonoSimDispatcher.cpp
testMonoSimDispatcher_LIBS += elderberryMC Com
TESTS += testMonoSimDispatcher

TESTPROD_Linux += testBrokenPipe
testBrokenPipe_SRCS += testBrokenPipe.cpp
testBrokenPipe_SYS_LIBS_Linux += pthread
testBrokenPipe_LIBS += elderberryMC Com
TESTS += testBrokenPipe 

TESTSCRIPTS_HOST += $(TESTS:%=%.t)

include $(TOP)/configure/RULES
