#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [version]"
	exit 1
fi

VERSION=$1

mkdir -p build/zurl-$VERSION
cp -a .gitignore CHANGELOG.md COPYING README.md configure zurl.qc zurl.pro zurl.conf.example qcm src tools tests build/zurl-$VERSION
rm -rf build/zurl-$VERSION/src/qzmq/.git build/zurl-$VERSION/src/common/.git build/zurl-$VERSION/src/jdns/.git
cd build
tar jcvf zurl-$VERSION.tar.bz2 zurl-$VERSION
