# The version of SDL we have is too old
# and doesn't provide a proper config file.
# We need to create imported targets for the config

find_package(SDL2 QUIET CONFIG)

find_library(SDL2_LIBRARY
             NAMES
              SDL2
             PATHS
              ${SDL2_LIBDIR}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2 DEFAULT_MSG SDL2_INCLUDE_DIRS SDL2_LIBRARIES SDL2_LIBRARY)
mark_as_advanced(SDL2_LIBRARIES SDL2_LIBRARY)

if (SDL2_FOUND AND NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 UNKNOWN IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES
                        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIRS}"
                        IMPORTED_LOCATION ${SDL2_LIBRARY}
  )
endif()

# Homebrew's SDL2 CMake config on macOS only exposes the inner "include/SDL2"
# directory on the imported target, which breaks source files that use the
# "#include <SDL2/SDL_foo.h>" style. Append the parent include dir so both
# include styles resolve. Scoped to APPLE to leave every other platform's
# FindSDL2 behavior untouched.
if (APPLE AND TARGET SDL2::SDL2)
  get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  set(_sdl2_extra_parents)
  foreach(_dir IN LISTS _sdl2_inc)
    if(_dir MATCHES "/SDL2$")
      get_filename_component(_parent "${_dir}" DIRECTORY)
      list(FIND _sdl2_inc "${_parent}" _already)
      if(_already EQUAL -1)
        list(APPEND _sdl2_extra_parents "${_parent}")
      endif()
    endif()
  endforeach()
  if(_sdl2_extra_parents)
    list(APPEND _sdl2_inc ${_sdl2_extra_parents})
    set_target_properties(SDL2::SDL2 PROPERTIES
                          INTERFACE_INCLUDE_DIRECTORIES "${_sdl2_inc}")
  endif()
  unset(_sdl2_inc)
  unset(_sdl2_extra_parents)
endif()
