LCC=gcc
LCFLAGS= -Wall -O1 -g3
KOBJECT=kam
OBJECT=bank_test

all: $(OBJECT) $(KOBJECT)

bank_test: bank_test.c
	$(LCC) $(LCFLAGS) -o $@ $? -lpthread


obj-m += $(KOBJECT).o

$(KOBJECT):
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f $(OBJECT)
