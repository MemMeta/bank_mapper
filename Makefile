LCC=gcc
LCFLAGS= -Wall -O1 -g3
KOBJECT=kam
OBJECT=bank_test bank_test_nomap bank_test_file

all: $(OBJECT) $(KOBJECT)

bank_test: bank_test.c 
	$(LCC) $(LCFLAGS) -o $@ $? -lpthread
bank_test_nomap: bank_test_nomap.c bank_test.h
	$(LCC) $(LCFLAGS) -o $@ bank_test_nomap.c -lpthread
bank_test_file: bank_test_file.c bank_test.h
	$(LCC) $(LCFLAGS) -o $@ bank_test_file.c


obj-m += $(KOBJECT).o

$(KOBJECT):
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f $(OBJECT)
