# Optional integration: append `include tests/wifi-tests.mk` to the main
# Makefile, or run `make -f Makefile -f tests/wifi-tests.mk wifi-service-test`.
# Do not also compile src/services/wifi.c: test_wifi_service.c includes it.
.PHONY: wifi-service-test
$(BUILD_DIR)/host-wifi-service-tests: tests/test_wifi_service.c src/services/wifi.c src/services/wifi.h src/core/status.h | $(BUILD_DIR)
	$(HOST_CC) $(CPPFLAGS) $(COMMON_CFLAGS) tests/test_wifi_service.c -o $@

wifi-service-test: $(BUILD_DIR)/host-wifi-service-tests
	$(BUILD_DIR)/host-wifi-service-tests

# Parent integration may additionally add: host-test: wifi-service-test
