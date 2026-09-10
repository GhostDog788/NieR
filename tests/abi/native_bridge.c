#include "abi.h"

/* Build this translation unit as an ordinary native shared library. Its
 * callback invokes code from the application through the platform ABI. */
Pair native_pair_bridge(PairCallback callback, Pair value, int delta) {
    return callback(value, delta);
}

Mixed native_mixed_bridge(MixedCallback callback, Mixed value, int delta) {
    return callback(value, delta);
}

Large native_large_bridge(LargeCallback callback, Large value, int delta) {
    return callback(value, delta);
}
