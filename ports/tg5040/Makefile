# Nimbus — tg5040 cross-compile Makefile
# Called inside Docker container via build script.

CROSS_COMPILE ?= aarch64-nextui-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
SYSROOT := /opt/aarch64-nextui-linux-gnu/aarch64-nextui-linux-gnu/libc/usr
PLATFORM_ID := tg5040

PKG_CONFIG := pkg-config
SDL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LDFLAGS := $(shell $(PKG_CONFIG) --libs sdl2) -lSDL2_ttf -lSDL2_image

# Bundled static curl + OpenSSL (built by Apostrophe build_third_party.sh)
CURL_PREFIX    := /workspace/third_party/apostrophe/build/third_party/$(PLATFORM_ID)/curl/install
OPENSSL_PREFIX := /workspace/third_party/apostrophe/build/third_party/$(PLATFORM_ID)/openssl/install
CURL_CFLAGS    := -I$(CURL_PREFIX)/include -DAP_ENABLE_CURL -ffunction-sections -fdata-sections
CURL_LDFLAGS   := -Wl,--whole-archive $(CURL_PREFIX)/lib/libcurl.a -Wl,--no-whole-archive \
	$(OPENSSL_PREFIX)/lib/libssl.a $(OPENSSL_PREFIX)/lib/libcrypto.a \
	-lz -Wl,--gc-sections

WORKSPACE  := /workspace
BUILD_DIR  ?= $(WORKSPACE)/build/tg5040
SRC        := $(WORKSPACE)/src/main.c
AP_INCLUDE := $(WORKSPACE)/third_party/apostrophe/include
CJSON_DIR  := $(WORKSPACE)/third_party/cjson
QRGEN_DIR  := $(WORKSPACE)/third_party/qrcodegen
PAKKIT_DIR := $(WORKSPACE)/third_party/pakkit/include
DEPS       := $(AP_INCLUDE)/apostrophe.h $(AP_INCLUDE)/apostrophe_widgets.h

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
	-Wno-format-truncation -Wno-stringop-truncation \
	-mcpu=cortex-a53 -mtune=cortex-a53 \
	-DPLATFORM_TG5040 \
	-I$(AP_INCLUDE) \
	-I$(CJSON_DIR) \
	-I$(QRGEN_DIR) \
	-I$(PAKKIT_DIR) \
	$(SDL_CFLAGS) $(CURL_CFLAGS)

LDFLAGS := $(SDL_LDFLAGS) $(CURL_LDFLAGS) -lpthread -ldl -lm

.PHONY: all clean

all: $(BUILD_DIR)/nimbus

$(BUILD_DIR)/nimbus: $(SRC) $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
