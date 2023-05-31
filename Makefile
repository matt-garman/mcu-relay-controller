# vim: tw=0 nowrap

dummy:
	gcc -Wall -ggdb3 -Os -DIMPL_DUMMY mcu-relay-controller.c dummy.c

attiny13:
	avr-gcc -Os -std=gnu99 -DIMPL_ATTINY -DATTINY13 -DF_CPU=1000000UL -mmcu=attiny13 -o attiny13.elf mcu-relay-controller.c attiny.c

attiny85:
	avr-gcc -Os -std=gnu99 -DIMPL_ATTINY -DF_CPU=1000000UL -mmcu=attiny85 -o attiny85.elf mcu-relay-controller.c attiny.c

pic12f675:
	xc8-cc -mcpu=pic12f675 -Os -DIMPL_PIC12F675 pic12f675.c  mcu-relay-controller.c

pic10f320:
	xc8-cc -mcpu=pic10f320 -Os -DIMPL_PIC10F320 pic10f320.c  mcu-relay-controller.c

all: dummy attiny13 attiny85 pic12f675 pic10f320

clean:
	rm -f *.elf *.hex *.hxl *.o *.s *.p1 *.sdb *.sym *.cmf *.lst *.rlf *.d *~ a.out

