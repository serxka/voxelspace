OBJS=voxelspace.o
CFLAGS=-O2

all: voxelspace

voxelspace: stb_image.h ${OBJS}
	${CC} ${CFLAGS} -lSDL2 -lm -o $@ ${OBJS}
	strip $@

stb_image.h:
	@echo downloading "stb_image.h" from source
	curl https://raw.githubusercontent.com/nothings/stb/master/stb_image.h > stb_image.h

clean:
	rm -f ${OBJS}
	rm -f voxelspace

.PHONY := all clean
