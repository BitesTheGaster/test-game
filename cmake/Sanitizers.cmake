# Sanitizer presets: GAME_ASAN (Address+UB), GAME_TSAN (Thread)

option(GAME_ASAN "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(GAME_TSAN "Enable ThreadSanitizer" OFF)

if(GAME_ASAN AND GAME_TSAN)
  message(FATAL_ERROR "GAME_ASAN and GAME_TSAN are mutually exclusive")
endif()

add_library(game_sanitizers INTERFACE)

if(GAME_ASAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(game_sanitizers INTERFACE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
      -fno-sanitize-recover=undefined
    )
    target_link_options(game_sanitizers INTERFACE
      -fsanitize=address,undefined
    )
  else()
    message(WARNING "GAME_ASAN requested but compiler does not support it; ignored")
  endif()
endif()

if(GAME_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(game_sanitizers INTERFACE
      -fsanitize=thread
      -fno-omit-frame-pointer
    )
    target_link_options(game_sanitizers INTERFACE
      -fsanitize=thread
    )
  else()
    message(WARNING "GAME_TSAN requested but compiler does not support it; ignored")
  endif()
endif()
