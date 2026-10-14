#Makefile at top of application tree
TOP = .
include $(TOP)/configure/CONFIG
DIRS := $(DIRS) $(filter-out $(DIRS), configure)
DIRS := $(DIRS) $(filter-out $(DIRS), $(wildcard *App))
DIRS := $(DIRS) $(filter-out $(DIRS), $(wildcard iocBoot))
DIRS += testIoc

elderberryMCApp_DEPEND_DIRS += configure
testIoc_DEPEND_DIRS += elderberryMCApp

include $(TOP)/configure/RULES_TOP

.PHONY: test

test:
	make -C elderberryMCApp/src/ runtests

.PHONY: testIoc-distclean
distclean: testIoc-distclean
testIoc-distclean: testIoc.realclean
	$(MAKE) -C testIoc distclean

# Code quality checks for the Linux library and embedded IOC (no build needed).
CPPCHECK ?= cppcheck
CLANG_TIDY ?= clang-tidy
CHECK_STD ?= c++14
CHECK_SOURCES = $(wildcard elderberryMCApp/src/*.cpp \
    elderberryMCApp/src/backend/*.cpp \
    elderberryMCApp/src/accessControl/*.cpp \
    testIoc/elderberryMCTestApp/src/*.cpp)
CHECK_CPPFLAGS = -D_GNU_SOURCE -D_DEFAULT_SOURCE -DUNIX -Dlinux \
    -IelderberryMCApp/src -IelderberryMCApp/src/backend \
    -IelderberryMCApp/src/accessControl
CHECK_EPICS_DIRS = $(EPICS_BASE)/include $(EPICS_BASE)/include/os/Linux \
    $(EPICS_BASE)/include/compiler/gcc
CPPCHECK_FLAGS ?= --enable=warning,style,performance,portability \
    --inline-suppr --suppress=missingIncludeSystem --error-exitcode=1
CLANG_TIDY_FLAGS ?= --checks='-*,clang-analyzer-*,bugprone-*,performance-*,portability-*' \
    --warnings-as-errors='*' --header-filter='(^|/)(elderberryMCApp|testIoc)/'

.PHONY: cppcheck clang-tidy checkall
cppcheck:
	@command -v "$(CPPCHECK)" >/dev/null 2>&1 || { echo "Install cppcheck or set CPPCHECK=/path/to/cppcheck"; exit 1; }
	$(CPPCHECK) $(CPPCHECK_FLAGS) --std=$(CHECK_STD) --language=c++ \
	    --suppress='*:$(EPICS_BASE)/include/*' $(CHECK_CPPFLAGS) \
	    $(addprefix -I,$(CHECK_EPICS_DIRS)) $(CHECK_SOURCES)

clang-tidy:
	@command -v "$(CLANG_TIDY)" >/dev/null 2>&1 || { echo "Install clang-tidy or set CLANG_TIDY=/path/to/clang-tidy"; exit 1; }
	$(CLANG_TIDY) $(CLANG_TIDY_FLAGS) $(CHECK_SOURCES) -- -std=$(CHECK_STD) \
	    $(CHECK_CPPFLAGS) $(foreach dir,$(CHECK_EPICS_DIRS),-isystem $(dir))

checkall:
	$(MAKE) --keep-going cppcheck clang-tidy


cscope:
	find . -name '*.[ch]' -o -name '*.cpp' | grep -v '/O\.' | grep -v '/include/' > cscope.files
	find -L $(EPICS_BASE) -name '*.[ch]' -o -name '*.cpp' | grep -v '/O\.' | grep -v '/include/' >> cscope.files
	cscope -Rb 
