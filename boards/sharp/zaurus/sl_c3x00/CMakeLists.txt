# Copyright (c) 2026 Takayuki Goto
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(board.c)

# Put the encoded updater.sh next to zephyr.bin, so that the two files to copy
# to the card for the Sharp ROM updater sit together in the build directory.
set(ZAURUS_UPDATER_CODEC ${CMAKE_CURRENT_SOURCE_DIR}/../common/support/zaurus_updater_codec.py)
set(ZAURUS_UPDATER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/support/updater.sh)
set(ZAURUS_UPDATER_OUT ${PROJECT_BINARY_DIR}/updater.sh)

add_custom_command(
  OUTPUT ${ZAURUS_UPDATER_OUT}
  COMMAND ${PYTHON_EXECUTABLE} ${ZAURUS_UPDATER_CODEC} ${ZAURUS_UPDATER_SRC} ${ZAURUS_UPDATER_OUT}
  DEPENDS ${ZAURUS_UPDATER_CODEC} ${ZAURUS_UPDATER_SRC}
  COMMENT "Encoding updater.sh for the Sharp ROM updater"
  )
add_custom_target(zaurus_updater ALL DEPENDS ${ZAURUS_UPDATER_OUT})
