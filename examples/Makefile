SUBDIRS = simple stencil
SUBDIRSCLEAN=$(addsuffix clean,$(SUBDIRS))

subdirs: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	make -C simple clean
	make -C stencil clean

.PHONY: $(SUBDIRS) clean
