// Tests: SHA-256 / SHA-256d against independently computed vectors
// (verified with Python hashlib before they were written here).
#include "crypto/sha256.h"
#include "test_harness.h"

using namespace oeeoo;

void TestCrypto() {
  test::BeginSection("sha256 / sha256d test vectors");
  // SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  CHECK(HexStr(Sha256("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
  CHECK(HexStr(Sha256("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // SHA-256d("") = 5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456
  CHECK(HexStr(Sha256d("")) ==
        "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");
  // SHA-256d("abc") = 4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358
  CHECK(HexStr(Sha256d("abc")) ==
        "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");
  // Determinism: same input, same output; different input, different output.
  CHECK(Sha256d("abc") == Sha256d(std::string("abc")));
  CHECK(Sha256d("abc") != Sha256d("abd"));
  CHECK(Sha256d("") != Sha256d("abc"));
}
