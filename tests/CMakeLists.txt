add_executable(game_tests
  test_game.cpp
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
