# If RACK_DIR is not defined when calling the Makefile, default to Rack SDK
RACK_DIR ?= ../Rack-SDK

# FLAGS will be passed to both the C and C++ compiler
# [PROD-START]
FLAGS +=
CFLAGS +=
CXXFLAGS +=
# [PROD-END]
# [DEV-START]
FLAGS += -g -O0
CFLAGS +=
CXXFLAGS += -DDEBUG
# [DEV-END]

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# [DEV-START]
# # Auto-install to VCV Rack plugins folder (DEV ONLY)
RACK_USER_DIR ?= $(HOME)/Library/Application Support/Rack2/plugins-mac-arm64
install: dist
# 	mkdir -p "$(RACK_USER_DIR)"
# 	cp dist/*.vcvplugin "$(RACK_USER_DIR)"/
# [DEV-END]
