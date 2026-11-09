.. zephyr:board:: zaurus_sl_c3200

Overview
********

The Sharp Zaurus SL-C3200 ("Terrier") is the last of the PXA270 clamshell
Zaurus machines, sold in 2006. Its siblings, the SL-C3000 ("Spitz") and the
SL-C3100 ("Borzoi"), differ only in storage; the description shared by the
three lives in ``sl_c3x00.dtsi`` next to this board.

.. list-table::
   :header-rows: 1

   * - Item
     - Detail
   * - SoC
     - Intel PXA270 (XScale, ARMv5TE) at 416 MHz
   * - RAM
     - 64 MB SDRAM at ``0xA0000000``
   * - NAND flash
     - 128 MB: boot loader, service menu, kernel and root filesystem
   * - Microdrive
     - 6 GB, ``/home``
   * - Console
     - FFUART (``0x40100000``), 115200 8N1, on the docking connector and on
       pin 2 of the service connector under the battery cover
   * - Kernel tick
     - PXA OS timer, OSMR0 match, 3.25 MHz
   * - Interrupts
     - PXA2xx interrupt controller, 34 sources
   * - LCD
     - 480x640 portrait TFT, 16 bpp, PXA27x LCD controller; panel power and
       backlight through the timing generator on SSP2 and the second SCOOP
   * - LEDs
     - green and orange, on the first SCOOP
   * - Keyboard
     - 11x7 GPIO matrix (``gpio-kbd-matrix``, polled) with Linux's keymap
   * - I2C
     - general purpose bus (WM8750 codec) and power manager bus (core
       regulator)

Supported Features
******************

.. zephyr:board-supported-hw::

Not supported: the touch screen, the CF/PCMCIA sockets, the SD slot, the NAND
flash, the microdrive, USB, IrDA, audio, power management and GPIO interrupts.

Memory layout
*************

The image is linked to run from SDRAM at ``0xA0008000``, which is where a Linux
``zImage`` is loaded. The bottom 32 kB of bank 0 is left out of the image: the
boot loader keeps its parameter block there.

Exception vectors live at the high vector base ``0xFFFF0000``, because address
0 on a PXA is chip select 0, i.e. read-only flash. The MMU maps a single RAM
page there during boot; see :kconfig:option:`CONFIG_ARMV5_HIGH_VECTORS`.

Building
********

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: zaurus_sl_c3200
   :goals: build

The build product to boot is ``build/zephyr/zephyr.bin``. With
:kconfig:option:`CONFIG_ARM_ZIMAGE_HEADER`, which this board enables, it is a
Linux ARM ``zImage`` that can be loaded anywhere in SDRAM and moves itself to
its link address.

Running under QEMU
******************

QEMU emulates the SL-C3200 as its ``terrier`` machine, but **only up to QEMU
9.1**: all PXA2xx machines were removed in 9.2. The Zephyr SDK and current
distributions ship a newer QEMU, so build 9.1 from source and install it in a
directory of its own, apart from any other QEMU. Building against the
distribution's libfdt keeps QEMU from building its bundled copy:

.. code-block:: console

   sudo apt install libfdt-dev
   wget https://download.qemu.org/qemu-9.1.3.tar.xz
   tar xf qemu-9.1.3.tar.xz
   cd qemu-9.1.3
   ./configure --target-list=arm-softmmu --prefix=/opt/qemu-9.1
   make -j$(nproc)
   sudo make install

Then point ``QEMU_BIN_PATH`` at its ``bin`` directory:

.. code-block:: console

   QEMU_BIN_PATH=/opt/qemu-9.1/bin west build -p always -b zaurus_sl_c3200 samples/hello_world
   west build -t run

A distribution package of QEMU 9.1 or older, such as the 8.2 of Ubuntu 24.04,
works as well.

``QEMU_BIN_PATH`` is read when CMake configures the build. Without it the SDK's
QEMU is used and fails with an unsupported machine type. QEMU is given
``zephyr.bin`` as a ``zImage`` and loads it at SDRAM plus 64 kB, so every run
also goes through the move to the link address.

Running on hardware
*******************

There is no JTAG header and no in-system programmer on a stock SL-C3200, so
there is no ``west flash``. The boot loader starts whatever ``zImage`` is in the
kernel area of the NAND flash, and the Sharp ROM updater writes that area from
a card. ``zephyr.bin`` takes the place of the Linux kernel there.

.. warning::

   This replaces the Sharp Linux kernel in NAND. Take the service menu NAND
   backup first: hold :kbd:`D` and :kbd:`M` while inserting the battery and
   powering on, then page 3/3, ``BACKUP RESTORE MENU``, item 1. On this model the
   backup covers the system, and ``NAND Flash Restore`` in the same menu brings
   it back. The boot loader and the menus are not touched, so they stay
   reachable whatever the kernel area holds.

#. Build the application as above. Next to ``zephyr.bin``, the build puts
   ``updater.sh``, the update script in the encoding the ROM updater requires:

   .. code-block:: none

      build/zephyr/zephyr.bin
      build/zephyr/updater.sh

#. Copy the two files as they are to the root of a FAT formatted SD or CF card
   of 2 GB or smaller, with a file manager or any other way. ``updater.sh``
   must keep its name; ``zephyr.bin`` may also be named ``zImage``.

   ``boards/sharp/zaurus/common/support/zaurus_updater_codec.py`` produces the
   encoded script from ``support/updater.sh`` by hand, and decodes one with
   ``-d``.

#. Power off, plug in the charger, then hold :kbd:`OK` and press :kbd:`On/Off`.
   In the update menu pick the update item, then the card.

#. The script checks the model, the size of ``zephyr.bin`` against the 1.2 MB
   kernel area and its ``zImage`` header, writes it and, where the updater has
   ``cmp``, reads it back to compare. Reset the machine when it reports
   ``Done``.

The console is on FFUART, inverted 3.3 V levels on the docking connector (TX on
GPIO99, RX on GPIO102) and non-inverted TX on pin 2 of the service connector
(GPIO83). The board routes both at boot.
