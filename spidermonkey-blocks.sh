# Script to copy the spidermonkey folder and dependencies from gecko-dev repo (https://github.com/mozilla/gecko-dev)
# need to clone at the same folder

###################
# Copy spidermonkey
###################
cp -r ../gecko-dev/js ./
cp -r ../gecko-dev/configure.py ./
cp -r ../gecko-dev/python/ ./
cp -r ../gecko-dev/moz.configure ./
cp -r ../gecko-dev/configure.in ./
cp -r ../gecko-dev/build ./
cp -r ../gecko-dev/testing ./
cp -r ../gecko-dev/config ./

rm -r build_OPT.OBJ
rm -r js/src/configure
rm -r js/src/old-configure

######################
# Compile Spidermonkey
######################
cd js/src
autoconf2.13

# This name should end with "_OPT.OBJ" to make the version control system ignore it.
mkdir build_OPT.OBJ
cd build_OPT.OBJ
../configure

# Use "mozmake" on Windows
make
