# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

function(generate_update_keys target_name update_loc)
    set(gen_dst ${CMAKE_CURRENT_BINARY_DIR}/gen)
    set(header_name update_keys_data.h)
    if (ARGC GREATER 2)
        string(APPEND gen_dst "/${ARGV2}")
        set(header_name fork_update_keys_data.h)
    endif()
    file(MAKE_DIRECTORY ${gen_dst})

    set(gen_files ${gen_dst}/${header_name})
    set(gen_script ${CMAKE_CURRENT_SOURCE_DIR}/cmake/update_keys_header.cmake)

    add_custom_command(
    OUTPUT
        ${gen_files}
    COMMAND
        ${CMAKE_COMMAND}
        -Droot_pem=${update_loc}/root-public.pem
        -Dmanifest=${update_loc}/manifest.min.json
        -Dmanifest_sig=${update_loc}/manifest.sig
        -Doutput=${gen_files}
        -P ${gen_script}
    COMMENT "Generating update keys header (${target_name})"
    DEPENDS
        ${gen_script}
        ${update_loc}/root-public.pem
        ${update_loc}/manifest.min.json
        ${update_loc}/manifest.sig
    VERBATIM
    )
    generate_target(${target_name} update_keys ${gen_files} "${gen_files}" ${gen_dst})
endfunction()
