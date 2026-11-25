add_executable(mpx_sender
    main.cpp
    sender_app.h
    sender_app.cpp
    sender_gui.h
    sender_gui.cpp
)

target_link_libraries(mpx_sender PRIVATE mpx_common imgui)

if(WIN32)
    set_target_properties(mpx_sender PROPERTIES WIN32_EXECUTABLE FALSE) # keep the console for log output during development
endif()
