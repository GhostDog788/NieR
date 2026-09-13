ifndef SELA_ROOT
$(error Set SELA_ROOT to the absolute path of your Sela checkout)
endif

SELA_BUILD_TOOL ?= $(SELA_ROOT)/build/prealpha/sela-build
SELA_SOURCE_DIR := $(CURDIR)
SELA_TARGETS := hello
SELA_NATIVE_OUTPUT := build/native-make/hello
SELA_ARTIFACT := $(CURDIR)/build/sela-make/hello.sela

include $(SELA_ROOT)/sdk/share/sela/Sela.mk

.PHONY: publication-directory
sela: | publication-directory
publication-directory:
	mkdir -p "$(CURDIR)/build/sela-make"
