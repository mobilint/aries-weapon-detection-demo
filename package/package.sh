if [[ $# -lt 2 ]]; then
   echo "Usage: $0 <PRODUCT> <DRIVER_TYPE>" >&2
   exit 1
fi
readonly PRODUCT="$1"
readonly DRIVER_TYPE="$2"

readonly BUILD_DIR=build_package
readonly PACKAGE_DIR="demo-package-${PRODUCT}"
cd "$(git rev-parse --show-toplevel)"

# build for so / a file
mkdir $BUILD_DIR
cd $BUILD_DIR
cmake -DVENDOR=mobilint -DPRODUCT="${PRODUCT}" -DDRIVER_TYPE="${DRIVER_TYPE}" -DCMAKE_BUILD_TYPE=Release .. && \
make qbruntime -j && make yaml-cpp -j
cd ..

# package
mkdir -p $PACKAGE_DIR/yaml-cpp/lib
mkdir -p $PACKAGE_DIR/qbruntime/lib

cp -r mxq rc src package/Makefile package/README.md $PACKAGE_DIR

cp -r $BUILD_DIR/_deps/yaml-cpp-src/include $PACKAGE_DIR/yaml-cpp
cp -r $BUILD_DIR/_deps/yaml-cpp-build/libyaml-cpp.a $PACKAGE_DIR/yaml-cpp/lib

cp -r $BUILD_DIR/_deps/qbruntime-src/include $PACKAGE_DIR/qbruntime
cp -r $BUILD_DIR/_deps/qbruntime-build/src/qbruntime/libqbruntime.so* $PACKAGE_DIR/qbruntime/lib

tar -czvf $PACKAGE_DIR.tar.gz $PACKAGE_DIR
rm -rf $PACKAGE_DIR
rm -rf $BUILD_DIR