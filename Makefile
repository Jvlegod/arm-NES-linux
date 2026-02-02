# CC=/home/jvle/Desktop/works/Embedded/rk3506_linux6.1_sdk_v1.2.0/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-gcc
# SYSROOT=/home/jvle/Desktop/works/Embedded/rk3506_linux6.1_sdk_v1.2.0/buildroot/output/rockchip_hd_rk3506g_evm_nand/host/arm-buildroot-linux-gnueabihf/sysroot/
TARBALL = InfoNES08J

# InfoNES
.CFILES =	./K6502.cpp \
		./InfoNES.cpp \
		./InfoNES_Mapper.cpp \
		./InfoNES_pAPU.cpp \
		./InfoNES_System_Linux.cpp

.OFILES	=	$(.CFILES:.cpp=.o)

# CCFLAGS =  -O2 -fsigned-char 
# LDFILGS = -lstdc++		# gcc3.x.x
CCFLAGS = -O2 -fsigned-char --sysroot=$(SYSROOT)
LDFILGS = --sysroot=$(SYSROOT) -lstdc++ -lm -lz -lpthread -lasound

all: InfoNES

InfoNES: $(.OFILES)
	$(CC) $(INCLUDES) -o $@ $(.OFILES) $(LDFILGS) -lm -lz -lpthread -lasound

.cpp.o:
	$(CC) $(INCLUDES) -c $(CCFLAGS) $*.cpp  -o $@

clean:
	rm -f $(.OFILES) ../*~ ../*/*~ core

cleanall:
	rm -f $(.OFILES) ../*~ ../*/*~ core InfoNES

release: clean all

tar:
	( cd ..; \
	tar cvf $(TARBALL).tar ./*; \
	gzip $(TARBALL).tar \
	)

install:
	install ./InfoNES /usr/local/bin
