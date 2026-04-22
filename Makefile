CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -finput-charset=UTF-8 -municode
LIBS := -lsetupapi -lhid -lshell32 -lgdi32

SRC := keychron_battery_display.c
TARGET := keychron_battery_display.exe

.PHONY: all clean run probe

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -mwindows -o $@ $< $(LIBS)

run: $(TARGET)
	./$(TARGET)

probe: $(TARGET)
	./$(TARGET) --probe

clean:
	cmd /C "if exist $(TARGET) del /Q $(TARGET)"
	cmd /C "if exist keychron_battery_probe.exe del /Q keychron_battery_probe.exe"
