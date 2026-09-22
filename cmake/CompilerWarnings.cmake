# Target-based warnings: attach via target_link_libraries(<tgt> PRIVATE game_warnings)

add_library(game_warnings INTERFACE)

target_compile_options(game_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wdouble-promotion
    -Wformat=2
  >
  $<$<CXX_COMPILER_ID:MSVC>:
    /W4
    /permissive-
    /w14265
    /w14062
  >
)

# In CI treat warnings as errors: add -Werror / /WX via GAME_WERROR
option(GAME_WERROR "Treat warnings as errors" OFF)
if(GAME_WERROR)
  target_compile_options(game_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/WX>
  )
endif()
