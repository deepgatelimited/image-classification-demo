add_app(capture main.c)
target_link_libraries(capture PRIVATE display cam serial_link)
