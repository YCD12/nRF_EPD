CC := gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O2

ROOT := ..
UC8179_TEST := /tmp/uc8179_command_sequence_test
CLOCK_DIFF_TEST := /tmp/epd_clock_diff_test
UC8179_SOURCES := uc8179_command_sequence_test.c $(ROOT)/EPD/UC8179.c
CLOCK_DIFF_SOURCES := epd_clock_diff_test.c $(ROOT)/EPD/epd_clock_diff.c
TEST_STUBS := $(wildcard stubs/*.h)

.PHONY: test clean

test: $(UC8179_TEST) $(CLOCK_DIFF_TEST)
	$(UC8179_TEST)
	$(CLOCK_DIFF_TEST)

$(UC8179_TEST): $(UC8179_SOURCES) $(ROOT)/EPD/UC8179.h \
                 $(ROOT)/EPD/EPD_driver.h $(TEST_STUBS)
	$(CC) $(CFLAGS) -I stubs -I $(ROOT)/EPD $(UC8179_SOURCES) -o $@

$(CLOCK_DIFF_TEST): $(CLOCK_DIFF_SOURCES) $(ROOT)/EPD/epd_clock_diff.h \
                    $(ROOT)/EPD/EPD_driver.h $(ROOT)/GUI/GUI.h \
                    $(ROOT)/GUI/ui_runtime.h $(TEST_STUBS)
	$(CC) $(CFLAGS) -I stubs -I $(ROOT)/EPD -I $(ROOT)/GUI \
		$(CLOCK_DIFF_SOURCES) -o $@

clean:
	rm -f $(UC8179_TEST) $(CLOCK_DIFF_TEST)
