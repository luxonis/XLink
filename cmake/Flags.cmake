## setup compilation flags
# conditionally applies flag. If flag is supported by current compiler, it will be added to compile options.
include(CheckCCompilerFlag)
function(add_flag target flag)
    check_c_compiler_flag(${flag} FLAG_${flag})
    if (FLAG_${flag} EQUAL 1)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${flag}>)
    endif ()
endfunction()

function(add_flag_source source flag)
    check_c_compiler_flag(${flag} FLAG_${flag})
    if (FLAG_${flag} EQUAL 1)
        set_source_files_properties(${source} PROPERTIES COMPILE_FLAGS "-Werror=switch-enum")
    endif ()
endfunction()

function(add_default_flags target)
    if ("${CMAKE_C_COMPILER_ID}" MATCHES "^(AppleClang|Clang|GNU)$")
        # enable those flags
        # add_flag(${target} -Woverloaded-virtual)     # warn if you overload (not override) a virtual function
        add_flag(${target} -Wformat=2)               # warn on security issues around functions that format output (ie printf)
        add_flag(${target} -Wmisleading-indentation) # (only in GCC >= 6.0) warn if indentation implies blocks where blocks do not exist
        add_flag(${target} -Wduplicated-cond)        # (only in GCC >= 6.0) warn if if / else chain has duplicated conditions
        add_flag(${target} -Wduplicated-branches)    # (only in GCC >= 7.0) warn if if / else branches have duplicated code
        add_flag(${target} -Wnull-dereference)       # (only in GCC >= 6.0) warn if a null dereference is detected
        add_flag(${target} -Wsign-compare)
        add_flag(${target} -Wtype-limits)            # size_t - size_t >= 0 -> always true

        # disable those flags
        add_flag(${target} -Wno-unused-command-line-argument)    # clang: warning: argument unused during compilation: '--coverage' [-Wunused-command-line-argument]
        add_flag(${target} -Wno-unused-parameter)    # prints too many useless warnings
        add_flag(${target} -Wno-format-nonliteral)   # prints way too many warnings from spdlog
        add_flag(${target} -Wno-gnu-zero-variadic-macro-arguments)   # https://stackoverflow.com/questions/21266380/is-the-gnu-zero-variadic-macro-arguments-safe-to-ignore

        # promote to errors
        add_flag(${target} -Werror=self-assign-field)  # error if self assign - bugprone
        add_flag(${target} -Werror=unused-lambda-capture)  # error if lambda capture is unused
        add_flag(${target} -Werror=return-type)      # warning: control reaches end of non-void function [-Wreturn-type]
        # add_flag(${target} -Werror=non-virtual-dtor) # warn the user if a class with virtual functions has a non-virtual destructor. This helps catch hard to track down memory errors
        add_flag(${target} -Werror=sign-compare)     # warn the user if they compare a signed and unsigned numbers
        # add_flag(${target} -Werror=reorder)          # field '$1' will be initialized after field '$2'

    elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # using Visual Studio C++
        # TODO(warchant): add flags https://github.com/lefticus/cppbestpractices/blob/master/02-Use_the_Tools_Available.md#msvc
    endif()

endfunction()