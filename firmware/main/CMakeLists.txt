if(CONFIG_ENABLE_STATUS_DISPLAY)
    set(DISPLAY_SRCS "status_display.cpp")
else()
    set(DISPLAY_SRCS "")
endif()

idf_component_register(SRCS "sdm120.cpp" "modbus.cpp" "electrical_power_measurement_delegate.cpp" ${DISPLAY_SRCS} "main.cpp"
                    INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
