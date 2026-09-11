ifndef NIER_ROOT
$(error Set NIER_ROOT to the absolute path of your NieR checkout)
endif

NIER_BUILD_TOOL ?= $(NIER_ROOT)/build/prealpha/nier-build
NIER_SOURCE_DIR := $(CURDIR)
NIER_TARGETS := hello
NIER_NATIVE_OUTPUT := build/native-make/hello
NIER_ARTIFACT := $(CURDIR)/build/nier-make/hello.nier

include $(NIER_ROOT)/sdk/share/nier/Nier.mk

.PHONY: publication-directory
nier: | publication-directory
publication-directory:
	mkdir -p "$(CURDIR)/build/nier-make"
