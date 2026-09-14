# Publisher integration for an unchanged Make project. Invoke this file as a
# separate makefile; the SDK runs the project's own Makefile once per target.
# Selected architectures are captured; foreign generators require explicitly
# provisioned QEMU/binfmt execution. The coordinator never installs handlers.
# Required: SELA_SOURCE_DIR, SELA_NATIVE_OUTPUT, SELA_ARTIFACT.
# Optional: SELA_BUILD_TOOL, SELA_ARCHS (CSV), SELA_BUILD_TARGETS,
# SELA_CONFIGURE_ARGS, SELA_CFLAGS. Unset SELA_ARCHS means all architectures.
SELA_BUILD_TOOL ?= sela-build
SELA_BUILD_TARGETS ?=
ifneq ($(origin SELA_TARGETS),undefined)
$(error SELA_TARGETS was removed; use SELA_BUILD_TARGETS for Make goals)
endif
ifneq ($(origin SELA_ARCHS),undefined)
export SELA_ARCHS
endif
SELA_CONFIGURE_ARGS ?=
SELA_CFLAGS ?=
sela_quote = '$(subst ','"'"',$(1))'

.PHONY: sela
.DEFAULT_GOAL := sela
sela:
	@test -n $(call sela_quote,$(SELA_SOURCE_DIR)) || { echo 'Set SELA_SOURCE_DIR' >&2; exit 1; }
	@test -n $(call sela_quote,$(SELA_NATIVE_OUTPUT)) || { echo 'Set SELA_NATIVE_OUTPUT' >&2; exit 1; }
	@test -n $(call sela_quote,$(SELA_ARTIFACT)) || { echo 'Set SELA_ARTIFACT' >&2; exit 1; }
	$(call sela_quote,$(SELA_BUILD_TOOL)) --system make \
	  --source $(call sela_quote,$(SELA_SOURCE_DIR)) \
	  --output $(call sela_quote,$(SELA_NATIVE_OUTPUT)) \
	  --artifact $(call sela_quote,$(SELA_ARTIFACT)) \
	  $(foreach target,$(SELA_BUILD_TARGETS),--build-target $(call sela_quote,$(target))) \
	  $(foreach arg,$(SELA_CONFIGURE_ARGS),--configure-arg $(call sela_quote,$(arg))) \
	  $(foreach flag,$(SELA_CFLAGS),--cflag $(call sela_quote,$(flag)))
