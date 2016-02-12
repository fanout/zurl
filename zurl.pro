TEMPLATE = subdirs

sub_libzurl.subdir = src/libzurl
sub_zurl.subdir = src/zurl
sub_zurl.depends = sub_libzurl
sub_tests.subdir = tests
sub_tests.depends = sub_libzurl

sub_tests.CONFIG += no_default_install

SUBDIRS += \
	sub_libzurl \
	sub_zurl \
	sub_tests
