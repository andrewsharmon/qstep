# Build the unoq_spi HAL module against an installed linuxcnc-uspace-dev.
#   make            -> unoq_spi.so
#   make install    -> copies it into LinuxCNC's module directory
obj-m := unoq_spi.o
include /usr/share/linuxcnc/Makefile.modinc
EXTRA_CFLAGS += -I$(CURDIR)/../firmware/common -Wall -Wextra -Wno-unused-parameter
