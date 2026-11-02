# Builds the Windows-side helpers with mingw-w64.
#   Arch: pacman -S mingw-w64-gcc   Debian/Ubuntu: apt install gcc-mingw-w64-x86-64   Fedora: dnf install mingw64-gcc
ifeq ($(origin CC),default)
CC      := x86_64-w64-mingw32-gcc
endif
CFLAGS  ?= -O2 -s -Wall -Wextra -Wno-unused-parameter
OUT     := build

all: $(OUT)/vmvad.sys $(OUT)/vmhook.dll $(OUT)/vmlaunch.exe

$(OUT):
	mkdir -p $@

# kernel-mode stub driver (runs inside Wine's winedevice.exe)
$(OUT)/vmvad.sys: src/vmvad.c | $(OUT)
	$(CC) $(CFLAGS) -ffreestanding -fno-stack-protector -nostdlib -nostartfiles -shared \
		-Wl,--subsystem,native -Wl,--entry,DriverEntry -Wl,--image-base,0x10000 \
		-o $@ $< -lntoskrnl

$(OUT)/vmhook.dll: src/vmhook.c | $(OUT)
	$(CC) $(CFLAGS) -D_WIN32_WINNT=0x0A00 -shared -static-libgcc -o $@ $< \
		-lole32 -lws2_32 -lpsapi

$(OUT)/vmlaunch.exe: src/vmlaunch.c | $(OUT)
	$(CC) $(CFLAGS) -municode -mwindows -static-libgcc -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
