# Copyright (c) 2026 Takayuki Goto
# SPDX-License-Identifier: Apache-2.0

# A stock SL-C3200 has no JTAG header and no in-system programmer, so there is
# no flash or debug runner: the Sharp ROM updater writes zephyr.bin to the
# kernel area of the NAND flash, with the updater.sh that CMakeLists.txt puts
# next to it. See doc/index.rst.
#
# QEMU emulates the machine, but only up to 9.1: the PXA2xx machines were
# removed in 9.2, and the Zephyr SDK ships a newer QEMU than that. Point
# QEMU_BIN_PATH at a directory holding a qemu-system-arm 9.1 or older (the
# stock Ubuntu 24.04 package is 8.2.2 and works) to use `west build -t run`.
set(SUPPORTED_EMU_PLATFORMS qemu)

set(QEMU_BINARY_SUFFIX arm)
set(QEMU_CPU_TYPE pxa270)

set(QEMU_BOARD_FLAGS
  -machine terrier
  -display none
  )

# Boot the zImage rather than the ELF, so that every QEMU run also goes through
# the path a real machine uses: QEMU loads it at SDRAM + 0x10000, not at its
# link address.
set(QEMU_KERNEL_OPTION "-kernel;@PROJECT_BINARY_DIR@/@CONFIG_KERNEL_BIN_NAME@.bin")

board_set_debugger_ifnset(qemu)
