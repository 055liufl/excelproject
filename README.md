# dbridge

Qt+SQLite+Excel batch import/export C++ dynamic library.

## Build

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH=/opt/Qt5.12.12/5.12.12/gcc_64
cmake --build build
```

## Test

```bash
cd build && ctest --output-on-failure
```

## Design

See `specs/` directory for design documents.
