# Interprocedural optimization (LTO), enabled explicitly via GAME_LTO (Release preset).

option(GAME_LTO "Enable interprocedural optimization (LTO)" OFF)

add_library(game_lto INTERFACE)

if(GAME_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
  if(IPO_SUPPORTED)
    target_link_options(game_lto INTERFACE -flto)
    message(STATUS "LTO enabled")
  else()
    message(WARNING "LTO requested but not supported: ${IPO_ERROR}")
  endif()
endif()
