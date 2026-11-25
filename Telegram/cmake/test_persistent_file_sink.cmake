# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

add_executable(test_persistent_file_sink)
init_non_host_target(test_persistent_file_sink "(tests)")

target_include_directories(test_persistent_file_sink PRIVATE ${src_loc})
nice_target_sources(test_persistent_file_sink ${src_loc}
PRIVATE
    media/streaming/media_streaming_download_attachment.h
    storage/persistent_file_sink.cpp
    storage/persistent_file_sink.h
    storage/download_presentation.h
    storage/pending_downloads.cpp
    storage/pending_downloads.h
    tests/test_persistent_file_sink.cpp
)

target_link_libraries(test_persistent_file_sink PRIVATE desktop-app::external_qt)

set_target_properties(test_persistent_file_sink PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    EXCLUDE_FROM_DEFAULT_BUILD TRUE
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_persistent_file_sink
)
