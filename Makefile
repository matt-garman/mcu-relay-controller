# vim: tw=0 nowrap

dummy:
	gcc -Wall -ggdb3 -Os -DIMPL_DUMMY mcu-relay-controller.c dummy.c

CORE_SRC      := mcu-relay-controller-iface.h  mcu-relay-controller.c
ATTINY_SRC    := $(CORE_SRC) attiny.c
PIC12F675_SRC := $(CORE_SRC) pic12f675.c
PIC10F320_SRC := $(CORE_SRC) pic10f320.c

attiny13: $(ATTINY_SRC)
	avr-gcc -Os -std=gnu99 -DIMPL_ATTINY -DATTINY13 -DF_CPU=1000000UL -mmcu=attiny13 -o attiny13.elf mcu-relay-controller.c attiny.c
	avr-objcopy -j .text -j .data -O ihex attiny13.elf attiny13.hex

attiny85: $(ATTINY_SRC)
	avr-gcc -Os -std=gnu99 -DIMPL_ATTINY -DF_CPU=1000000UL -mmcu=attiny85 -o attiny85.elf mcu-relay-controller.c attiny.c
	avr-objcopy -j .text -j .data -O ihex attiny85.elf attiny85.hex

pic12f675: $(PIC12F675_SRC)
	xc8-cc -mcpu=pic12f675 -Os -DIMPL_PIC12F675 mcu-relay-controller.c pic12f675.c

pic10f320: $(PIC10F320_SRC)
	xc8-cc -mcpu=pic10f320 -Os -DIMPL_PIC10F320 mcu-relay-controller.c pic10f320.c

all: dummy attiny13 attiny85 pic12f675 pic10f320

clean:
	rm -f *.elf *.hex *.hxl *.o *.s *.p1 *.sdb *.sym *.cmf *.lst *.rlf *.d *~ a.out

