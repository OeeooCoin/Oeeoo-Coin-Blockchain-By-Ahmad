#include "crypto/address.h"

#include "consensus/consensus.h"
#include "crypto/base58.h"

namespace oeeoo {

std::string PubKeyHashToAddress(const Hash160& hash160) {
  const std::vector<unsigned char> payload(hash160.begin(), hash160.end());
  return Base58CheckEncode(P2PKH_VERSION_BYTE, payload);
}

std::string PubKeyToAddress(const std::vector<unsigned char>& pubkey) {
  return PubKeyHashToAddress(ComputeHash160(pubkey));
}

bool DecodeP2PKHAddress(const std::string& address, Hash160& hash160) {
  unsigned char version = 0;
  std::vector<unsigned char> payload;
  if (!Base58CheckDecode(address, version, payload)) return false;
  if (version != P2PKH_VERSION_BYTE) return false;
  if (payload.size() != 20) return false;
  for (std::size_t i = 0; i < 20; ++i) hash160[i] = payload[i];
  return true;
}

}  // namespace oeeoo
