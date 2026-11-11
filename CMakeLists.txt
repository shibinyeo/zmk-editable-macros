# Copyright (c) 2026 The zmk-editable-macros contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_BEHAVIOR_EDITABLE_MACRO)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

  target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/behavior_editable_macro.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/em_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/em_player.c
  )

  target_sources_ifdef(CONFIG_SETTINGS app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/em_settings.c)

  target_sources_ifdef(CONFIG_ZMK_EDITABLE_MACRO_STUDIO_RPC app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/em_rpc.c)

  target_sources_ifdef(CONFIG_ZMK_EDITABLE_MACRO_TEST_RPC app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/em_test_rpc.c)

  target_sources_ifdef(CONFIG_ZMK_EDITABLE_MACRO_TEST_SEED app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/em_test_seed.c)

  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_EDITABLE_MACRO_TEST app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/behavior_editable_macro_test.c)
endif()
