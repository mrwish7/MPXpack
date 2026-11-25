add_library(mpx_common STATIC
    app_config.h
    net_common.h
    net_common.cpp
    tcp_socket.h
    tcp_socket.cpp
    udp_socket.h
    udp_socket.cpp
    udp_framing.h
    udp_framing.cpp
    base64.h
    base64.cpp
    byte_ring_buffer.h
    byte_ring_buffer.cpp
    block_queue.h
    wavpack_encoder.h
    wavpack_encoder.cpp
    wavpack_decoder.h
    wavpack_decoder.cpp
    audio_device.h
    audio_device.cpp
    icecast_source_client.h
    icecast_source_client.cpp
    icecast_relay_client.h
    icecast_relay_client.cpp
    synthetic_signal.h
    synthetic_signal.cpp
    resampler.h
    resampler.cpp
    config_store.h
    config_store.cpp
    app_paths.h
    app_paths.cpp
    win_timer_resolution.h
    win_timer_resolution.cpp
)

target_include_directories(mpx_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mpx_common PUBLIC wavpack miniaudio SampleRate::samplerate)

if(WIN32)
    target_link_libraries(mpx_common PUBLIC ws2_32 winmm)
endif()
