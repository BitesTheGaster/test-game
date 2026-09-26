add_executable(game_tests
  test_game.cpp
  # Diagnostics, not tests: they print a balance measurement and assert one
  # invariant, and they are tagged so `ctest` never picks them up as a suite of
  # their own. Kept in the binary because a balance number you cannot reproduce
  # is a number nobody re-checks after the next change.
  slotdiag.cpp
)
target_link_libraries(game_tests
  PRIVATE
    game_lib
    Catch2::Catch2WithMain
    game_warnings
    game_sanitizers
)

include(Catch)
catch_discover_tests(game_tests)
