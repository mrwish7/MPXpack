add_executable(test_block_sizing test_block_sizing.cpp)
target_link_libraries(test_block_sizing PRIVATE mpx_common)
add_test(NAME test_block_sizing COMMAND test_block_sizing)

add_executable(test_encode_decode_roundtrip test_encode_decode_roundtrip.cpp)
target_link_libraries(test_encode_decode_roundtrip PRIVATE mpx_common)
add_test(NAME test_encode_decode_roundtrip COMMAND test_encode_decode_roundtrip)

add_executable(test_lossless_roundtrip test_lossless_roundtrip.cpp)
target_link_libraries(test_lossless_roundtrip PRIVATE mpx_common)
add_test(NAME test_lossless_roundtrip COMMAND test_lossless_roundtrip)

add_executable(test_loopback_udp test_loopback_udp.cpp)
target_link_libraries(test_loopback_udp PRIVATE mpx_common)
add_test(NAME test_loopback_udp COMMAND test_loopback_udp)

add_executable(test_resampler_passband test_resampler_passband.cpp)
target_link_libraries(test_resampler_passband PRIVATE mpx_common)
add_test(NAME test_resampler_passband COMMAND test_resampler_passband)

add_executable(test_udp_reassembler_resync test_udp_reassembler_resync.cpp)
target_link_libraries(test_udp_reassembler_resync PRIVATE mpx_common)
add_test(NAME test_udp_reassembler_resync COMMAND test_udp_reassembler_resync)

add_executable(test_decoder_late_start test_decoder_late_start.cpp)
target_link_libraries(test_decoder_late_start PRIVATE mpx_common)
add_test(NAME test_decoder_late_start COMMAND test_decoder_late_start)

# Not registered with add_test(): needs a real Icecast server + credentials
# passed as arguments, so it's not something ctest can run unattended.
add_executable(test_icecast_live test_icecast_live.cpp)
target_link_libraries(test_icecast_live PRIVATE mpx_common)

# Not registered with add_test(): needs a 192kHz-native capture+playback
# device pair (e.g. VB-Audio Hi-Fi Cable) — see file header. SenderApp/
# ReceiverApp live in the mpx_sender/mpx_receiver executables, not
# mpx_common, so their sources are added directly here rather than linked.
add_executable(test_udp_live_loopback
    test_udp_live_loopback.cpp
    ${CMAKE_SOURCE_DIR}/src/sender/sender_app.cpp
    ${CMAKE_SOURCE_DIR}/src/receiver/receiver_app.cpp
)
target_include_directories(test_udp_live_loopback PRIVATE
    ${CMAKE_SOURCE_DIR}/src/sender
    ${CMAKE_SOURCE_DIR}/src/receiver
)
target_link_libraries(test_udp_live_loopback PRIVATE mpx_common)

# Diagnostic tool, not a pass/fail test — see file header.
add_executable(probe_audio_devices probe_audio_devices.cpp)
target_link_libraries(probe_audio_devices PRIVATE mpx_common)

# Diagnostic tool, not a pass/fail test — see file header. No confirmed
# hard SNR threshold for RDS decodability, so this reports numbers rather
# than asserting pass/fail.
add_executable(test_rds_snr test_rds_snr.cpp)
target_link_libraries(test_rds_snr PRIVATE mpx_common)

# Diagnostic tool, not a pass/fail test — see file header.
add_executable(test_quality_flags test_quality_flags.cpp)
target_link_libraries(test_quality_flags PRIVATE mpx_common)

# Diagnostic tool, not a pass/fail test — see file header.
add_executable(test_noise_floor_spectrum test_noise_floor_spectrum.cpp)
target_link_libraries(test_noise_floor_spectrum PRIVATE mpx_common)

# Diagnostic tool, not a pass/fail test — see file header.
add_executable(test_noise_floor_by_rate test_noise_floor_by_rate.cpp)
target_link_libraries(test_noise_floor_by_rate PRIVATE mpx_common)

# Diagnostic tool, not a pass/fail test — see file header. Takes a real
# captured MPX WAV file path as argv[1].
add_executable(probe_real_mpx_resample probe_real_mpx_resample.cpp)
target_link_libraries(probe_real_mpx_resample PRIVATE mpx_common)

# Diagnostic tool — see file header. Needs a loopback device pair (e.g.
# VB-Audio Hi-Fi Cable).
add_executable(probe_output_channel_select probe_output_channel_select.cpp)
target_link_libraries(probe_output_channel_select PRIVATE mpx_common)

# Diagnostic tool — see file header. Needs a loopback device pair (e.g.
# VB-Audio Hi-Fi Cable). Windows only (MME is a no-op elsewhere).
add_executable(probe_mme_resample_check probe_mme_resample_check.cpp)
target_link_libraries(probe_mme_resample_check PRIVATE mpx_common)
