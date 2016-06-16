all: cplib tests examples

cplib:
	$(MAKE) -C src

tests:
	$(MAKE) -C tests

examples:
	$(MAKE) -C examples

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean
	$(MAKE) -C examples clean
	$(RM) -rf *~

.PHONY: clean cplib tests examples
