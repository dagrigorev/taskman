# Compatibility front end. CMake is the only build definition.
# mingw32-make [all|debug|test|rebuild|run|clean] [STRICT=1] [DEBUG=1]
.DEFAULT_GOAL := all
PS = powershell -NoProfile -ExecutionPolicy Bypass
OPTIONS = -Toolchain mingw
ifeq ($(STRICT),1)
OPTIONS += -Strict
endif
ifeq ($(DEBUG),1)
OPTIONS += -Configuration Debug
endif

all:
	$(PS) -File build.ps1 build $(OPTIONS)

debug test rebuild run:
	$(PS) -File build.ps1 $@ $(OPTIONS)

clean:
	$(PS) -File build.ps1 clean

.PHONY: all debug test rebuild run clean
