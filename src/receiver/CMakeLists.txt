add_executable(mpx_receiver
    main.cpp
    receiver_app.h
    receiver_app.cpp
    receiver_gui.h
    receiver_gui.cpp
)

target_link_libraries(mpx_receiver PRIVATE mpx_common imgui)
