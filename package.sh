#!/bin/sh
set -e

if [ $# -lt 1 ]; then
	echo "usage: $0 [version]"
	exit 1
fi

VERSION=$1

mkdir -p build/zurl-$VERSION
cp -a .gitignore COPYING README configure zurl.qc zurl.pro zurl.conf.example qcm src tools tests build/zurl-$VERSION
cd build
tar jcvf zurl-$VERSION.tar.bz2 zurl-$VERSION
