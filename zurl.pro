TEMPLATE = subdirs

sub_libzurl.subdir = src/pro/libzurl
sub_zurl.subdir = src/pro/zurl
sub_zurl.depends = sub_libzurl
sub_tests.subdir = tests
sub_tests.depends = sub_tests

SUBDIRS += \
	sub_libzurl \
	sub_zurl \
	sub_tests
