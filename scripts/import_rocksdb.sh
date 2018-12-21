#!/bin/sh

# Copies rocksdb source repo from $1 to ~/src/rocksdb, then removes unused
# and unwanted directories.  The general usage is:
#
# $ cd ~/rocksdb
# $ git checkout v1.2.3.4.5  # whatever release you want to import
# $ cd ~/rethinkdb
# $ ./import_rocksdb.sh ~/rocksdb
#
# Then, maybe, inspect the repo to see if there's new extraneous stuff
# we don't want.
#
# Then test if `make -j8 static_lib` works.


# Exit upon error
set -e

# Implicitly checks that we're (probably) in ~/rethinkdb
cd src && cd .. && cd scripts && cd ..

rm -rf rocksdb
cp -R "$1" rocksdb
cd rocksdb
rm -rf .git
rm -r java
rm -r docs
sed -i 's/^\tcd java; $(MAKE) clean$//' Makefile
