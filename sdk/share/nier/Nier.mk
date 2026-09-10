# Publisher integration for an unchanged Make project. Invoke this file as a
# separate makefile; the SDK privately runs the project's own Makefile twice.
# Required: NIER_SOURCE_DIR, NIER_NATIVE_OUTPUT, NIER_ARTIFACT.
# Optional: NIER_BUILD_TOOL, NIER_TARGETS, NIER_CONFIGURE_ARGS, NIER_CFLAGS.
NIER_BUILD_TOOL ?= nier-build
NIER_TARGETS ?=
NIER_CONFIGURE_ARGS ?=
NIER_CFLAGS ?=
nier_quote = '$(subst ','"'"',$(1))'

.PHONY: nier
.DEFAULT_GOAL := nier
nier:
	@test -n $(call nier_quote,$(NIER_SOURCE_DIR)) || { echo 'Set NIER_SOURCE_DIR' >&2; exit 1; }
	@test -n $(call nier_quote,$(NIER_NATIVE_OUTPUT)) || { echo 'Set NIER_NATIVE_OUTPUT' >&2; exit 1; }
	@test -n $(call nier_quote,$(NIER_ARTIFACT)) || { echo 'Set NIER_ARTIFACT' >&2; exit 1; }
	$(call nier_quote,$(NIER_BUILD_TOOL)) --system make \
	  --source $(call nier_quote,$(NIER_SOURCE_DIR)) \
	  --output $(call nier_quote,$(NIER_NATIVE_OUTPUT)) \
	  --artifact $(call nier_quote,$(NIER_ARTIFACT)) \
	  $(foreach target,$(NIER_TARGETS),--target $(call nier_quote,$(target))) \
	  $(foreach arg,$(NIER_CONFIGURE_ARGS),--configure-arg $(call nier_quote,$(arg))) \
	  $(foreach flag,$(NIER_CFLAGS),--cflag $(call nier_quote,$(flag)))
