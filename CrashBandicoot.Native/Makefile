# CrashBandicoot (native armv7 port) - Theos project.
# Target: iPad mini 1 (A5, armv7, iOS 9.3.5). Built on Linux with the Theos
# iOS cross toolchain; see ../.github/workflows/native-ios.yml.
export THEOS_DEVICE_IP =
ARCHS = armv7
# Headers/libs from the iPhoneOS 9.3 SDK, Mach-O min version 7.0 so any
# 32-bit device from iOS 7 up to 9.3.5 will load it.
TARGET = iphone:clang:9.3:7.0
INSTALL_TARGET_PROCESSES = CrashBandicootARMv7

include $(THEOS)/makefiles/common.mk

APPLICATION_NAME = CrashBandicootARMv7

# gen/*.c is produced by tools/cs2c.py from the recompiler's C# output
# (never committed: it is derived from the user's own disc image).
CrashBandicootARMv7_FILES = $(wildcard src/*.c) $(wildcard gen/*.c) host/ios_host.m host/ios_availability_stub.m

# The overlay/pad code in ios_host.m is written for ARC.
ios_host.m_CFLAGS += -fobjc-arc

# -fno-modules: the 9.3 SDK's legacy module maps confuse modern clang.
# -std=gnu11: runtime uses anonymous unions/structs, statement-expr free C.
# UIAlertView is deprecated in the 9.3 SDK but works on every OS we target.
ADDITIONAL_CFLAGS += -fno-modules -fno-implicit-modules -fno-implicit-module-maps \
	-Wno-deprecated-declarations -Wno-unused-label -Wno-unused-variable -Wno-unused-function \
	-Wno-unused-parameter -Wno-error -Wno-missing-braces -Wno-format-truncation \
	-Wno-tautological-compare -Wno-implicit-fallthrough -Wno-misleading-indentation \
	-Wno-unknown-warning-option -Wno-constant-logical-operand -fno-strict-aliasing

CrashBandicootARMv7_CFLAGS = -Isrc -Igen -Ihost -std=gnu11 -DNDEBUG

CrashBandicootARMv7_LDFLAGS = \
	-framework UIKit -framework Foundation -framework QuartzCore -framework CoreGraphics \
	-framework OpenGLES -framework AudioToolbox -framework AVFoundation \
	-weak_framework GameController -Wl,-dead_strip

CrashBandicootARMv7_CODESIGN_FLAGS = -SResources/entitlements.plist
CrashBandicootARMv7_PLIST = Resources/Info.plist
CrashBandicootARMv7_RESOURCE_FILES = Resources/LaunchImage.png Resources/LaunchImage@2x.png

include $(THEOS_MAKE_PATH)/application.mk

after-install::
	install.exec "killall -9 CrashBandicootARMv7 || true"
